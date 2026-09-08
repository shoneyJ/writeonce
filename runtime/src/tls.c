/* tls.c — hand-rolled TLS 1.3 (runtime-v2 9 phase F).
 *
 * Phase F1: the record layer (RFC 8446 §5.2). A TLS 1.3 record protects
 *
 *     TLSInnerPlaintext = content || content_type(1) || zero padding
 *
 * with an AEAD whose additional data is the 5-byte record header and whose
 * nonce is the static write IV XOR'd with the 64-bit record sequence number,
 * big-endian, left-padded to 12 bytes (§5.3). The outer opaque_type is always
 * application_data (23) once traffic is protected; the real type is the last
 * non-zero byte of the decrypted inner plaintext.
 *
 * The AEAD itself is phase A (crypto.h): AES-128-GCM or ChaCha20-Poly1305,
 * selected by the negotiated cipher suite. This file adds only the framing. */

#include <stdlib.h>
#include <string.h>

#include "tls.h"
#include "crypto.h"

/* Build the per-record nonce: iv XOR (seq as a big-endian 64-bit value in the
 * low 8 bytes). RFC 8446 §5.3. */
static void record_nonce(const uint8_t iv[12], uint64_t seq, uint8_t nonce[12]) {
    memcpy(nonce, iv, 12);
    for (int i = 0; i < 8; i++)
        nonce[4 + i] ^= (uint8_t)(seq >> (8 * (7 - i)));
}

/* AEAD seal/open dispatch by suite. aad is the 5-byte header. seal writes
 * ct||tag into out (inner_len + 16 bytes); open reads ct||tag from in. */
static int aead_seal(int suite, const uint8_t *key, size_t keylen,
                     const uint8_t nonce[12], const uint8_t *aad, size_t aadlen,
                     const uint8_t *pt, size_t ptlen, uint8_t *out) {
    if (suite == WO_TLS_AES_128_GCM_SHA256)
        return wo_aes_gcm_seal(key, keylen, nonce, aad, aadlen, pt, ptlen, out);
    if (suite == WO_TLS_CHACHA20_POLY1305_SHA256)
        return wo_chacha20poly1305_seal(key, nonce, aad, aadlen, pt, ptlen, out);
    return -1;
}
static int aead_open(int suite, const uint8_t *key, size_t keylen,
                     const uint8_t nonce[12], const uint8_t *aad, size_t aadlen,
                     const uint8_t *ct, size_t ctlen, const uint8_t tag[16],
                     uint8_t *out) {
    if (suite == WO_TLS_AES_128_GCM_SHA256)
        return wo_aes_gcm_open(key, keylen, nonce, aad, aadlen, ct, ctlen, tag, out);
    if (suite == WO_TLS_CHACHA20_POLY1305_SHA256)
        return wo_chacha20poly1305_open(key, nonce, aad, aadlen, ct, ctlen, tag, out);
    return -1;
}

int wo_tls_record_seal(int suite, const uint8_t *key, size_t keylen,
                       const uint8_t iv[12], uint64_t seq, uint8_t content_type,
                       const uint8_t *pt, size_t ptlen, uint8_t *out) {
    if (suite != WO_TLS_AES_128_GCM_SHA256 &&
        suite != WO_TLS_CHACHA20_POLY1305_SHA256)
        return -1;

    size_t inner_len = ptlen + 1;               /* content || content_type */
    size_t payload_len = inner_len + 16;        /* + AEAD tag */
    /* 5-byte header: application_data, legacy 0x0303, payload length. */
    out[0] = WO_TLS_CT_APPLICATION_DATA;
    out[1] = 0x03; out[2] = 0x03;
    out[3] = (uint8_t)(payload_len >> 8);
    out[4] = (uint8_t)payload_len;

    /* Assemble the inner plaintext (no padding) in a scratch buffer. */
    uint8_t stackbuf[512];
    uint8_t *inner = inner_len <= sizeof stackbuf ? stackbuf
                                                  : (uint8_t *)malloc(inner_len);
    if (!inner) return -1;
    memcpy(inner, pt, ptlen);
    inner[ptlen] = content_type;

    uint8_t nonce[12];
    record_nonce(iv, seq, nonce);
    /* AEAD writes ct(inner_len) || tag(16) straight after the header. */
    int rc = aead_seal(suite, key, keylen, nonce, out, 5, inner, inner_len,
                       out + 5);
    if (inner != stackbuf) free(inner);
    if (rc != 0) return -1;
    return (int)(5 + payload_len);
}

int wo_tls_record_open(int suite, const uint8_t *key, size_t keylen,
                       const uint8_t iv[12], uint64_t seq, const uint8_t *rec,
                       size_t reclen, uint8_t *out, uint8_t *content_type) {
    if (suite != WO_TLS_AES_128_GCM_SHA256 &&
        suite != WO_TLS_CHACHA20_POLY1305_SHA256)
        return -1;
    if (reclen < 5 + 16) return -1;             /* header + at least a tag */
    size_t payload_len = ((size_t)rec[3] << 8) | rec[4];
    if (payload_len + 5 != reclen || payload_len < 16) return -1;

    size_t inner_len = payload_len - 16;
    const uint8_t *ct = rec + 5;
    const uint8_t *tag = rec + 5 + inner_len;

    uint8_t nonce[12];
    record_nonce(iv, seq, nonce);
    /* additional data is the 5-byte header, verbatim. */
    if (aead_open(suite, key, keylen, nonce, rec, 5, ct, inner_len, tag, out) != 0)
        return -1;

    /* Strip trailing zero padding; the last non-zero byte is the content type. */
    size_t n = inner_len;
    while (n > 0 && out[n - 1] == 0) n--;
    if (n == 0) return -1;                       /* all-zero: no content type */
    *content_type = out[n - 1];
    return (int)(n - 1);
}

/* ---- key schedule (phase F2, RFC 8446 §7.1) -----------------------------
 * Derive-Secret(Secret, Label, Messages)
 *     = HKDF-Expand-Label(Secret, Label, Transcript-Hash(Messages), Hash.len)
 * with Hash = SHA-256 for the suites we implement. The whole schedule is a
 * chain of HKDF-Extract and Derive-Secret over the phase-B primitives. */

/* Transcript hash of the empty message list: SHA-256(""). */
static void empty_hash(uint8_t out[32]) { wo_sha256((const uint8_t *)"", 0, out); }

/* Derive-Secret with an explicit 32-byte transcript hash. */
static void derive_secret(const uint8_t secret[32], const char *label,
                          const uint8_t transcript[32], uint8_t out[32]) {
    wo_hkdf_sha256_expand_label(secret, label, strlen(label), transcript, 32,
                                out, 32);
}

void wo_tls_derive_handshake(wo_tls_key_schedule *ks, const uint8_t *ecdhe,
                             size_t ecdhe_len, const uint8_t hash_ch_sh[32]) {
    uint8_t zeros[32] = {0}, eh[32], early[32], derived[32];
    empty_hash(eh);

    /* Early Secret = HKDF-Extract(0, 0) (no PSK). */
    wo_hkdf_sha256_extract(zeros, 32, zeros, 32, early);
    /* Handshake Secret = HKDF-Extract(Derive-Secret(early,"derived",""), ECDHE). */
    derive_secret(early, "derived", eh, derived);
    wo_hkdf_sha256_extract(derived, 32, ecdhe, ecdhe_len, ks->handshake_secret);

    derive_secret(ks->handshake_secret, "c hs traffic", hash_ch_sh,
                  ks->client_hs_traffic);
    derive_secret(ks->handshake_secret, "s hs traffic", hash_ch_sh,
                  ks->server_hs_traffic);

    /* Master Secret = HKDF-Extract(Derive-Secret(hs,"derived",""), 0). */
    derive_secret(ks->handshake_secret, "derived", eh, derived);
    wo_hkdf_sha256_extract(derived, 32, zeros, 32, ks->master_secret);
}

void wo_tls_derive_application(wo_tls_key_schedule *ks,
                               const uint8_t hash_ch_sf[32]) {
    derive_secret(ks->master_secret, "c ap traffic", hash_ch_sf,
                  ks->client_ap_traffic);
    derive_secret(ks->master_secret, "s ap traffic", hash_ch_sf,
                  ks->server_ap_traffic);
}

void wo_tls_traffic_keys(const uint8_t traffic_secret[32], size_t key_len,
                         uint8_t *key, uint8_t iv[12]) {
    wo_hkdf_sha256_expand_label(traffic_secret, "key", 3, NULL, 0, key, key_len);
    wo_hkdf_sha256_expand_label(traffic_secret, "iv", 2, NULL, 0, iv, 12);
}

void wo_tls_finished_verify(const uint8_t base_key[32],
                            const uint8_t transcript_hash[32], uint8_t out[32]) {
    uint8_t finished_key[32];
    wo_hkdf_sha256_expand_label(base_key, "finished", 8, NULL, 0, finished_key, 32);
    wo_hmac_sha256(finished_key, 32, transcript_hash, 32, out);
}

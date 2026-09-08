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

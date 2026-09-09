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

#include <stdint.h>
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

/* ---- handshake messages (phase F3, RFC 8446 §4) --------------------------
 * Serialization uses a bounds-checked writer; parsing a bounds-checked
 * reader. Every malformation in a parsed message is a rejection — these bytes
 * are attacker-controlled. TLS wire lengths are fixed-size big-endian. */

/* handshake types + extension/wire constants we use. */
enum { HS_CLIENT_HELLO = 1, HS_SERVER_HELLO = 2 };
enum {
    EXT_SERVER_NAME = 0x0000, EXT_SUPPORTED_GROUPS = 0x000a,
    EXT_SIGNATURE_ALGORITHMS = 0x000d, EXT_SUPPORTED_VERSIONS = 0x002b,
    EXT_KEY_SHARE = 0x0033,
};
enum { GROUP_X25519 = 0x001d };
enum { CS_AES_128_GCM = 0x1301, CS_CHACHA20_POLY1305 = 0x1303 };

/* HelloRetryRequest is a ServerHello carrying this fixed random (§4.1.3). We
 * do not implement HRR; detect it and reject. */
static const uint8_t HRR_RANDOM[32] = {
  0xCF,0x21,0xAD,0x74,0xE5,0x9A,0x61,0x11,0xBE,0x1D,0x8C,0x02,0x1E,0x65,0xB8,0x91,
  0xC2,0xA2,0x11,0x16,0x7A,0xBB,0x8C,0x5E,0x07,0x9E,0x09,0xE2,0xC8,0xA8,0x33,0x9C
};

/* ---- bounded reader (all malformation -> ok=0) ---- */
typedef struct { const uint8_t *p; size_t n, i; int ok; } rbuf;
static uint8_t r8(rbuf *r) { if (r->i >= r->n) { r->ok = 0; return 0; } return r->p[r->i++]; }
static uint16_t r16(rbuf *r) { uint16_t v = (uint16_t)r8(r) << 8; return v | r8(r); }
static uint32_t r24(rbuf *r) {
    uint32_t v = (uint32_t)r8(r) << 16; v |= (uint32_t)r8(r) << 8; return v | r8(r);
}
static const uint8_t *rbytes(rbuf *r, size_t k) {
    if (!r->ok || r->i + k > r->n) { r->ok = 0; return NULL; }
    const uint8_t *p = r->p + r->i; r->i += k; return p;
}

/* ---- bounded writer (overflow -> ok=0) ---- */
typedef struct { uint8_t *p; size_t cap, n; int ok; } wbuf;
static void wbytes(wbuf *w, const uint8_t *b, size_t k) {
    if (!w->ok || w->n + k > w->cap) { w->ok = 0; return; }
    memcpy(w->p + w->n, b, k); w->n += k;
}
static void w8(wbuf *w, uint8_t v) { wbytes(w, &v, 1); }
static void w16(wbuf *w, uint16_t v) { uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v }; wbytes(w, b, 2); }
/* Reserve a 16-bit length placeholder; returns its offset for backpatch. */
static size_t w16_stub(wbuf *w) { size_t at = w->n; w16(w, 0); return at; }
static void w16_fill(wbuf *w, size_t at) {
    if (!w->ok) return;
    size_t len = w->n - at - 2;
    w->p[at] = (uint8_t)(len >> 8); w->p[at + 1] = (uint8_t)len;
}

/* signature_scheme wire values ClientHello offers. */
enum {
    SIG_RSA_PKCS1_SHA256 = 0x0401,
    SIG_ECDSA_P256_SHA256 = 0x0403,
    SIG_RSA_PSS_SHA256 = 0x0804,
};

/* Pull r/s (each padded to 32 bytes) out of a DER ECDSA-Sig-Value
 * SEQ { INTEGER r, INTEGER s }. 0 ok, -1 malformed. */
static int ecdsa_sig_rs(const uint8_t *sig, size_t len, uint8_t r32[32],
                        uint8_t s32[32]) {
    rbuf r = { sig, len, 0, 1 };
    if (r8(&r) != 0x30) return -1;
    size_t seqlen = r8(&r);
    if (seqlen & 0x80) return -1;                     /* short-form only here */
    for (int part = 0; part < 2; part++) {
        if (r8(&r) != 0x02) return -1;                /* INTEGER */
        size_t il = r8(&r);
        const uint8_t *iv = rbytes(&r, il);
        if (!iv) return -1;
        while (il > 0 && iv[0] == 0) { iv++; il--; }   /* drop sign byte(s) */
        if (il > 32) return -1;
        uint8_t *dst = part == 0 ? r32 : s32;
        memset(dst, 0, 32);
        memcpy(dst + (32 - il), iv, il);
    }
    return r.ok ? 0 : -1;
}

/* Verify a server CertificateVerify (RFC 8446 §4.4.3). The signed content is
 * 64*0x20 || "TLS 1.3, server CertificateVerify" || 0x00 || transcript_hash,
 * and the signature is over that content under the leaf certificate's key.
 * Only the three schemes ClientHello offered are accepted; the scheme must
 * match the leaf key type. 1 valid, 0 otherwise. */
int wo_tls_verify_cert_verify(const uint8_t *leaf_der, size_t leaf_len,
                              uint16_t sig_scheme, const uint8_t *sig,
                              size_t sig_len, const uint8_t transcript_hash[32]) {
    int key_alg;
    const uint8_t *n, *e, *x, *y; size_t nl, el;
    if (wo_x509_parse_spki(leaf_der, leaf_len, &key_alg, &n, &nl, &e, &el, &x, &y) != 0)
        return 0;

    /* content = 64 spaces || context-string || 0x00 || transcript_hash */
    static const char CTX[] = "TLS 1.3, server CertificateVerify";
    uint8_t content[64 + 33 + 1 + 32];
    memset(content, 0x20, 64);
    memcpy(content + 64, CTX, 33);
    content[64 + 33] = 0x00;
    memcpy(content + 64 + 34, transcript_hash, 32);
    uint8_t mhash[32];
    wo_sha256(content, sizeof content, mhash);

    switch (sig_scheme) {
    case SIG_RSA_PSS_SHA256:
        return key_alg == 1 &&
               wo_rsa_pss_sha256_verify(n, nl, e, el, sig, sig_len, mhash, 32);
    case SIG_RSA_PKCS1_SHA256:
        return key_alg == 1 &&
               wo_rsa_pkcs1_sha256_verify(n, nl, e, el, sig, sig_len, mhash);
    case SIG_ECDSA_P256_SHA256: {
        uint8_t r32[32], s32[32];
        if (key_alg != 2 || ecdsa_sig_rs(sig, sig_len, r32, s32) != 0) return 0;
        return wo_ecdsa_p256_sha256_verify(x, y, r32, s32, mhash);
    }
    default:
        return 0;
    }
}

/* Parse a ServerHello handshake message. Extracts the negotiated suite (as a
 * WO_TLS_* enum) and the server's X25519 key-share. 0 ok, -1 on any
 * malformation, an unsupported suite/group, or a HelloRetryRequest. */
int wo_tls_parse_server_hello(const uint8_t *msg, size_t len, int *suite,
                              uint8_t server_pub[32]) {
    rbuf r = { msg, len, 0, 1 };
    if (r8(&r) != HS_SERVER_HELLO) return -1;
    uint32_t body = r24(&r);
    if (!r.ok || body != len - 4) return -1;
    if (r16(&r) != 0x0303) return -1;                 /* legacy_version */
    const uint8_t *random = rbytes(&r, 32);
    if (!random || memcmp(random, HRR_RANDOM, 32) == 0) return -1;  /* no HRR */
    uint8_t sidlen = r8(&r);
    if (sidlen > 32 || !rbytes(&r, sidlen)) return -1;
    uint16_t cs = r16(&r);
    if (cs == CS_AES_128_GCM) *suite = WO_TLS_AES_128_GCM_SHA256;
    else if (cs == CS_CHACHA20_POLY1305) *suite = WO_TLS_CHACHA20_POLY1305_SHA256;
    else return -1;
    if (r8(&r) != 0) return -1;                       /* legacy_compression */

    uint16_t extlen = r16(&r);
    const uint8_t *ext = rbytes(&r, extlen);
    if (!ext) return -1;
    rbuf e = { ext, extlen, 0, 1 };
    int have_ks = 0, have_ver = 0;
    while (e.ok && e.i < e.n) {
        uint16_t type = r16(&e), elen = r16(&e);
        const uint8_t *ed = rbytes(&e, elen);
        if (!ed) return -1;
        rbuf d = { ed, elen, 0, 1 };
        if (type == EXT_KEY_SHARE) {
            if (r16(&d) != GROUP_X25519) return -1;   /* group */
            if (r16(&d) != 32) return -1;             /* key_exchange length */
            const uint8_t *k = rbytes(&d, 32);
            if (!k) return -1;
            memcpy(server_pub, k, 32);
            have_ks = 1;
        } else if (type == EXT_SUPPORTED_VERSIONS) {
            if (r16(&d) != 0x0304) return -1;         /* selected_version 1.3 */
            have_ver = 1;
        }
    }
    if (!e.ok || !have_ks || !have_ver) return -1;
    return 0;
}

/* Build a ClientHello handshake message (offering TLS 1.3, x25519, and
 * RSA-PSS/RSA-PKCS1/ECDSA-P256 signatures) for `hostname`. random32 and
 * session_id are caller-supplied (fresh randomness / a 32-byte legacy id for
 * middlebox compatibility). Writes the message into out; *outlen gets its
 * length. Returns 0 ok, -1 if out is too small. */
int wo_tls_build_client_hello(const char *hostname, size_t hostlen,
                              const uint8_t client_pub[32],
                              const uint8_t random32[32],
                              const uint8_t session_id[32], uint8_t *out,
                              size_t outcap, size_t *outlen) {
    wbuf w = { out, outcap, 0, 1 };
    w8(&w, HS_CLIENT_HELLO);
    /* 3-byte handshake length placeholder, backpatched at the end */
    size_t hlen_at = w.n; w8(&w, 0); w8(&w, 0); w8(&w, 0);

    w16(&w, 0x0303);                                  /* legacy_version */
    wbytes(&w, random32, 32);
    w8(&w, 32); wbytes(&w, session_id, 32);           /* legacy_session_id */
    /* cipher_suites: AES-128-GCM, ChaCha20-Poly1305 */
    w16(&w, 4); w16(&w, CS_AES_128_GCM); w16(&w, CS_CHACHA20_POLY1305);
    w8(&w, 1); w8(&w, 0);                             /* compression: null */

    size_t exts_at = w16_stub(&w);                    /* extensions length */

    /* server_name (SNI) */
    w16(&w, EXT_SERVER_NAME);
    size_t sni_at = w16_stub(&w);
    w16(&w, (uint16_t)(hostlen + 3));                 /* server_name_list len */
    w8(&w, 0);                                         /* name_type host_name */
    w16(&w, (uint16_t)hostlen);
    wbytes(&w, (const uint8_t *)hostname, hostlen);
    w16_fill(&w, sni_at);

    /* supported_groups: x25519 */
    w16(&w, EXT_SUPPORTED_GROUPS);
    w16(&w, 4); w16(&w, 2); w16(&w, GROUP_X25519);

    /* signature_algorithms */
    w16(&w, EXT_SIGNATURE_ALGORITHMS);
    w16(&w, 8); w16(&w, 6);
    w16(&w, 0x0804);                                  /* rsa_pss_rsae_sha256 */
    w16(&w, 0x0401);                                  /* rsa_pkcs1_sha256 */
    w16(&w, 0x0403);                                  /* ecdsa_secp256r1_sha256 */

    /* supported_versions: TLS 1.3 */
    w16(&w, EXT_SUPPORTED_VERSIONS);
    w16(&w, 3); w8(&w, 2); w16(&w, 0x0304);

    /* key_share: x25519 */
    w16(&w, EXT_KEY_SHARE);
    w16(&w, 38); w16(&w, 36);                         /* ext len, client_shares len */
    w16(&w, GROUP_X25519); w16(&w, 32);
    wbytes(&w, client_pub, 32);

    w16_fill(&w, exts_at);

    /* backpatch the 3-byte handshake length */
    if (w.ok) {
        size_t blen = w.n - hlen_at - 3;
        w.p[hlen_at] = (uint8_t)(blen >> 16);
        w.p[hlen_at + 1] = (uint8_t)(blen >> 8);
        w.p[hlen_at + 2] = (uint8_t)blen;
    }
    if (!w.ok) return -1;
    *outlen = w.n;
    return 0;
}

/* ---- sans-io client handshake driver (phase F3c) -------------------------
 * The FSM described in tls.h. The caller frames records; this drives the
 * handshake and produces the client Finished. Every failure path lands in
 * ST_FAILED and returns WO_TLS_FAILED — there is no "warn and continue". */

enum { ST_WANT_SH = 0, ST_WANT_FLIGHT, ST_ESTABLISHED, ST_FAILED };

/* Constant-time 32-byte compare (verify_data). 1 equal, 0 not. */
static int ct_eq32(const uint8_t *a, const uint8_t *b) {
    uint8_t d = 0;
    for (int i = 0; i < 32; i++) d |= a[i] ^ b[i];
    return d == 0;
}

/* Append to the transcript; 0 ok, -1 overflow. */
static int tr_add(wo_tls_client *c, const uint8_t *p, size_t n) {
    if (c->tlen + n > sizeof c->transcript) return -1;
    memcpy(c->transcript + c->tlen, p, n); c->tlen += n;
    return 0;
}

int wo_tls_client_start_with(wo_tls_client *c, const uint8_t *ch_msg,
                             size_t ch_len, const uint8_t priv[32]) {
    memset(c, 0, sizeof *c);
    memcpy(c->client_priv, priv, 32);
    /* client_pub = X25519(priv, basepoint 9) — informational here. */
    uint8_t base[32] = { 9 };
    wo_x25519(c->client_pub, priv, base);

    if (tr_add(c, ch_msg, ch_len) != 0) { c->st = ST_FAILED; return -1; }
    /* frame the ClientHello as a plaintext handshake record (legacy 0x0301). */
    if (5 + ch_len > sizeof c->out) { c->st = ST_FAILED; return -1; }
    c->out[0] = WO_TLS_CT_HANDSHAKE;
    c->out[1] = 0x03; c->out[2] = 0x01;
    c->out[3] = (uint8_t)(ch_len >> 8); c->out[4] = (uint8_t)ch_len;
    memcpy(c->out + 5, ch_msg, ch_len);
    c->outn = 5 + ch_len;
    c->st = ST_WANT_SH;
    return 0;
}

void wo_tls_client_set_host(wo_tls_client *c, const char *host, size_t hostlen) {
    c->host = host; c->hostlen = hostlen;
}

size_t wo_tls_client_take_output(wo_tls_client *c, uint8_t *out, size_t outcap) {
    size_t n = c->outn < outcap ? c->outn : 0;   /* all-or-nothing */
    if (n) { memcpy(out, c->out, n); c->outn = 0; }
    return n;
}

/* Process the ServerHello record. */
static wo_tls_status on_server_hello(wo_tls_client *c, const uint8_t *rec,
                                     size_t reclen) {
    size_t bodylen = ((size_t)rec[3] << 8) | rec[4];
    if (bodylen + 5 != reclen) return WO_TLS_FAILED;
    const uint8_t *sh = rec + 5;
    uint8_t server_pub[32];
    if (wo_tls_parse_server_hello(sh, bodylen, &c->suite, server_pub) != 0)
        return WO_TLS_FAILED;
    c->keylen = c->suite == WO_TLS_AES_128_GCM_SHA256 ? 16 : 32;
    if (tr_add(c, sh, bodylen) != 0) return WO_TLS_FAILED;

    uint8_t ecdhe[32], th[32];
    wo_x25519(ecdhe, c->client_priv, server_pub);
    wo_sha256(c->transcript, c->tlen, th);            /* CH..SH */
    wo_tls_derive_handshake(&c->ks, ecdhe, 32, th);
    /* read = server handshake keys, write = client handshake keys */
    wo_tls_traffic_keys(c->ks.server_hs_traffic, c->keylen, c->rd_key, c->rd_iv);
    wo_tls_traffic_keys(c->ks.client_hs_traffic, c->keylen, c->wr_key, c->wr_iv);
    c->rd_seq = c->wr_seq = 0;
    c->st = ST_WANT_FLIGHT;
    return WO_TLS_WANT_MORE;
}

/* Handle one decrypted handshake message from the server flight. Returns 1 if
 * this message completed the handshake (server Finished), 0 to continue, -1 on
 * failure. */
static int on_flight_msg(wo_tls_client *c, const uint8_t *msg, size_t mlen) {
    uint8_t type = msg[0];
    if (type == 0x08) {                               /* EncryptedExtensions */
        return tr_add(c, msg, mlen) == 0 ? 0 : -1;
    }
    if (type == 0x0b) {                               /* Certificate */
        /* leaf = first CertificateEntry's cert_data */
        if (mlen < 8) return -1;
        size_t p = 4 + 1 + msg[4];                    /* skip ctx */
        if (p + 3 > mlen) return -1;
        p += 3;                                        /* cert_list length */
        if (p + 3 > mlen) return -1;
        size_t clen = ((size_t)msg[p] << 16) | ((size_t)msg[p+1] << 8) | msg[p+2];
        p += 3;
        if (p + clen > mlen || clen > sizeof c->leaf) return -1;
        memcpy(c->leaf, msg + p, clen); c->leaflen = clen;
        /* hostname check (when a host was set): a leaf whose SAN does not match
         * the target host is a refused connection, not a warning. */
        if (c->host && !wo_x509_check_host(c->leaf, c->leaflen, c->host, c->hostlen))
            return -1;
        return tr_add(c, msg, mlen) == 0 ? 0 : -1;
    }
    if (type == 0x0f) {                               /* CertificateVerify */
        if (c->leaflen == 0 || mlen < 8) return -1;
        uint8_t th[32];
        wo_sha256(c->transcript, c->tlen, th);        /* CH..Certificate */
        uint16_t scheme = ((uint16_t)msg[4] << 8) | msg[5];
        size_t siglen = ((size_t)msg[6] << 8) | msg[7];
        if (8 + siglen > mlen) return -1;
        if (!wo_tls_verify_cert_verify(c->leaf, c->leaflen, scheme, msg + 8, siglen, th))
            return -1;
        return tr_add(c, msg, mlen) == 0 ? 0 : -1;
    }
    if (type == 0x14) {                               /* Finished (server) */
        if (mlen != 4 + 32) return -1;
        uint8_t th[32], expect[32];
        wo_sha256(c->transcript, c->tlen, th);        /* CH..CertificateVerify */
        wo_tls_finished_verify(c->ks.server_hs_traffic, th, expect);
        if (!ct_eq32(expect, msg + 4)) return -1;
        if (tr_add(c, msg, mlen) != 0) return -1;

        /* application keys need the transcript through server Finished. */
        uint8_t th_sf[32];
        wo_sha256(c->transcript, c->tlen, th_sf);     /* CH..server Finished */
        wo_tls_derive_application(&c->ks, th_sf);

        /* client Finished = HMAC over the same transcript with client hs key */
        uint8_t vd[32];
        wo_tls_finished_verify(c->ks.client_hs_traffic, th_sf, vd);
        uint8_t cfin[36];
        cfin[0] = 0x14; cfin[1] = 0; cfin[2] = 0; cfin[3] = 32;
        memcpy(cfin + 4, vd, 32);
        /* encrypt with the client HANDSHAKE keys, then switch to app keys. */
        int n = wo_tls_record_seal(c->suite, c->wr_key, c->keylen, c->wr_iv,
                                   c->wr_seq, WO_TLS_CT_HANDSHAKE, cfin, 36, c->out);
        if (n < 0) return -1;
        c->outn = (size_t)n; c->wr_seq++;

        wo_tls_traffic_keys(c->ks.server_ap_traffic, c->keylen, c->rd_key, c->rd_iv);
        wo_tls_traffic_keys(c->ks.client_ap_traffic, c->keylen, c->wr_key, c->wr_iv);
        c->rd_seq = c->wr_seq = 0;
        c->st = ST_ESTABLISHED;
        return 1;
    }
    return -1;                                        /* unexpected message */
}

wo_tls_status wo_tls_client_push_record(wo_tls_client *c, const uint8_t *rec,
                                        size_t reclen) {
    if (c->st == ST_FAILED) return WO_TLS_FAILED;
    if (reclen < 5) { c->st = ST_FAILED; return WO_TLS_FAILED; }
    uint8_t ct = rec[0];
    if (ct == WO_TLS_CT_CHANGE_CIPHER_SPEC) return WO_TLS_WANT_MORE;  /* ignore */

    if (c->st == ST_WANT_SH) {
        if (ct != WO_TLS_CT_HANDSHAKE) { c->st = ST_FAILED; return WO_TLS_FAILED; }
        wo_tls_status s = on_server_hello(c, rec, reclen);
        if (s == WO_TLS_FAILED) c->st = ST_FAILED;
        return s;
    }
    if (c->st == ST_WANT_FLIGHT) {
        if (ct != WO_TLS_CT_APPLICATION_DATA) { c->st = ST_FAILED; return WO_TLS_FAILED; }
        uint8_t pt[WO_TLS_BUF_MAX]; uint8_t inner = 0;
        int n = wo_tls_record_open(c->suite, c->rd_key, c->keylen, c->rd_iv,
                                   c->rd_seq, rec, reclen, pt, &inner);
        if (n < 0) { c->st = ST_FAILED; return WO_TLS_FAILED; }
        c->rd_seq++;
        if (inner != WO_TLS_CT_HANDSHAKE) { c->st = ST_FAILED; return WO_TLS_FAILED; }
        if (c->hsn + (size_t)n > sizeof c->hsbuf) { c->st = ST_FAILED; return WO_TLS_FAILED; }
        memcpy(c->hsbuf + c->hsn, pt, (size_t)n); c->hsn += (size_t)n;

        /* process every complete handshake message now buffered */
        size_t off = 0;
        while (c->hsn - off >= 4) {
            const uint8_t *m = c->hsbuf + off;
            size_t mlen = 4 + (((size_t)m[1] << 16) | ((size_t)m[2] << 8) | m[3]);
            if (c->hsn - off < mlen) break;           /* wait for more records */
            int r = on_flight_msg(c, m, mlen);
            if (r < 0) { c->st = ST_FAILED; return WO_TLS_FAILED; }
            off += mlen;
            if (r == 1) return WO_TLS_ESTABLISHED;    /* Finished processed */
        }
        /* keep the tail (a partial message) for the next record */
        if (off > 0) { memmove(c->hsbuf, c->hsbuf + off, c->hsn - off); c->hsn -= off; }
        return WO_TLS_WANT_MORE;
    }
    return WO_TLS_WANT_MORE;                           /* already established */
}

int wo_tls_client_encrypt(wo_tls_client *c, const uint8_t *data, size_t len,
                          uint8_t *out, size_t outcap) {
    if (c->st != ST_ESTABLISHED) return -1;
    if (len + WO_TLS_RECORD_OVERHEAD > outcap) return -1;
    int n = wo_tls_record_seal(c->suite, c->wr_key, c->keylen, c->wr_iv,
                               c->wr_seq, WO_TLS_CT_APPLICATION_DATA, data, len, out);
    if (n < 0) return -1;
    c->wr_seq++;
    return n;
}

int wo_tls_client_decrypt(wo_tls_client *c, const uint8_t *rec, size_t reclen,
                          uint8_t *out, size_t outcap, uint8_t *content_type) {
    if (c->st != ST_ESTABLISHED) return -1;
    if (reclen > outcap + WO_TLS_RECORD_OVERHEAD) return -1;
    int n = wo_tls_record_open(c->suite, c->rd_key, c->keylen, c->rd_iv,
                               c->rd_seq, rec, reclen, out, content_type);
    if (n < 0) return -1;
    c->rd_seq++;
    return n;
}

/* ---- certificate chain validation (phase F3c-net security core) ----------
 * Verify a server certificate chain (leaf-first DER). Each cert must be signed
 * by the next; the chain top must be trusted (equal to, or signed by, a trust
 * anchor); the leaf SAN must match the host; and every cert must be inside its
 * validity window. Any failure is a rejection — no partial trust. Pure over
 * the public phase-D/E verifiers, so it is offline-testable; the CA-bundle load
 * and socket glue that feed it are the live-gated remainder of F3c-net. */
int wo_tls_verify_chain(const uint8_t *const *certs, const size_t *cert_lens,
                        size_t n_certs, const uint8_t *const *anchors,
                        const size_t *anchor_lens, size_t n_anchors,
                        const char *host, size_t hostlen, const char now14[14]) {
    if (n_certs == 0 || n_anchors == 0) return 0;

    /* leaf hostname (SAN) must match, and the leaf must be usable as a server
     * cert (decision 6: EKU serverAuth, or no EKU). */
    if (host && !wo_x509_check_host(certs[0], cert_lens[0], host, hostlen))
        return 0;
    if (!wo_x509_eku_serverauth_ok(certs[0], cert_lens[0]))
        return 0;

    /* every cert must be temporally valid. */
    for (size_t i = 0; i < n_certs; i++)
        if (!wo_x509_check_validity(certs[i], cert_lens[i], now14)) return 0;

    /* every issuer the server sent (certs[1..]) must be a CA (decision 6:
     * basicConstraints CA:TRUE, and its pathLenConstraint must cover the number
     * of intermediates below it). This is what stops a leaf masquerading as a
     * CA. Index i issues cert i-1; the intermediates strictly below it (above
     * the leaf) are indices 1..i-1, i.e. (i-1) of them. */
    for (size_t i = 1; i < n_certs; i++) {
        int is_ca, has_pl, pl;
        if (wo_x509_basic_constraints(certs[i], cert_lens[i], &is_ca, &has_pl, &pl) != 0)
            return 0;
        if (!is_ca) return 0;
        if (has_pl && pl < (int)(i - 1)) return 0;
    }

    /* each cert is signed by the next one the server sent. */
    for (size_t i = 0; i + 1 < n_certs; i++)
        if (!wo_x509_verify_one(certs[i], cert_lens[i], certs[i + 1], cert_lens[i + 1]))
            return 0;

    /* the chain top must chain to a trust anchor: either it is one verbatim, or
     * an anchor (which must itself be a CA) signed it. When an anchor signs the
     * top, its pathLenConstraint must cover all (n_certs-1) intermediates. */
    const uint8_t *top = certs[n_certs - 1]; size_t toplen = cert_lens[n_certs - 1];
    for (size_t a = 0; a < n_anchors; a++) {
        if (toplen == anchor_lens[a] && memcmp(top, anchors[a], toplen) == 0)
            return 1;                                   /* server sent the root */
        int is_ca, has_pl, pl;
        if (wo_x509_basic_constraints(anchors[a], anchor_lens[a], &is_ca, &has_pl, &pl) != 0)
            continue;
        if (!is_ca) continue;                            /* anchor not a CA */
        if (has_pl && pl < (int)(n_certs - 1)) continue; /* pathLen too short */
        if (wo_x509_verify_one(top, toplen, anchors[a], anchor_lens[a]))
            return 1;                                   /* anchor signed the top */
    }
    return 0;                                            /* untrusted */
}

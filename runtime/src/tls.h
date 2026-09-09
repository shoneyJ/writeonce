/* tls.h — hand-rolled TLS 1.3 (runtime-v2 9 phase F). Sits on the crypto
 * ladder (crypto.h): AEAD (A), HKDF (B), X25519 (C), signatures (D), X.509
 * (E). This header is phase F: the record layer first, the handshake FSM and
 * net.connect_tls on top. Internal C; the VM enters through net builtins. */
#ifndef WO_TLS_H
#define WO_TLS_H

#include <stddef.h>
#include <stdint.h>

/* The two SHA-256 TLS 1.3 cipher suites we implement. AES-128-GCM is
 * mandatory-to-implement (RFC 8446 §9.1); ChaCha20-Poly1305 is the portable
 * fallback when the CPU has no AES-NI. AES-256-GCM uses SHA-384 and is a later
 * add (the HKDF is SHA-256 today). */
enum {
    WO_TLS_AES_128_GCM_SHA256 = 1,
    WO_TLS_CHACHA20_POLY1305_SHA256 = 2,
};

/* TLS 1.3 record content types (RFC 8446 §5.1). */
enum {
    WO_TLS_CT_CHANGE_CIPHER_SPEC = 20,
    WO_TLS_CT_ALERT = 21,
    WO_TLS_CT_HANDSHAKE = 22,
    WO_TLS_CT_APPLICATION_DATA = 23,
};

/* Overhead a sealed record adds over its plaintext: 5-byte header + 1-byte
 * inner content-type + 16-byte AEAD tag. */
#define WO_TLS_RECORD_OVERHEAD 22

/* Seal one TLS 1.3 record (RFC 8446 §5.2). Writes the full wire record —
 * 5-byte header || encrypted (TLSInnerPlaintext) || 16-byte tag — into out,
 * which must hold at least ptlen + WO_TLS_RECORD_OVERHEAD bytes. `seq` is the
 * record sequence number; the per-record nonce is iv XOR seq (big-endian, §5.3).
 * No padding. Returns the record length, or -1 on a bad suite. */
int wo_tls_record_seal(int suite, const uint8_t *key, size_t keylen,
                       const uint8_t iv[12], uint64_t seq, uint8_t content_type,
                       const uint8_t *pt, size_t ptlen, uint8_t *out);

/* Open one TLS 1.3 record. `rec` is the full wire record (header included),
 * reclen its length. Writes the recovered content into out (must hold
 * reclen bytes) and the recovered inner content-type into *content_type,
 * after stripping trailing zero padding (§5.2/§5.4). Returns the content
 * length, or -1 on a malformed record or AEAD authentication failure. */
int wo_tls_record_open(int suite, const uint8_t *key, size_t keylen,
                       const uint8_t iv[12], uint64_t seq, const uint8_t *rec,
                       size_t reclen, uint8_t *out, uint8_t *content_type);

/* ---- key schedule (phase F2, RFC 8446 §7.1, SHA-256) --------------------- */

/* The traffic secrets the handshake derives, in the order they become known. */
typedef struct {
    uint8_t handshake_secret[32];
    uint8_t master_secret[32];
    uint8_t client_hs_traffic[32];
    uint8_t server_hs_traffic[32];
    uint8_t client_ap_traffic[32];
    uint8_t server_ap_traffic[32];
} wo_tls_key_schedule;

/* Handshake-phase secrets from the ECDHE shared secret and the
 * ClientHello..ServerHello transcript hash. Fills handshake_secret, the two
 * hs-traffic secrets, and master_secret (which needs no further transcript). */
void wo_tls_derive_handshake(wo_tls_key_schedule *ks, const uint8_t *ecdhe,
                             size_t ecdhe_len, const uint8_t hash_ch_sh[32]);

/* Application-phase traffic secrets from master_secret (already in ks) and the
 * ClientHello..server-Finished transcript hash. */
void wo_tls_derive_application(wo_tls_key_schedule *ks,
                               const uint8_t hash_ch_sf[32]);

/* Per-direction AEAD key (key_len 16 or 32) and 12-byte IV from a traffic
 * secret (HKDF-Expand-Label "key"/"iv"). */
void wo_tls_traffic_keys(const uint8_t traffic_secret[32], size_t key_len,
                         uint8_t *key, uint8_t iv[12]);

/* Finished verify_data = HMAC(HKDF-Expand-Label(base_key,"finished","",32),
 * transcript_hash). Same routine builds and checks it (compare with ct_memeq). */
void wo_tls_finished_verify(const uint8_t base_key[32],
                            const uint8_t transcript_hash[32], uint8_t out[32]);

/* ---- handshake messages (phase F3, RFC 8446 §4) -------------------------- */

/* Parse a ServerHello handshake message (bytes start at the handshake type
 * 0x02). Fills *suite (a WO_TLS_* enum) and the server's 32-byte X25519 key
 * share. Returns 0, or -1 on any malformation, an unsupported suite/group, or
 * a HelloRetryRequest. */
int wo_tls_parse_server_hello(const uint8_t *msg, size_t len, int *suite,
                              uint8_t server_pub[32]);

/* Verify a server CertificateVerify (RFC 8446 §4.4.3) against the leaf
 * certificate DER, given the negotiated signature_scheme wire value, the
 * signature, and the running transcript hash (ClientHello..Certificate). Only
 * rsa_pss_rsae_sha256 (0x0804), rsa_pkcs1_sha256 (0x0401) and
 * ecdsa_secp256r1_sha256 (0x0403) are accepted, and the scheme must match the
 * leaf key type. Returns 1 valid, 0 otherwise. */
int wo_tls_verify_cert_verify(const uint8_t *leaf_der, size_t leaf_len,
                              uint16_t sig_scheme, const uint8_t *sig,
                              size_t sig_len, const uint8_t transcript_hash[32]);

/* Build a ClientHello handshake message offering TLS 1.3 / x25519 /
 * RSA-PSS+RSA-PKCS1+ECDSA-P256, for `hostname` (SNI). random32 and the 32-byte
 * legacy session_id are caller-supplied. Writes into out (cap outcap); *outlen
 * gets the length. Returns 0, or -1 if the buffer is too small. */
int wo_tls_build_client_hello(const char *hostname, size_t hostlen,
                              const uint8_t client_pub[32],
                              const uint8_t random32[32],
                              const uint8_t session_id[32], uint8_t *out,
                              size_t outcap, size_t *outlen);

/* Verify a server certificate chain (leaf-first DER): each cert signed by the
 * next, the chain top trusted (equal to, or signed by, one of the anchors), the
 * leaf SAN matching host (pass NULL to skip), and every cert within its
 * validity window at now14 ("YYYYMMDDHHMMSS"). 1 fully valid & trusted, 0
 * otherwise (no partial trust). The offline-testable security core of the
 * F3c-net remainder; the CA-bundle load + socket glue that feed it are still to
 * come. */
int wo_tls_verify_chain(const uint8_t *const *certs, const size_t *cert_lens,
                        size_t n_certs, const uint8_t *const *anchors,
                        const size_t *anchor_lens, size_t n_anchors,
                        const char *host, size_t hostlen, const char now14[14]);

/* Decode a PEM bundle (concatenated CERTIFICATE blocks) into DER trust anchors.
 * base64-decodes each block into `arena` (appended) and records its span in
 * certs[]/cert_lens[]; returns the count (0..max_certs) or -1 on arena overflow
 * or a malformed block. Pure — the caller reads the file and owns the arena, so
 * this is offline-testable. (rv2 9 F3c-net decision 4) */
long wo_tls_pem_to_ders(const char *pem, size_t pemlen, uint8_t *arena,
                        size_t arena_cap, const uint8_t **certs,
                        size_t *cert_lens, size_t max_certs);

/* ---- sans-io client handshake driver (phase F3c) -------------------------
 * A pure state machine: no sockets. The caller frames TLS records (read the
 * 5-byte header, then that many bytes) and feeds whole records in; the driver
 * advances the handshake and hands back bytes to send. This keeps every I/O
 * concern out of the security-critical FSM, so it is fully testable offline
 * (KAT'd against the RFC 8448 record trace).
 *
 * SECURITY NOTE — not yet a safe live client: this core verifies the server's
 * CertificateVerify and Finished (proving possession of the leaf key) but does
 * NOT yet walk the certificate chain to a trust anchor or match the hostname
 * against the cert SAN. Those (the deferred phase-E bits) MUST land before this
 * drives a real connection, or the client is open to MITM. It is currently an
 * internal building block; net.connect_tls is not wired to it. */

#define WO_TLS_BUF_MAX 16384u     /* transcript / reassembly cap (RFC 8448 fits) */
#define WO_TLS_LEAF_MAX 8192u     /* largest leaf certificate accepted */

typedef enum {
    WO_TLS_WANT_MORE = 0,         /* need another record */
    WO_TLS_ESTABLISHED = 1,       /* handshake done; output holds client Finished */
    WO_TLS_FAILED = -1,           /* verification/parse failure — connection dead */
} wo_tls_status;

typedef struct {
    int suite;
    size_t keylen;                            /* AEAD key length, 16 or 32 */
    uint8_t client_priv[32], client_pub[32];
    wo_tls_key_schedule ks;
    /* current read/write record protection (handshake, then application) */
    uint8_t rd_key[32], rd_iv[12], wr_key[32], wr_iv[12];
    uint64_t rd_seq, wr_seq;
    uint8_t transcript[WO_TLS_BUF_MAX]; size_t tlen;
    uint8_t hsbuf[WO_TLS_BUF_MAX]; size_t hsn;   /* reassembled handshake bytes */
    uint8_t leaf[WO_TLS_LEAF_MAX]; size_t leaflen;
    uint16_t cv_scheme;
    uint8_t out[1024]; size_t outn;              /* bytes for the caller to send */
    const char *host; size_t hostlen;            /* if set, leaf SAN is enforced */
    int st;                                      /* internal FSM state */
} wo_tls_client;

/* Enforce the leaf certificate's SAN against `host` during the handshake — a
 * connection whose certificate does not match is refused. MUST be called
 * (after start) for a real connection; if left unset the driver skips the
 * hostname check (offline testing only, and MITM-unsafe on the wire). The
 * string must outlive the handshake (not copied). */
void wo_tls_client_set_host(wo_tls_client *c, const char *host, size_t hostlen);

/* Start a handshake from a caller-built ClientHello handshake message and a
 * fixed X25519 private key (production passes fresh randomness; the KAT injects
 * the RFC's). Frames the ClientHello into a plaintext record in c->out for the
 * caller to send, and seeds the transcript. 0 ok, -1 on a buffer problem. */
int wo_tls_client_start_with(wo_tls_client *c, const uint8_t *ch_msg,
                             size_t ch_len, const uint8_t priv[32]);

/* Feed one whole TLS record. Advances the FSM. Returns WANT_MORE (need the
 * next record), ESTABLISHED (handshake complete — drain c->out for the client
 * Finished, then use the encrypt/decrypt calls), or FAILED. */
wo_tls_status wo_tls_client_push_record(wo_tls_client *c, const uint8_t *rec,
                                        size_t reclen);

/* Copy out (and clear) the bytes the driver wants sent. Returns the count. */
size_t wo_tls_client_take_output(wo_tls_client *c, uint8_t *out, size_t outcap);

/* Application data, post-handshake (application traffic keys). encrypt writes a
 * full record into out; decrypt reads one record and yields the plaintext plus
 * its inner content type. Return the byte count, or -1 on overflow / auth
 * failure. */
int wo_tls_client_encrypt(wo_tls_client *c, const uint8_t *data, size_t len,
                          uint8_t *out, size_t outcap);
int wo_tls_client_decrypt(wo_tls_client *c, const uint8_t *rec, size_t reclen,
                          uint8_t *out, size_t outcap, uint8_t *content_type);

#endif

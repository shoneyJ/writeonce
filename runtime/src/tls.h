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

#endif

/* crypto.h — hand-rolled digests (iteration 34). Whole-value, libc-only.
 * The raw cores are exposed for the unit test; the VM enters through
 * wo_builtin_crypto (ids WO_B_SHA1..WO_B_HMAC_SHA256). */
#ifndef WO_CRYPTO_H
#define WO_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#include "vm.h"

void wo_sha1(const uint8_t *msg, size_t len, uint8_t out[20]);
void wo_sha256(const uint8_t *msg, size_t len, uint8_t out[32]);
void wo_hmac_sha256(const uint8_t *key, size_t klen, const uint8_t *msg,
                    size_t mlen, uint8_t out[32]);

/* ChaCha20-Poly1305 AEAD (rv2 8 phase A, RFC 8439). Raw cores exposed for
 * the unit test; the VM enters through wo_builtin_crypto. */
void wo_poly1305(const uint8_t key[32], const uint8_t *m, size_t bytes,
                 uint8_t mac[16]);
int wo_chacha20poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
                             const uint8_t *aad, size_t aadlen,
                             const uint8_t *pt, size_t ptlen, uint8_t *out);
int wo_chacha20poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
                             const uint8_t *aad, size_t aadlen,
                             const uint8_t *ct, size_t ctlen,
                             const uint8_t tag[16], uint8_t *out);

/* AES-GCM (rv2 8 phase B, hardware AES-NI/PCLMULQDQ path). keylen 16 or 32,
 * nonce 12 bytes. seal writes out[ptlen] || tag[16]. Returns 0 ok, 1 auth
 * failure (open), -2 when no hardware AES is available (phase C fallback). */
int wo_aes_gcm_available(void);
extern int wo_aes_force_software; /* test hook: force the portable AES path */
int wo_aes_gcm_seal(const uint8_t *key, size_t keylen, const uint8_t nonce[12],
                    const uint8_t *aad, size_t aadlen, const uint8_t *pt,
                    size_t ptlen, uint8_t *out);
int wo_aes_gcm_open(const uint8_t *key, size_t keylen, const uint8_t nonce[12],
                    const uint8_t *aad, size_t aadlen, const uint8_t *ct,
                    size_t ctlen, const uint8_t tag[16], uint8_t *out);

/* HKDF-SHA256 (rv2 9 phase B: the TLS 1.3 key schedule). RFC 5869 + RFC 8446
 * §7.1. Internal to the runtime's crypto/TLS code (no `.wo` builtin yet). */
void wo_hkdf_sha256_extract(const uint8_t *salt, size_t saltlen,
                            const uint8_t *ikm, size_t ikmlen, uint8_t prk[32]);
int wo_hkdf_sha256_expand(const uint8_t prk[32], const uint8_t *info,
                          size_t infolen, uint8_t *okm, size_t okmlen);
int wo_hkdf_sha256_expand_label(const uint8_t secret[32], const char *label,
                                size_t labellen, const uint8_t *ctx,
                                size_t ctxlen, uint8_t *out, size_t outlen);

/* X25519 (rv2 9 phase C, RFC 7748) — internal C, consumed by the TLS ECDHE
 * handshake. out = X25519(scalar, u-coordinate). */
void wo_x25519(uint8_t out[32], const uint8_t scalar[32],
               const uint8_t point[32]);

/* RSA signature verification (rv2 9 phase D, SHA-256). Public-key only, so
 * not constant-time by design. n/e/sig big-endian; hash is the 32-byte digest.
 * Returns 1 on a valid signature, 0 otherwise. */
int wo_rsa_pkcs1_sha256_verify(const uint8_t *n, size_t nlen, const uint8_t *e,
                               size_t elen, const uint8_t *sig, size_t siglen,
                               const uint8_t hash[32]);
int wo_rsa_pss_sha256_verify(const uint8_t *n, size_t nlen, const uint8_t *e,
                             size_t elen, const uint8_t *sig, size_t siglen,
                             const uint8_t mhash[32], size_t saltlen);

/* ECDSA-P256 verify (rv2 9 phase D). Public key (qx,qy) affine, signature
 * (r,s), 32-byte SHA-256 hash; all big-endian. 1 valid, 0 otherwise. */
int wo_ecdsa_p256_sha256_verify(const uint8_t qx[32], const uint8_t qy[32],
                                const uint8_t r[32], const uint8_t s[32],
                                const uint8_t hash[32]);

int wo_builtin_crypto(wo_vm *vm, uint64_t *R, uint32_t ins, const char **msg);

#endif

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

/* RSA-PSS SIGN over SHA-256 (rv2 9 phase G1). Private key (n, d) big-endian;
 * caller supplies the salt (fresh in production, fixed for a KAT). Writes nlen
 * signature bytes. The modexp with the secret d is constant-time. 0 ok, -1 on
 * a bad size. */
int wo_rsa_pss_sha256_sign(const uint8_t *n, size_t nlen, const uint8_t *d,
                           size_t dlen, const uint8_t mhash[32],
                           const uint8_t *salt, size_t saltlen, uint8_t *out);

/* ECDSA-P256 verify (rv2 9 phase D). Public key (qx,qy) affine, signature
 * (r,s), 32-byte SHA-256 hash; all big-endian. 1 valid, 0 otherwise. */
int wo_ecdsa_p256_sha256_verify(const uint8_t qx[32], const uint8_t qy[32],
                                const uint8_t r[32], const uint8_t s[32],
                                const uint8_t hash[32]);

/* ECDSA-P256 SIGN over SHA-256 with an RFC 6979 deterministic nonce (rv2 9
 * phase G1b). Private scalar d + 32-byte hash in; (r,s) big-endian out. The
 * secret-dependent scalar mult and inversions are constant-time. 0 ok, -1. */
int wo_ecdsa_p256_sha256_sign(const uint8_t d[32], const uint8_t hash[32],
                              uint8_t r_out[32], uint8_t s_out[32]);

/* X.509 / ASN.1 DER (rv2 9 phase E, core). Internal C consumed by the TLS
 * handshake. key_alg / return values use the WO_X509_* enums in crypto.c
 * (RSA = 1, EC_P256 = 2). */
int wo_x509_verify_one(const uint8_t *cert_der, size_t cert_len,
                       const uint8_t *issuer_der, size_t issuer_len);
int wo_x509_parse_spki(const uint8_t *cert_der, size_t cert_len, int *key_alg,
                       const uint8_t **rsa_n, size_t *rsa_n_len,
                       const uint8_t **rsa_e, size_t *rsa_e_len,
                       const uint8_t **ec_x, const uint8_t **ec_y);
int wo_x509_check_validity(const uint8_t *cert_der, size_t cert_len,
                           const char now14[14]);
/* Match hostname against the cert's subjectAltName dNSNames (RFC 6125, single
 * left-most wildcard). 1 match, 0 otherwise (no SAN => 0; no CN fallback). */
int wo_x509_check_host(const uint8_t *cert_der, size_t cert_len,
                       const char *hostname, size_t hostlen);
/* basicConstraints: *is_ca set from cA (absent extension => 0); when a
 * pathLenConstraint is present, *has_pathlen=1 and *pathlen its value. 0 ok,
 * -1 malformed. (rv2 9 F3c decision 6) */
int wo_x509_basic_constraints(const uint8_t *cert_der, size_t cert_len,
                              int *is_ca, int *has_pathlen, int *pathlen);
/* Extended Key Usage: 1 if usable as a TLS server cert (EKU absent, or lists
 * serverAuth / anyExtendedKeyUsage), 0 otherwise. (rv2 9 F3c decision 6) */
int wo_x509_eku_serverauth_ok(const uint8_t *cert_der, size_t cert_len);
/* Parse a DER private key (PKCS#8 / PKCS#1 RSAPrivateKey / SEC1 ECPrivateKey)
 * into RSA (n,d) or an EC P-256 32-byte scalar. Spans point into der_buf.
 * *key_alg gets 1 (RSA) or 2 (EC P-256). 0 ok, -1 malformed/unsupported. */
int wo_pkey_parse(const uint8_t *der_buf, size_t len, int *key_alg,
                  const uint8_t **rsa_n, size_t *rsa_nlen,
                  const uint8_t **rsa_d, size_t *rsa_dlen, const uint8_t **ec_d);

int wo_builtin_crypto(wo_vm *vm, uint64_t *R, uint32_t ins, const char **msg);

#endif

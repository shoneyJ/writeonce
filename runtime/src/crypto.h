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
int wo_aes_gcm_seal(const uint8_t *key, size_t keylen, const uint8_t nonce[12],
                    const uint8_t *aad, size_t aadlen, const uint8_t *pt,
                    size_t ptlen, uint8_t *out);
int wo_aes_gcm_open(const uint8_t *key, size_t keylen, const uint8_t nonce[12],
                    const uint8_t *aad, size_t aadlen, const uint8_t *ct,
                    size_t ctlen, const uint8_t tag[16], uint8_t *out);

int wo_builtin_crypto(wo_vm *vm, uint64_t *R, uint32_t ins, const char **msg);

#endif

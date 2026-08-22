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

int wo_builtin_crypto(wo_vm *vm, uint64_t *R, uint32_t ins, const char **msg);

#endif

/* crypto.c — SHA-1, SHA-256, HMAC-SHA256 (iteration 34). Hand-rolled,
 * libc-only, whole-value: init/update/final collapsed into one pass over
 * one buffer, because every builtin here takes complete Bytes — there is
 * no streaming surface. Vectors: RFC 3174, FIPS 180-4, RFC 4231 — pinned
 * in test/test_crypto.c. SHA-1 exists for the WebSocket handshake
 * (Sec-WebSocket-Accept is SHA-1 by RFC 6455, not a choice). */
#include "crypto.h"

#include <string.h>

#include "obj.h"
#include "wob.h"

static uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

/* Both digests consume the message in 64-byte blocks with the same
 * padding scheme (0x80, zeros, 64-bit big-endian bit length). The tail
 * is at most two blocks; building it on the stack keeps the cores
 * allocation-free. */

static void sha1_block(uint32_t h[5], const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
               (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t t = rotl32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl32(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

void wo_sha1(const uint8_t *msg, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                     0xC3D2E1F0u};
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha1_block(h, msg + i);
    uint8_t tail[128];
    size_t rem = len - i;
    memcpy(tail, msg + i, rem);
    tail[rem] = 0x80;
    size_t tlen = rem + 1 <= 56 ? 64 : 128;
    memset(tail + rem + 1, 0, tlen - rem - 1 - 8);
    uint64_t bits = (uint64_t)len * 8;
    for (int b = 0; b < 8; b++) tail[tlen - 1 - b] = (uint8_t)(bits >> (8 * b));
    sha1_block(h, tail);
    if (tlen == 128) sha1_block(h, tail + 64);
    for (int w = 0; w < 5; w++) {
        out[w * 4] = (uint8_t)(h[w] >> 24);
        out[w * 4 + 1] = (uint8_t)(h[w] >> 16);
        out[w * 4 + 2] = (uint8_t)(h[w] >> 8);
        out[w * 4 + 3] = (uint8_t)h[w];
    }
}

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
               (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

void wo_sha256(const uint8_t *msg, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha256_block(h, msg + i);
    uint8_t tail[128];
    size_t rem = len - i;
    memcpy(tail, msg + i, rem);
    tail[rem] = 0x80;
    size_t tlen = rem + 1 <= 56 ? 64 : 128;
    memset(tail + rem + 1, 0, tlen - rem - 1 - 8);
    uint64_t bits = (uint64_t)len * 8;
    for (int b = 0; b < 8; b++) tail[tlen - 1 - b] = (uint8_t)(bits >> (8 * b));
    sha256_block(h, tail);
    if (tlen == 128) sha256_block(h, tail + 64);
    for (int w = 0; w < 8; w++) {
        out[w * 4] = (uint8_t)(h[w] >> 24);
        out[w * 4 + 1] = (uint8_t)(h[w] >> 16);
        out[w * 4 + 2] = (uint8_t)(h[w] >> 8);
        out[w * 4 + 3] = (uint8_t)h[w];
    }
}

/* RFC 2104 over SHA-256: a key longer than the 64-byte block is hashed
 * first; shorter keys zero-pad. Two passes, no allocation. */
void wo_hmac_sha256(const uint8_t *key, size_t klen, const uint8_t *msg,
                    size_t mlen, uint8_t out[32]) {
    uint8_t k[64] = {0};
    if (klen > 64) {
        wo_sha256(key, klen, k); /* leaves 32 bytes, rest stays zero */
    } else {
        memcpy(k, key, klen);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    /* inner = sha256(ipad || msg) — the message can be arbitrarily long, so
     * the inner pass re-runs the block loop by hand instead of concatenating */
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    sha256_block(h, ipad);
    size_t i = 0;
    for (; i + 64 <= mlen; i += 64) sha256_block(h, msg + i);
    uint8_t tail[128];
    size_t rem = mlen - i;
    memcpy(tail, msg + i, rem);
    tail[rem] = 0x80;
    size_t tlen = rem + 1 <= 56 ? 64 : 128;
    memset(tail + rem + 1, 0, tlen - rem - 1 - 8);
    uint64_t bits = ((uint64_t)mlen + 64) * 8; /* +64: the ipad block */
    for (int b = 0; b < 8; b++) tail[tlen - 1 - b] = (uint8_t)(bits >> (8 * b));
    sha256_block(h, tail);
    if (tlen == 128) sha256_block(h, tail + 64);
    uint8_t inner[32];
    for (int w = 0; w < 8; w++) {
        inner[w * 4] = (uint8_t)(h[w] >> 24);
        inner[w * 4 + 1] = (uint8_t)(h[w] >> 16);
        inner[w * 4 + 2] = (uint8_t)(h[w] >> 8);
        inner[w * 4 + 3] = (uint8_t)h[w];
    }
    uint8_t outer[96];
    memcpy(outer, opad, 64);
    memcpy(outer + 64, inner, 32);
    wo_sha256(outer, 96, out);
}

/* The VM half: Bytes in, fresh Bytes out. Wrong class id traps
 * WO_T_BOUNDS with the Bytes builtins' message shape. */
static const wo_str *arg_bytes(uint64_t r, const char **msg) {
    const wo_str *b = (const wo_str *)(uintptr_t)r;
    if (!b || b->h.class_id != WO_CLS_BYTES) {
        *msg = "not a bytes value";
        return NULL;
    }
    return b;
}

int wo_builtin_crypto(wo_vm *vm, uint64_t *R, uint32_t ins, const char **msg) {
    wo_rt *rt = &vm->rt;
    uint8_t A = wo_ins_a(ins), B = wo_ins_b(ins), C = wo_ins_c(ins);
    uint8_t digest[32];
    uint32_t dlen;
    switch (C) {
    case WO_B_SHA1: {
        const wo_str *b = arg_bytes(R[B], msg);
        if (!b) return WO_T_BOUNDS;
        wo_sha1((const uint8_t *)b->data, b->len, digest);
        dlen = 20;
        break;
    }
    case WO_B_SHA256: {
        const wo_str *b = arg_bytes(R[B], msg);
        if (!b) return WO_T_BOUNDS;
        wo_sha256((const uint8_t *)b->data, b->len, digest);
        dlen = 32;
        break;
    }
    case WO_B_HMAC_SHA256: {
        const wo_str *k = arg_bytes(R[B], msg);
        const wo_str *m = k ? arg_bytes(R[B + 1], msg) : NULL;
        if (!m) return WO_T_BOUNDS;
        wo_hmac_sha256((const uint8_t *)k->data, k->len,
                       (const uint8_t *)m->data, m->len, digest);
        dlen = 32;
        break;
    }
    default:
        *msg = "unknown crypto builtin";
        return WO_T_BOUNDS;
    }
    wo_str *out = wo_bytes_new(rt, (const char *)digest, dlen);
    if (!out) {
        *msg = "out of memory";
        return WO_T_OOM;
    }
    R[A] = (uint64_t)(uintptr_t)out;
    return 0;
}

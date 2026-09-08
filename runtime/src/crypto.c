/* crypto.c — SHA-1, SHA-256, HMAC-SHA256 (iteration 34). Hand-rolled,
 * libc-only, whole-value: init/update/final collapsed into one pass over
 * one buffer, because every builtin here takes complete Bytes — there is
 * no streaming surface. Vectors: RFC 3174, FIPS 180-4, RFC 4231 — pinned
 * in test/test_crypto.c. SHA-1 exists for the WebSocket handshake
 * (Sec-WebSocket-Accept is SHA-1 by RFC 6455, not a choice). */
#include "crypto.h"

#include <stdlib.h>
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

/* ---- ChaCha20-Poly1305 AEAD (rv2 8 phase A, RFC 8439) ------------------
 * Hand-rolled, libc-only, constant-time by construction (add/xor/rotate and
 * limb arithmetic; no data-dependent branches, no table lookups). The
 * reference is RFC 8439; the paper .dev/reference/cryptography-06-00030.pdf
 * describes the same algorithm. Vectors pinned in test/test_crypto.c. */

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static void wr32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr64le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

#define CHACHA_QR(x, a, b, c, d)                                            \
    do {                                                                   \
        x[a] += x[b]; x[d] ^= x[a]; x[d] = rotl32(x[d], 16);               \
        x[c] += x[d]; x[b] ^= x[c]; x[b] = rotl32(x[b], 12);               \
        x[a] += x[b]; x[d] ^= x[a]; x[d] = rotl32(x[d], 8);                \
        x[c] += x[d]; x[b] ^= x[c]; x[b] = rotl32(x[b], 7);                \
    } while (0)

static void chacha20_block(const uint8_t key[32], uint32_t counter,
                           const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t s[16], x[16];
    s[0] = 0x61707865u; s[1] = 0x3320646eu;
    s[2] = 0x79622d32u; s[3] = 0x6b206574u;
    for (int i = 0; i < 8; i++) s[4 + i] = rd32le(key + 4 * i);
    s[12] = counter;
    s[13] = rd32le(nonce); s[14] = rd32le(nonce + 4); s[15] = rd32le(nonce + 8);
    for (int i = 0; i < 16; i++) x[i] = s[i];
    for (int i = 0; i < 10; i++) {
        CHACHA_QR(x, 0, 4, 8, 12); CHACHA_QR(x, 1, 5, 9, 13);
        CHACHA_QR(x, 2, 6, 10, 14); CHACHA_QR(x, 3, 7, 11, 15);
        CHACHA_QR(x, 0, 5, 10, 15); CHACHA_QR(x, 1, 6, 11, 12);
        CHACHA_QR(x, 2, 7, 8, 13); CHACHA_QR(x, 3, 4, 9, 14);
    }
    for (int i = 0; i < 16; i++) wr32le(out + 4 * i, x[i] + s[i]);
}

/* XOR the ChaCha20 keystream (from `counter`) over `len` bytes. in==out safe. */
static void chacha20_xor(const uint8_t key[32], const uint8_t nonce[12],
                         uint32_t counter, const uint8_t *in, size_t len,
                         uint8_t *out) {
    uint8_t blk[64];
    size_t off = 0;
    while (len > 0) {
        chacha20_block(key, counter, nonce, blk);
        size_t n = len < 64 ? len : 64;
        for (size_t i = 0; i < n; i++) out[off + i] = in[off + i] ^ blk[i];
        off += n; len -= n; counter++;
    }
}

/* Poly1305 one-shot (poly1305-donna 32-bit, RFC 8439 §2.5). key = r||s. */
void wo_poly1305(const uint8_t key[32], const uint8_t *m, size_t bytes,
                 uint8_t mac[16]) {
    uint32_t t0 = rd32le(key), t1 = rd32le(key + 4),
             t2 = rd32le(key + 8), t3 = rd32le(key + 12);
    uint32_t r0 = t0 & 0x3ffffffu;
    uint32_t r1 = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03u;
    uint32_t r2 = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ffu;
    uint32_t r3 = ((t2 >> 14) | (t3 << 18)) & 0x3f03fffu;
    uint32_t r4 = (t3 >> 8) & 0x00fffffu;
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0, c;

    while (bytes > 0) {
        uint8_t block[16];
        size_t n = bytes < 16 ? bytes : 16;
        uint32_t hibit;
        if (n < 16) {
            memset(block, 0, 16);
            memcpy(block, m, n);
            block[n] = 1;
            hibit = 0;
        } else {
            memcpy(block, m, 16);
            hibit = 1u << 24;
        }
        t0 = rd32le(block); t1 = rd32le(block + 4);
        t2 = rd32le(block + 8); t3 = rd32le(block + 12);
        h0 += t0 & 0x3ffffffu;
        h1 += ((t0 >> 26) | (t1 << 6)) & 0x3ffffffu;
        h2 += ((t1 >> 20) | (t2 << 12)) & 0x3ffffffu;
        h3 += ((t2 >> 14) | (t3 << 18)) & 0x3ffffffu;
        h4 += (t3 >> 8) | hibit;

        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 +
                      (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 +
                      (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 +
                      (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 +
                      (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 +
                      (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffffu;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffffu;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffffu;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffffu;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffffu;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffffu; h1 += c;

        m += n; bytes -= n;
    }

    c = h1 >> 26; h1 &= 0x3ffffffu; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffffu; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffffu; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffffu; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffffu; h1 += c;

    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffffu;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffffu;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffffu;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffffu;
    uint32_t g4 = h4 + c - (1u << 26);

    uint32_t mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;

    h0 = (h0 | (h1 << 26));
    h1 = ((h1 >> 6) | (h2 << 20));
    h2 = ((h2 >> 12) | (h3 << 14));
    h3 = ((h3 >> 18) | (h4 << 8));

    uint64_t f = (uint64_t)h0 + rd32le(key + 16); h0 = (uint32_t)f;
    f = (uint64_t)h1 + rd32le(key + 20) + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + rd32le(key + 24) + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + rd32le(key + 28) + (f >> 32); h3 = (uint32_t)f;

    wr32le(mac, h0); wr32le(mac + 4, h1); wr32le(mac + 8, h2); wr32le(mac + 12, h3);
}

static int ct_memeq(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

/* The AEAD MAC: Poly1305 over aad || pad16 || ct || pad16 || le64(aadlen) ||
 * le64(ctlen). Returns 0, or -1 on OOM building the (16-aligned) buffer. */
static int aead_tag(const uint8_t polykey[32], const uint8_t *aad, size_t aadlen,
                    const uint8_t *ct, size_t ctlen, uint8_t tag[16]) {
    size_t apad = (aadlen + 15u) & ~(size_t)15u;
    size_t cpad = (ctlen + 15u) & ~(size_t)15u;
    size_t mlen = apad + cpad + 16u;
    uint8_t *mb = (uint8_t *)calloc(1, mlen);
    if (!mb) return -1;
    if (aadlen) memcpy(mb, aad, aadlen);
    if (ctlen) memcpy(mb + apad, ct, ctlen);
    wr64le(mb + apad + cpad, (uint64_t)aadlen);
    wr64le(mb + apad + cpad + 8, (uint64_t)ctlen);
    wo_poly1305(polykey, mb, mlen, tag);
    free(mb);
    return 0;
}

/* RFC 8439 §2.8 seal: out = ciphertext || 16-byte tag (out must hold
 * ptlen+16). Returns 0, or -1 on OOM. */
int wo_chacha20poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
                             const uint8_t *aad, size_t aadlen,
                             const uint8_t *pt, size_t ptlen, uint8_t *out) {
    uint8_t polyblock[64];
    chacha20_block(key, 0, nonce, polyblock); /* Poly1305 key = counter-0 block */
    chacha20_xor(key, nonce, 1, pt, ptlen, out);
    return aead_tag(polyblock, aad, aadlen, out, ptlen, out + ptlen);
}

/* Open: verify the tag over `ct` (ctlen, the ciphertext WITHOUT the tag) and
 * `tag`, then decrypt into `out` (ctlen bytes). 0 = ok, 1 = auth failure,
 * -1 = OOM. Constant-time tag compare; on failure `out` is not written. */
int wo_chacha20poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
                             const uint8_t *aad, size_t aadlen,
                             const uint8_t *ct, size_t ctlen,
                             const uint8_t tag[16], uint8_t *out) {
    uint8_t polyblock[64], want[16];
    chacha20_block(key, 0, nonce, polyblock);
    if (aead_tag(polyblock, aad, aadlen, ct, ctlen, want) != 0) return -1;
    if (!ct_memeq(want, tag, 16)) return 1;
    chacha20_xor(key, nonce, 1, ct, ctlen, out);
    return 0;
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
    case WO_B_CHACHA20POLY1305_SEAL: {
        const wo_str *k = arg_bytes(R[B], msg);
        const wo_str *n = k ? arg_bytes(R[B + 1], msg) : NULL;
        const wo_str *a = n ? arg_bytes(R[B + 2], msg) : NULL;
        const wo_str *p = a ? arg_bytes(R[B + 3], msg) : NULL;
        if (!p) return WO_T_BOUNDS;
        if (k->len != 32 || n->len != 12) {
            *msg = "chacha20poly1305: key must be 32 bytes, nonce 12";
            return WO_T_BOUNDS;
        }
        uint8_t *buf = (uint8_t *)malloc(p->len + 16u);
        if (!buf) { *msg = "out of memory"; return WO_T_OOM; }
        if (wo_chacha20poly1305_seal((const uint8_t *)k->data,
                                     (const uint8_t *)n->data,
                                     (const uint8_t *)a->data, a->len,
                                     (const uint8_t *)p->data, p->len, buf) != 0) {
            free(buf);
            *msg = "out of memory";
            return WO_T_OOM;
        }
        wo_str *o = wo_bytes_new(rt, (const char *)buf, (uint32_t)(p->len + 16u));
        free(buf);
        if (!o) { *msg = "out of memory"; return WO_T_OOM; }
        R[A] = (uint64_t)(uintptr_t)o;
        return 0;
    }
    case WO_B_CHACHA20POLY1305_OPEN: {
        const wo_str *k = arg_bytes(R[B], msg);
        const wo_str *n = k ? arg_bytes(R[B + 1], msg) : NULL;
        const wo_str *a = n ? arg_bytes(R[B + 2], msg) : NULL;
        const wo_str *ctag = a ? arg_bytes(R[B + 3], msg) : NULL;
        if (!ctag) return WO_T_BOUNDS;
        if (k->len != 32 || n->len != 12) {
            *msg = "chacha20poly1305: key must be 32 bytes, nonce 12";
            return WO_T_BOUNDS;
        }
        if (ctag->len < 16) { R[A] = 0; return 0; } /* no room for a tag: reject */
        uint32_t bodylen = ctag->len - 16u;
        uint8_t *buf = (uint8_t *)malloc(bodylen ? bodylen : 1u);
        if (!buf) { *msg = "out of memory"; return WO_T_OOM; }
        int rc = wo_chacha20poly1305_open(
            (const uint8_t *)k->data, (const uint8_t *)n->data,
            (const uint8_t *)a->data, a->len,
            (const uint8_t *)ctag->data, bodylen,
            (const uint8_t *)ctag->data + bodylen, buf);
        if (rc == -1) { free(buf); *msg = "out of memory"; return WO_T_OOM; }
        if (rc != 0) { free(buf); R[A] = 0; return 0; } /* auth failure -> nil */
        wo_str *o = wo_bytes_new(rt, (const char *)buf, bodylen);
        free(buf);
        if (!o) { *msg = "out of memory"; return WO_T_OOM; }
        R[A] = (uint64_t)(uintptr_t)o;
        return 0;
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

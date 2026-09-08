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

/* ---- AES-GCM via AES-NI + PCLMULQDQ (rv2 8 phase B, x86-64 hardware path) --
 * Constant-time by hardware (no tables, no data-dependent branches). The
 * functions carry target attributes so the binary still runs on CPUs without
 * the extensions; aes_gcm_available() gates entry (the bitsliced software
 * fallback is phase C). Refs: Intel AES-NI + carry-less-multiplication
 * whitepapers, NIST SP 800-38D. Vectors: NIST/RFC in test/test_crypto.c. */
#if defined(__x86_64__)
#include <wmmintrin.h>
#include <emmintrin.h>
#include <tmmintrin.h>

int wo_aes_gcm_available(void) {
    return __builtin_cpu_supports("aes") && __builtin_cpu_supports("pclmul") &&
           __builtin_cpu_supports("ssse3");
}

#define AES128_ASSIST(t1, t2)                                                  \
    do {                                                                       \
        __m128i _t3;                                                           \
        t2 = _mm_shuffle_epi32(t2, 0xff);                                      \
        _t3 = _mm_slli_si128(t1, 4); t1 = _mm_xor_si128(t1, _t3);              \
        _t3 = _mm_slli_si128(_t3, 4); t1 = _mm_xor_si128(t1, _t3);             \
        _t3 = _mm_slli_si128(_t3, 4); t1 = _mm_xor_si128(t1, _t3);             \
        t1 = _mm_xor_si128(t1, t2);                                            \
    } while (0)

__attribute__((target("aes,sse2")))
static void aes128_expand(const uint8_t *key, __m128i rk[11]) {
    __m128i t1 = _mm_loadu_si128((const __m128i *)key), t2;
    rk[0] = t1;
    t2 = _mm_aeskeygenassist_si128(t1, 0x01); AES128_ASSIST(t1, t2); rk[1] = t1;
    t2 = _mm_aeskeygenassist_si128(t1, 0x02); AES128_ASSIST(t1, t2); rk[2] = t1;
    t2 = _mm_aeskeygenassist_si128(t1, 0x04); AES128_ASSIST(t1, t2); rk[3] = t1;
    t2 = _mm_aeskeygenassist_si128(t1, 0x08); AES128_ASSIST(t1, t2); rk[4] = t1;
    t2 = _mm_aeskeygenassist_si128(t1, 0x10); AES128_ASSIST(t1, t2); rk[5] = t1;
    t2 = _mm_aeskeygenassist_si128(t1, 0x20); AES128_ASSIST(t1, t2); rk[6] = t1;
    t2 = _mm_aeskeygenassist_si128(t1, 0x40); AES128_ASSIST(t1, t2); rk[7] = t1;
    t2 = _mm_aeskeygenassist_si128(t1, 0x80); AES128_ASSIST(t1, t2); rk[8] = t1;
    t2 = _mm_aeskeygenassist_si128(t1, 0x1b); AES128_ASSIST(t1, t2); rk[9] = t1;
    t2 = _mm_aeskeygenassist_si128(t1, 0x36); AES128_ASSIST(t1, t2); rk[10] = t1;
}

__attribute__((target("aes,sse2")))
static void aes256_assist1(__m128i *t1, __m128i *t2) {
    __m128i t4;
    *t2 = _mm_shuffle_epi32(*t2, 0xff);
    t4 = _mm_slli_si128(*t1, 4); *t1 = _mm_xor_si128(*t1, t4);
    t4 = _mm_slli_si128(t4, 4); *t1 = _mm_xor_si128(*t1, t4);
    t4 = _mm_slli_si128(t4, 4); *t1 = _mm_xor_si128(*t1, t4);
    *t1 = _mm_xor_si128(*t1, *t2);
}
__attribute__((target("aes,sse2")))
static void aes256_assist2(__m128i *t1, __m128i *t3) {
    __m128i t2, t4;
    t4 = _mm_aeskeygenassist_si128(*t1, 0x00);
    t2 = _mm_shuffle_epi32(t4, 0xaa);
    t4 = _mm_slli_si128(*t3, 4); *t3 = _mm_xor_si128(*t3, t4);
    t4 = _mm_slli_si128(t4, 4); *t3 = _mm_xor_si128(*t3, t4);
    t4 = _mm_slli_si128(t4, 4); *t3 = _mm_xor_si128(*t3, t4);
    *t3 = _mm_xor_si128(*t3, t2);
}
/* rcon must be a compile-time immediate to aeskeygenassist, so the schedule
 * is unrolled rather than looped over an rcon array. */
#define AES256_STEP(RC)                                                        \
    do {                                                                       \
        t2 = _mm_aeskeygenassist_si128(t3, (RC));                              \
        aes256_assist1(&t1, &t2); rk[k++] = t1;                                \
        aes256_assist2(&t1, &t3); rk[k++] = t3;                                \
    } while (0)

__attribute__((target("aes,sse2")))
static void aes256_expand(const uint8_t *key, __m128i rk[15]) {
    __m128i t1 = _mm_loadu_si128((const __m128i *)key);
    __m128i t3 = _mm_loadu_si128((const __m128i *)(key + 16));
    __m128i t2;
    int k = 2;
    rk[0] = t1; rk[1] = t3;
    AES256_STEP(0x01); AES256_STEP(0x02); AES256_STEP(0x04);
    AES256_STEP(0x08); AES256_STEP(0x10); AES256_STEP(0x20);
    t2 = _mm_aeskeygenassist_si128(t3, 0x40);
    aes256_assist1(&t1, &t2); rk[k] = t1; /* rk[14] */
}

__attribute__((target("aes")))
static __m128i aes_enc(const __m128i *rk, int nr, __m128i m) {
    m = _mm_xor_si128(m, rk[0]);
    for (int i = 1; i < nr; i++) m = _mm_aesenc_si128(m, rk[i]);
    return _mm_aesenclast_si128(m, rk[nr]);
}

/* Carry-less multiply in GF(2^128) with the GCM reduction, operands in the
 * byte-reversed domain (Intel CLMUL whitepaper gfmul + fast reduction). */
__attribute__((target("pclmul,sse2")))
static __m128i gfmul(__m128i a, __m128i b) {
    __m128i t3, t4, t5, t6, t7, t8, t9, t2;
    t3 = _mm_clmulepi64_si128(a, b, 0x00);
    t4 = _mm_clmulepi64_si128(a, b, 0x10);
    t5 = _mm_clmulepi64_si128(a, b, 0x01);
    t6 = _mm_clmulepi64_si128(a, b, 0x11);
    t4 = _mm_xor_si128(t4, t5);
    t5 = _mm_slli_si128(t4, 8);
    t4 = _mm_srli_si128(t4, 8);
    t3 = _mm_xor_si128(t3, t5);
    t6 = _mm_xor_si128(t6, t4);
    t7 = _mm_srli_epi32(t3, 31);
    t8 = _mm_srli_epi32(t6, 31);
    t3 = _mm_slli_epi32(t3, 1);
    t6 = _mm_slli_epi32(t6, 1);
    t9 = _mm_srli_si128(t7, 12);
    t8 = _mm_slli_si128(t8, 4);
    t7 = _mm_slli_si128(t7, 4);
    t3 = _mm_or_si128(t3, t7);
    t6 = _mm_or_si128(t6, t8);
    t6 = _mm_or_si128(t6, t9);
    t7 = _mm_slli_epi32(t3, 31);
    t8 = _mm_slli_epi32(t3, 30);
    t9 = _mm_slli_epi32(t3, 25);
    t7 = _mm_xor_si128(t7, t8);
    t7 = _mm_xor_si128(t7, t9);
    t8 = _mm_srli_si128(t7, 4);
    t7 = _mm_slli_si128(t7, 12);
    t3 = _mm_xor_si128(t3, t7);
    t2 = _mm_srli_epi32(t3, 1);
    t4 = _mm_srli_epi32(t3, 2);
    t5 = _mm_srli_epi32(t3, 7);
    t2 = _mm_xor_si128(t2, t4);
    t2 = _mm_xor_si128(t2, t5);
    t2 = _mm_xor_si128(t2, t8);
    t3 = _mm_xor_si128(t3, t2);
    t6 = _mm_xor_si128(t6, t3);
    return t6;
}

/* GHASH `T = (T ^ block)·H` over full+partial 16-byte blocks (bswap domain). */
__attribute__((target("pclmul,ssse3")))
static __m128i ghash(__m128i T, __m128i H, const uint8_t *data, size_t len,
                     __m128i bswap) {
    size_t off = 0;
    while (len - off >= 16) {
        __m128i b = _mm_loadu_si128((const __m128i *)(data + off));
        b = _mm_shuffle_epi8(b, bswap);
        T = gfmul(_mm_xor_si128(T, b), H);
        off += 16;
    }
    if (off < len) {
        uint8_t last[16];
        memset(last, 0, 16);
        memcpy(last, data + off, len - off);
        __m128i b = _mm_loadu_si128((const __m128i *)last);
        b = _mm_shuffle_epi8(b, bswap);
        T = gfmul(_mm_xor_si128(T, b), H);
    }
    return T;
}

/* GCM core (encrypt==1 seals, 0 opens). On open, `tag_in` is compared
 * constant-time; returns 0 ok, 1 auth failure. On seal, writes tag_out. */
__attribute__((target("aes,pclmul,ssse3")))
static int aes_gcm_core(const uint8_t *key, size_t keylen, const uint8_t nonce[12],
                        const uint8_t *aad, size_t aadlen, const uint8_t *in,
                        size_t inlen, uint8_t *out, uint8_t tag_out[16],
                        const uint8_t *tag_in) {
    __m128i rk[15];
    int nr;
    if (keylen == 16) { aes128_expand(key, rk); nr = 10; }
    else { aes256_expand(key, rk); nr = 14; }
    const __m128i bswap =
        _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);

    __m128i H = aes_enc(rk, nr, _mm_setzero_si128());
    H = _mm_shuffle_epi8(H, bswap); /* reflect H once */

    uint8_t j0[16];
    memcpy(j0, nonce, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    __m128i ej0 = aes_enc(rk, nr, _mm_loadu_si128((const __m128i *)j0));

    /* GHASH over aad || pad || ciphertext || pad || len-block.
     * On seal the ciphertext is what we produce; on open it is the input. */
    __m128i T = _mm_setzero_si128();
    T = ghash(T, H, aad, aadlen, bswap);
    if (!tag_in) {
        /* seal: CTR-encrypt from counter 2, then GHASH the produced ct */
    }

    /* CTR: counter starts at 2 (inc32(J0)); build blocks from nonce||BE32 */
    uint32_t ctr = 2;
    size_t off = 0;
    /* For open, GHASH the input ciphertext first (before we overwrite via out) */
    if (tag_in) T = ghash(T, H, in, inlen, bswap);
    while (off < inlen) {
        uint8_t cb[16];
        memcpy(cb, nonce, 12);
        cb[12] = (uint8_t)(ctr >> 24); cb[13] = (uint8_t)(ctr >> 16);
        cb[14] = (uint8_t)(ctr >> 8); cb[15] = (uint8_t)ctr;
        __m128i ks = aes_enc(rk, nr, _mm_loadu_si128((const __m128i *)cb));
        uint8_t ksb[16];
        _mm_storeu_si128((__m128i *)ksb, ks);
        size_t n = inlen - off < 16 ? inlen - off : 16;
        for (size_t i = 0; i < n; i++) out[off + i] = in[off + i] ^ ksb[i];
        off += n; ctr++;
    }
    if (!tag_in) T = ghash(T, H, out, inlen, bswap); /* seal: GHASH the ct */

    uint8_t lb[16];
    uint64_t aBits = (uint64_t)aadlen * 8, cBits = (uint64_t)inlen * 8;
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(aBits >> (56 - 8 * i));
    for (int i = 0; i < 8; i++) lb[8 + i] = (uint8_t)(cBits >> (56 - 8 * i));
    T = ghash(T, H, lb, 16, bswap);

    T = _mm_shuffle_epi8(T, bswap); /* back to big-endian bytes */
    __m128i tagv = _mm_xor_si128(T, ej0);
    uint8_t tag[16];
    _mm_storeu_si128((__m128i *)tag, tagv);

    if (tag_in) {
        uint8_t d = 0;
        for (int i = 0; i < 16; i++) d |= (uint8_t)(tag[i] ^ tag_in[i]);
        return d == 0 ? 0 : 1;
    }
    memcpy(tag_out, tag, 16);
    return 0;
}
#else
int wo_aes_gcm_available(void) { return 0; }
#endif

/* ---- Portable constant-time AES-GCM (rv2 8 phase C software fallback) ------
 * No intrinsics, no lookup tables: the S-box is the GF(2^8) inverse via a
 * fixed-exponent power ladder (constant-time in the input), GHASH is the
 * bit-by-bit GF(2^128) multiply (constant-time, mask-driven). Slower than the
 * AES-NI path; its job is portability and side-channel safety, not speed. The
 * ARMv8 crypto-extension hardware path is deferred (untestable on the x86-64
 * dev host). */

int wo_aes_force_software = 0; /* test hook: force the software path */

static uint8_t gf8_mul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        p ^= (uint8_t)(-(b & 1)) & a;
        uint8_t hi = (uint8_t)(-((a >> 7) & 1));
        a = (uint8_t)((a << 1)) ^ (uint8_t)(hi & 0x1b);
        b = (uint8_t)(b >> 1);
    }
    return p;
}

static uint8_t aes_sbox_ct(uint8_t x) {
    /* inverse = x^254 in GF(2^8); the exponent is a compile-time constant so
     * the ladder is a fixed op sequence — constant-time in x. inv(0) = 0. */
    uint8_t base = x, inv = 1;
    for (int i = 0; i < 8; i++) {
        if ((254u >> i) & 1u) inv = gf8_mul(inv, base);
        base = gf8_mul(base, base);
    }
    /* affine: out_i = inv_i ^ inv_{i+4} ^ inv_{i+5} ^ inv_{i+6} ^ inv_{i+7}, ^0x63 */
    uint8_t s = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t bit = (uint8_t)(((inv >> i) ^ (inv >> ((i + 4) & 7)) ^
                                 (inv >> ((i + 5) & 7)) ^ (inv >> ((i + 6) & 7)) ^
                                 (inv >> ((i + 7) & 7))) & 1u);
        s |= (uint8_t)(bit << i);
    }
    return (uint8_t)(s ^ 0x63);
}

static uint32_t subword(uint32_t w) {
    return ((uint32_t)aes_sbox_ct(w >> 24) << 24) |
           ((uint32_t)aes_sbox_ct((w >> 16) & 0xff) << 16) |
           ((uint32_t)aes_sbox_ct((w >> 8) & 0xff) << 8) |
           (uint32_t)aes_sbox_ct(w & 0xff);
}

static void aes_expand_sw(const uint8_t *key, size_t keylen, uint8_t *rk,
                          int *nr_out) {
    int nk = (int)(keylen / 4);   /* 4 (AES-128) or 8 (AES-256) */
    int nr = nk + 6;              /* 10 or 14 */
    int total = 4 * (nr + 1);
    uint32_t w[60];
    for (int i = 0; i < nk; i++)
        w[i] = ((uint32_t)key[4 * i] << 24) | ((uint32_t)key[4 * i + 1] << 16) |
               ((uint32_t)key[4 * i + 2] << 8) | (uint32_t)key[4 * i + 3];
    uint8_t rcon = 1;
    for (int i = nk; i < total; i++) {
        uint32_t t = w[i - 1];
        if (i % nk == 0) {
            t = (t << 8) | (t >> 24);                /* RotWord */
            t = subword(t);                          /* SubWord */
            t ^= (uint32_t)rcon << 24;
            rcon = gf8_mul(rcon, 2);
        } else if (nk > 6 && i % nk == 4) {
            t = subword(t);
        }
        w[i] = w[i - nk] ^ t;
    }
    for (int i = 0; i < total; i++) {
        rk[4 * i] = (uint8_t)(w[i] >> 24);
        rk[4 * i + 1] = (uint8_t)(w[i] >> 16);
        rk[4 * i + 2] = (uint8_t)(w[i] >> 8);
        rk[4 * i + 3] = (uint8_t)w[i];
    }
    *nr_out = nr;
}

static void aes_block_sw(const uint8_t *rk, int nr, const uint8_t in[16],
                         uint8_t out[16]) {
    uint8_t s[16];
    memcpy(s, in, 16);
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
    for (int round = 1; round <= nr; round++) {
        for (int i = 0; i < 16; i++) s[i] = aes_sbox_ct(s[i]);
        uint8_t t[16]; /* ShiftRows: state is column-major, byte = row + 4*col */
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) t[r + 4 * c] = s[r + 4 * ((c + r) & 3)];
        memcpy(s, t, 16);
        if (round != nr) {
            for (int c = 0; c < 4; c++) {
                uint8_t *col = s + 4 * c;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = gf8_mul(a0, 2) ^ gf8_mul(a1, 3) ^ a2 ^ a3;
                col[1] = a0 ^ gf8_mul(a1, 2) ^ gf8_mul(a2, 3) ^ a3;
                col[2] = a0 ^ a1 ^ gf8_mul(a2, 2) ^ gf8_mul(a3, 3);
                col[3] = gf8_mul(a0, 3) ^ a1 ^ a2 ^ gf8_mul(a3, 2);
            }
        }
        for (int i = 0; i < 16; i++) s[i] ^= rk[16 * round + i];
    }
    memcpy(out, s, 16);
}

/* GF(2^128) multiply, GCM bit order, constant-time (mask-driven, no tables). */
static void gf128_mul(const uint8_t X[16], const uint8_t Y[16], uint8_t out[16]) {
    uint8_t Z[16] = { 0 }, V[16];
    memcpy(V, Y, 16);
    for (int i = 0; i < 128; i++) {
        uint8_t bit = (uint8_t)((X[i >> 3] >> (7 - (i & 7))) & 1);
        uint8_t m = (uint8_t)(-bit);
        for (int j = 0; j < 16; j++) Z[j] ^= (uint8_t)(V[j] & m);
        uint8_t lsb = (uint8_t)(V[15] & 1);
        for (int j = 15; j > 0; j--)
            V[j] = (uint8_t)((V[j] >> 1) | (V[j - 1] << 7));
        V[0] = (uint8_t)(V[0] >> 1);
        V[0] ^= (uint8_t)(0xe1 & (uint8_t)(-lsb));
    }
    memcpy(out, Z, 16);
}

static void ghash_sw(const uint8_t H[16], const uint8_t *data, size_t len,
                     uint8_t T[16]) {
    size_t off = 0;
    while (off < len) {
        uint8_t blk[16];
        size_t n = len - off < 16 ? len - off : 16;
        memset(blk, 0, 16);
        memcpy(blk, data + off, n);
        for (int j = 0; j < 16; j++) T[j] ^= blk[j];
        uint8_t tmp[16];
        gf128_mul(T, H, tmp);
        memcpy(T, tmp, 16);
        off += n;
    }
}

static int aes_gcm_core_sw(const uint8_t *key, size_t keylen,
                           const uint8_t nonce[12], const uint8_t *aad,
                           size_t aadlen, const uint8_t *in, size_t inlen,
                           uint8_t *out, uint8_t tag_out[16],
                           const uint8_t *tag_in) {
    uint8_t rk[15 * 16];
    int nr;
    aes_expand_sw(key, keylen, rk, &nr);
    uint8_t H[16] = { 0 };
    aes_block_sw(rk, nr, H, H); /* H = AES(0) */
    uint8_t j0[16];
    memcpy(j0, nonce, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    uint8_t ej0[16];
    aes_block_sw(rk, nr, j0, ej0);

    uint8_t T[16] = { 0 };
    ghash_sw(H, aad, aadlen, T);
    if (tag_in) ghash_sw(H, in, inlen, T); /* open: GHASH ciphertext first */

    uint32_t ctr = 2;
    size_t off = 0;
    while (off < inlen) {
        uint8_t cb[16];
        memcpy(cb, nonce, 12);
        cb[12] = (uint8_t)(ctr >> 24); cb[13] = (uint8_t)(ctr >> 16);
        cb[14] = (uint8_t)(ctr >> 8); cb[15] = (uint8_t)ctr;
        uint8_t ks[16];
        aes_block_sw(rk, nr, cb, ks);
        size_t n = inlen - off < 16 ? inlen - off : 16;
        for (size_t i = 0; i < n; i++) out[off + i] = in[off + i] ^ ks[i];
        off += n; ctr++;
    }
    if (!tag_in) ghash_sw(H, out, inlen, T); /* seal: GHASH produced ciphertext */

    uint8_t lb[16];
    uint64_t aB = (uint64_t)aadlen * 8, cB = (uint64_t)inlen * 8;
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(aB >> (56 - 8 * i));
    for (int i = 0; i < 8; i++) lb[8 + i] = (uint8_t)(cB >> (56 - 8 * i));
    ghash_sw(H, lb, 16, T);

    uint8_t tag[16];
    for (int i = 0; i < 16; i++) tag[i] = (uint8_t)(T[i] ^ ej0[i]);
    if (tag_in) {
        uint8_t d = 0;
        for (int i = 0; i < 16; i++) d |= (uint8_t)(tag[i] ^ tag_in[i]);
        return d == 0 ? 0 : 1;
    }
    memcpy(tag_out, tag, 16);
    return 0;
}

/* Public seal/open. keylen 16 (AES-128) or 32 (AES-256), nonce 12 bytes.
 * Dispatches to the AES-NI path when available (and not forced software),
 * else the portable constant-time fallback. Returns 0 ok, 1 auth failure. */
int wo_aes_gcm_seal(const uint8_t *key, size_t keylen, const uint8_t nonce[12],
                    const uint8_t *aad, size_t aadlen, const uint8_t *pt,
                    size_t ptlen, uint8_t *out) {
#if defined(__x86_64__)
    if (wo_aes_gcm_available() && !wo_aes_force_software)
        return aes_gcm_core(key, keylen, nonce, aad, aadlen, pt, ptlen, out,
                            out + ptlen, NULL);
#endif
    return aes_gcm_core_sw(key, keylen, nonce, aad, aadlen, pt, ptlen, out,
                           out + ptlen, NULL);
}
int wo_aes_gcm_open(const uint8_t *key, size_t keylen, const uint8_t nonce[12],
                    const uint8_t *aad, size_t aadlen, const uint8_t *ct,
                    size_t ctlen, const uint8_t tag[16], uint8_t *out) {
#if defined(__x86_64__)
    if (wo_aes_gcm_available() && !wo_aes_force_software)
        return aes_gcm_core(key, keylen, nonce, aad, aadlen, ct, ctlen, out,
                            NULL, tag);
#endif
    return aes_gcm_core_sw(key, keylen, nonce, aad, aadlen, ct, ctlen, out, NULL,
                           tag);
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
    case WO_B_AES_GCM_SEAL: {
        const wo_str *k = arg_bytes(R[B], msg);
        const wo_str *n = k ? arg_bytes(R[B + 1], msg) : NULL;
        const wo_str *a = n ? arg_bytes(R[B + 2], msg) : NULL;
        const wo_str *p = a ? arg_bytes(R[B + 3], msg) : NULL;
        if (!p) return WO_T_BOUNDS;
        if ((k->len != 16 && k->len != 32) || n->len != 12) {
            *msg = "aes_gcm: key must be 16 or 32 bytes, nonce 12";
            return WO_T_BOUNDS;
        }
        uint8_t *buf = (uint8_t *)malloc(p->len + 16u);
        if (!buf) { *msg = "out of memory"; return WO_T_OOM; }
        int rc = wo_aes_gcm_seal((const uint8_t *)k->data, k->len,
                                 (const uint8_t *)n->data,
                                 (const uint8_t *)a->data, a->len,
                                 (const uint8_t *)p->data, p->len, buf);
        if (rc == -2) {
            free(buf);
            *msg = "aes_gcm requires hardware AES (AES-NI); software fallback is rv2 8 phase C";
            return WO_T_BOUNDS;
        }
        wo_str *o = wo_bytes_new(rt, (const char *)buf, (uint32_t)(p->len + 16u));
        free(buf);
        if (!o) { *msg = "out of memory"; return WO_T_OOM; }
        R[A] = (uint64_t)(uintptr_t)o;
        return 0;
    }
    case WO_B_AES_GCM_OPEN: {
        const wo_str *k = arg_bytes(R[B], msg);
        const wo_str *n = k ? arg_bytes(R[B + 1], msg) : NULL;
        const wo_str *a = n ? arg_bytes(R[B + 2], msg) : NULL;
        const wo_str *ctag = a ? arg_bytes(R[B + 3], msg) : NULL;
        if (!ctag) return WO_T_BOUNDS;
        if ((k->len != 16 && k->len != 32) || n->len != 12) {
            *msg = "aes_gcm: key must be 16 or 32 bytes, nonce 12";
            return WO_T_BOUNDS;
        }
        if (ctag->len < 16) { R[A] = 0; return 0; }
        uint32_t bodylen = ctag->len - 16u;
        uint8_t *buf = (uint8_t *)malloc(bodylen ? bodylen : 1u);
        if (!buf) { *msg = "out of memory"; return WO_T_OOM; }
        int rc = wo_aes_gcm_open((const uint8_t *)k->data, k->len,
                                 (const uint8_t *)n->data,
                                 (const uint8_t *)a->data, a->len,
                                 (const uint8_t *)ctag->data, bodylen,
                                 (const uint8_t *)ctag->data + bodylen, buf);
        if (rc == -2) {
            free(buf);
            *msg = "aes_gcm requires hardware AES (AES-NI); software fallback is rv2 8 phase C";
            return WO_T_BOUNDS;
        }
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

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

/* ---- HKDF-SHA256 (rv2 9 phase B: the TLS 1.3 key schedule) --------------
 * RFC 5869 (Extract/Expand) + RFC 8446 §7.1 (Expand-Label), built on the
 * existing HMAC-SHA256. Internal C consumed by the TLS handshake; no `.wo`
 * builtin until a `.wo` consumer exists. SHA-256 only — the hash of the
 * mandatory suites (TLS_AES_128_GCM_SHA256, TLS_CHACHA20_POLY1305_SHA256);
 * SHA-384 is a later addition for the AES-256 suite. */

void wo_hkdf_sha256_extract(const uint8_t *salt, size_t saltlen,
                            const uint8_t *ikm, size_t ikmlen, uint8_t prk[32]) {
    uint8_t zero[32] = { 0 };
    if (!salt || saltlen == 0) { salt = zero; saltlen = 32; }
    wo_hmac_sha256(salt, saltlen, ikm, ikmlen, prk);
}

/* OKM = T(1)||T(2)||…, T(i) = HMAC(PRK, T(i-1)||info||i). 0 ok, -1 on a
 * too-long request (>255*32) or OOM. */
int wo_hkdf_sha256_expand(const uint8_t prk[32], const uint8_t *info,
                          size_t infolen, uint8_t *okm, size_t okmlen) {
    if (okmlen > 255u * 32u) return -1;
    uint8_t t[32];
    size_t tlen = 0, done = 0;
    uint8_t counter = 1;
    while (done < okmlen) {
        size_t mlen = tlen + infolen + 1;
        uint8_t *m = (uint8_t *)malloc(mlen ? mlen : 1);
        if (!m) return -1;
        if (tlen) memcpy(m, t, tlen);
        if (infolen) memcpy(m + tlen, info, infolen);
        m[tlen + infolen] = counter;
        wo_hmac_sha256(prk, 32, m, mlen, t);
        free(m);
        tlen = 32;
        size_t n = okmlen - done < 32 ? okmlen - done : 32;
        memcpy(okm + done, t, n);
        done += n; counter++;
    }
    return 0;
}

/* RFC 8446 §7.1: HKDF-Expand-Label(secret, label, context, len) where
 * HkdfLabel = uint16 len || opaque("tls13 "+label) || opaque(context). */
int wo_hkdf_sha256_expand_label(const uint8_t secret[32], const char *label,
                                size_t labellen, const uint8_t *ctx,
                                size_t ctxlen, uint8_t *out, size_t outlen) {
    if (labellen > 249 || ctxlen > 255 || outlen > 65535) return -1;
    uint8_t info[2 + 1 + 255 + 1 + 255];
    size_t p = 0;
    info[p++] = (uint8_t)(outlen >> 8);
    info[p++] = (uint8_t)outlen;
    info[p++] = (uint8_t)(6 + labellen);
    memcpy(info + p, "tls13 ", 6); p += 6;
    memcpy(info + p, label, labellen); p += labellen;
    info[p++] = (uint8_t)ctxlen;
    if (ctxlen) { memcpy(info + p, ctx, ctxlen); p += ctxlen; }
    return wo_hkdf_sha256_expand(secret, info, p, out, outlen);
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

/* ---- X25519 (rv2 9 phase C, RFC 7748) ----------------------------------
 * Montgomery-ladder scalar multiplication over Curve25519, constant-time
 * (mask-based conditional swap, no data-dependent branches). Field arithmetic
 * is the radix-2^51 representation with 128-bit intermediate products
 * (curve25519-donna-c64, public domain). Internal C; the consumer is the TLS
 * ECDHE handshake. Vectors: RFC 7748 §5.2 in test_crypto.c. */
typedef uint64_t felem[5];
typedef unsigned __int128 u128;
#define FE_MASK 0x7ffffffffffffULL

static uint64_t ld64(const uint8_t *b) {
    uint64_t r = 0;
    for (int i = 0; i < 8; i++) r |= (uint64_t)b[i] << (8 * i);
    return r;
}
static void st64(uint8_t *b, uint64_t v) {
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
}

static void fexpand(felem out, const uint8_t *in) {
    out[0] = ld64(in) & FE_MASK;
    out[1] = (ld64(in + 6) >> 3) & FE_MASK;
    out[2] = (ld64(in + 12) >> 6) & FE_MASK;
    out[3] = (ld64(in + 19) >> 1) & FE_MASK;
    out[4] = (ld64(in + 24) >> 12) & FE_MASK;
}

static void fcontract(uint8_t *out, const felem in) {
    felem h;
    for (int i = 0; i < 5; i++) h[i] = in[i];
    for (int r = 0; r < 3; r++) { /* weak-reduce a few times */
        uint64_t c;
        c = h[0] >> 51; h[0] &= FE_MASK; h[1] += c;
        c = h[1] >> 51; h[1] &= FE_MASK; h[2] += c;
        c = h[2] >> 51; h[2] &= FE_MASK; h[3] += c;
        c = h[3] >> 51; h[3] &= FE_MASK; h[4] += c;
        c = h[4] >> 51; h[4] &= FE_MASK; h[0] += 19 * c;
    }
    /* q = 1 iff h >= p = 2^255-19 */
    uint64_t q = (h[0] + 19) >> 51;
    q = (h[1] + q) >> 51; q = (h[2] + q) >> 51;
    q = (h[3] + q) >> 51; q = (h[4] + q) >> 51;
    h[0] += 19 * q;
    h[1] += h[0] >> 51; h[0] &= FE_MASK;
    h[2] += h[1] >> 51; h[1] &= FE_MASK;
    h[3] += h[2] >> 51; h[2] &= FE_MASK;
    h[4] += h[3] >> 51; h[3] &= FE_MASK;
    h[4] &= FE_MASK;
    st64(out, h[0] | (h[1] << 51));
    st64(out + 8, (h[1] >> 13) | (h[2] << 38));
    st64(out + 16, (h[2] >> 26) | (h[3] << 25));
    st64(out + 24, (h[3] >> 39) | (h[4] << 12));
}

static void fsum(felem out, const felem a, const felem b) {
    for (int i = 0; i < 5; i++) out[i] = a[i] + b[i];
}
static void fdiff(felem out, const felem a, const felem b) { /* out = a - b */
    static const uint64_t t54m152 = (1ULL << 54) - 152, t54m8 = (1ULL << 54) - 8;
    out[0] = a[0] + t54m152 - b[0];
    out[1] = a[1] + t54m8 - b[1];
    out[2] = a[2] + t54m8 - b[2];
    out[3] = a[3] + t54m8 - b[3];
    out[4] = a[4] + t54m8 - b[4];
}
static void fscalar(felem out, const felem in) { /* * 121665 */
    u128 a;
    a = (u128)in[0] * 121665; out[0] = (uint64_t)a & FE_MASK;
    a = (u128)in[1] * 121665 + (uint64_t)(a >> 51); out[1] = (uint64_t)a & FE_MASK;
    a = (u128)in[2] * 121665 + (uint64_t)(a >> 51); out[2] = (uint64_t)a & FE_MASK;
    a = (u128)in[3] * 121665 + (uint64_t)(a >> 51); out[3] = (uint64_t)a & FE_MASK;
    a = (u128)in[4] * 121665 + (uint64_t)(a >> 51); out[4] = (uint64_t)a & FE_MASK;
    out[0] += 19 * (uint64_t)(a >> 51);
}
static void fmul(felem out, const felem in2, const felem in) {
    u128 t[5];
    uint64_t r0 = in[0], r1 = in[1], r2 = in[2], r3 = in[3], r4 = in[4];
    uint64_t s0 = in2[0], s1 = in2[1], s2 = in2[2], s3 = in2[3], s4 = in2[4], c;
    t[0] = (u128)r0 * s0;
    t[1] = (u128)r0 * s1 + (u128)r1 * s0;
    t[2] = (u128)r0 * s2 + (u128)r2 * s0 + (u128)r1 * s1;
    t[3] = (u128)r0 * s3 + (u128)r3 * s0 + (u128)r1 * s2 + (u128)r2 * s1;
    t[4] = (u128)r0 * s4 + (u128)r4 * s0 + (u128)r3 * s1 + (u128)r1 * s3 +
           (u128)r2 * s2;
    r4 *= 19; r1 *= 19; r2 *= 19; r3 *= 19;
    t[0] += (u128)r4 * s1 + (u128)r1 * s4 + (u128)r2 * s3 + (u128)r3 * s2;
    t[1] += (u128)r4 * s2 + (u128)r2 * s4 + (u128)r3 * s3;
    t[2] += (u128)r4 * s3 + (u128)r3 * s4;
    t[3] += (u128)r4 * s4;
    c = (uint64_t)(t[0] >> 51); r0 = (uint64_t)t[0] & FE_MASK;
    t[1] += c; c = (uint64_t)(t[1] >> 51); r1 = (uint64_t)t[1] & FE_MASK;
    t[2] += c; c = (uint64_t)(t[2] >> 51); r2 = (uint64_t)t[2] & FE_MASK;
    t[3] += c; c = (uint64_t)(t[3] >> 51); r3 = (uint64_t)t[3] & FE_MASK;
    t[4] += c; c = (uint64_t)(t[4] >> 51); r4 = (uint64_t)t[4] & FE_MASK;
    r0 += c * 19; c = r0 >> 51; r0 &= FE_MASK;
    r1 += c; c = r1 >> 51; r1 &= FE_MASK; r2 += c;
    out[0] = r0; out[1] = r1; out[2] = r2; out[3] = r3; out[4] = r4;
}
static void fsquare(felem out, const felem in) { fmul(out, in, in); }

static void fmontswap(felem a, felem b, uint64_t iswap) {
    uint64_t m = (uint64_t)(-(int64_t)iswap);
    for (int i = 0; i < 5; i++) {
        uint64_t x = m & (a[i] ^ b[i]);
        a[i] ^= x; b[i] ^= x;
    }
}

/* out = x^(2^255-21) = x^(p-2), the field inverse (donna's addition chain). */
static void crecip(felem out, const felem z) {
    felem a, t0, b, c;
    int i;
    fsquare(a, z);                            /* 2 */
    fsquare(t0, a); fsquare(t0, t0); fmul(b, t0, z);   /* 9 */
    fmul(a, b, a);                            /* 11 */
    fsquare(t0, a); fmul(b, t0, b);           /* 2^5 - 2^0 */
    fsquare(t0, b); for (i = 1; i < 5; i++) fsquare(t0, t0); fmul(b, t0, b);
    fsquare(t0, b); for (i = 1; i < 10; i++) fsquare(t0, t0); fmul(c, t0, b);
    fsquare(t0, c); for (i = 1; i < 20; i++) fsquare(t0, t0); fmul(t0, t0, c);
    fsquare(t0, t0); for (i = 1; i < 10; i++) fsquare(t0, t0); fmul(b, t0, b);
    fsquare(t0, b); for (i = 1; i < 50; i++) fsquare(t0, t0); fmul(c, t0, b);
    fsquare(t0, c); for (i = 1; i < 100; i++) fsquare(t0, t0); fmul(t0, t0, c);
    fsquare(t0, t0); for (i = 1; i < 50; i++) fsquare(t0, t0); fmul(t0, t0, b);
    /* z^(2^250-1) -> 5 squarings -> z^(2^255-32), * z^11 -> z^(2^255-21) = z^(p-2) */
    for (i = 0; i < 5; i++) { fsquare(t0, t0); }
    fmul(out, t0, a);
}

static void cmult(felem outx, felem outz, const uint8_t *scalar,
                  const felem point) {
    felem x1, x2, z2, x3, z3;
    felem a, aa, b, bb, e, c, d, da, cb, t0, t1;
    for (int i = 0; i < 5; i++) { x1[i] = point[i]; x3[i] = point[i]; }
    for (int i = 0; i < 5; i++) { x2[i] = 0; z2[i] = 0; z3[i] = 0; }
    x2[0] = 1; z3[0] = 1;
    uint64_t swap = 0;
    for (int t = 254; t >= 0; t--) {
        uint64_t kt = (scalar[t >> 3] >> (t & 7)) & 1;
        swap ^= kt;
        fmontswap(x2, x3, swap);
        fmontswap(z2, z3, swap);
        swap = kt;
        fsum(a, x2, z2); fdiff(b, x2, z2);
        fsum(c, x3, z3); fdiff(d, x3, z3);
        fmul(da, d, a); fmul(cb, c, b);
        fsum(t0, da, cb); fdiff(t1, da, cb);
        fsquare(x3, t0); fsquare(t1, t1); fmul(z3, x1, t1);
        fsquare(aa, a); fsquare(bb, b);
        fmul(x2, aa, bb); fdiff(e, aa, bb);
        fscalar(t0, e); fsum(t0, aa, t0); fmul(z2, e, t0);
    }
    fmontswap(x2, x3, swap);
    fmontswap(z2, z3, swap);
    for (int i = 0; i < 5; i++) { outx[i] = x2[i]; outz[i] = z2[i]; }
}

/* RFC 7748 X25519(scalar, u-coordinate) -> shared u-coordinate. */
void wo_x25519(uint8_t out[32], const uint8_t scalar[32],
               const uint8_t point[32]) {
    uint8_t e[32];
    for (int i = 0; i < 32; i++) e[i] = scalar[i];
    e[0] &= 248; e[31] &= 127; e[31] |= 64; /* clamp */
    felem bp, x, z, zi;
    fexpand(bp, point);
    cmult(x, z, e, bp);
    crecip(zi, z);
    fmul(x, x, zi);
    fcontract(out, x);
}

/* ---- RSA signature verification (rv2 9 phase D, part 1) -----------------
 * PKCS#1 v1.5 and PSS over SHA-256, for the server certificate chain and the
 * TLS 1.3 CertificateVerify. Verification touches ONLY public data (public key
 * + signature), so it needs no constant-time discipline — a plain bignum
 * modexp with the small public exponent. Montgomery multiplication (CIOS,
 * 64-bit limbs, 128-bit products). Internal C; consumer is the TLS handshake.
 * Vectors: RSA-2048 PKCS1v15 + PSS in test_crypto.c. */
#define RSA_MAXW 66 /* up to ~4224-bit modulus */

static int bn_from_be(uint64_t *w, const uint8_t *b, size_t len) {
    int k = (int)((len + 7) / 8);
    if (k > RSA_MAXW || k == 0) return -1;
    for (int i = 0; i < k; i++) w[i] = 0;
    for (size_t i = 0; i < len; i++)
        w[i / 8] |= (uint64_t)b[len - 1 - i] << (8 * (i % 8));
    return k;
}
static void bn_to_be(uint8_t *b, size_t len, const uint64_t *w, int k) {
    for (size_t i = 0; i < len; i++)
        b[len - 1 - i] = (i / 8 < (size_t)k) ? (uint8_t)(w[i / 8] >> (8 * (i % 8))) : 0;
}
static int bn_ge(const uint64_t *a, const uint64_t *b, int k) {
    for (int i = k - 1; i >= 0; i--) { if (a[i] > b[i]) return 1; if (a[i] < b[i]) return 0; }
    return 1;
}
static void bn_sub(uint64_t *out, const uint64_t *a, const uint64_t *b, int k) {
    u128 borrow = 0;
    for (int i = 0; i < k; i++) {
        u128 d = (u128)a[i] - b[i] - borrow;
        out[i] = (uint64_t)d;
        borrow = (uint64_t)(d >> 64) & 1;
    }
}
static uint64_t bn_shl1(uint64_t *a, int k) {
    uint64_t carry = 0;
    for (int i = 0; i < k; i++) { uint64_t nc = a[i] >> 63; a[i] = (a[i] << 1) | carry; carry = nc; }
    return carry;
}
static uint64_t inv64(uint64_t a) { /* a odd: a^-1 mod 2^64, Newton */
    uint64_t x = a;
    for (int i = 0; i < 5; i++) x *= 2 - a * x;
    return x;
}
static void mont_mul(uint64_t *out, const uint64_t *a, const uint64_t *b,
                     const uint64_t *m, uint64_t n0, int k) {
    uint64_t t[RSA_MAXW + 2];
    for (int i = 0; i < k + 2; i++) t[i] = 0;
    for (int i = 0; i < k; i++) {
        u128 carry = 0;
        for (int j = 0; j < k; j++) {
            u128 s = (u128)a[i] * b[j] + t[j] + carry;
            t[j] = (uint64_t)s; carry = s >> 64;
        }
        u128 s = (u128)t[k] + carry; t[k] = (uint64_t)s; t[k + 1] += (uint64_t)(s >> 64);
        uint64_t mp = (uint64_t)((u128)t[0] * n0);
        carry = ((u128)mp * m[0] + t[0]) >> 64;
        for (int j = 1; j < k; j++) {
            u128 s2 = (u128)mp * m[j] + t[j] + carry;
            t[j - 1] = (uint64_t)s2; carry = s2 >> 64;
        }
        u128 s3 = (u128)t[k] + carry; t[k - 1] = (uint64_t)s3; carry = s3 >> 64;
        t[k] = t[k + 1] + (uint64_t)carry; t[k + 1] = 0;
    }
    if (t[k] || bn_ge(t, m, k)) bn_sub(t, t, m, k);
    for (int i = 0; i < k; i++) out[i] = t[i];
}
/* out = base^e mod m (e big-endian bytes, public exponent). */
static void bn_modexp(uint64_t *out, const uint64_t *base, const uint64_t *m,
                      int k, const uint8_t *e, size_t elen) {
    uint64_t n0 = 0 - inv64(m[0]);
    uint64_t rsq[RSA_MAXW], aR[RSA_MAXW], x[RSA_MAXW], one[RSA_MAXW], tmp[RSA_MAXW];
    for (int i = 0; i < k; i++) { rsq[i] = 0; one[i] = 0; }
    rsq[0] = 1; one[0] = 1;
    for (int i = 0; i < 128 * k; i++) { /* rsq = 2^(128k) mod m */
        uint64_t of = bn_shl1(rsq, k);
        if (of || bn_ge(rsq, m, k)) bn_sub(rsq, rsq, m, k);
    }
    mont_mul(aR, base, rsq, m, n0, k); /* base -> Montgomery */
    mont_mul(x, one, rsq, m, n0, k);   /* x = R mod m (== 1 in Montgomery) */
    for (size_t bi = 0; bi < elen * 8; bi++) {
        uint8_t bit = (e[bi / 8] >> (7 - (bi % 8))) & 1;
        mont_mul(tmp, x, x, m, n0, k);
        for (int i = 0; i < k; i++) x[i] = tmp[i];
        if (bit) { mont_mul(tmp, x, aR, m, n0, k); for (int i = 0; i < k; i++) x[i] = tmp[i]; }
    }
    mont_mul(out, x, one, m, n0, k); /* out of Montgomery */
}

static void mgf1_sha256(const uint8_t *seed, size_t seedlen, uint8_t *mask,
                        size_t masklen) {
    size_t done = 0;
    uint32_t counter = 0;
    uint8_t in[96];
    while (done < masklen) {
        memcpy(in, seed, seedlen);
        in[seedlen] = (uint8_t)(counter >> 24); in[seedlen + 1] = (uint8_t)(counter >> 16);
        in[seedlen + 2] = (uint8_t)(counter >> 8); in[seedlen + 3] = (uint8_t)counter;
        uint8_t d[32];
        wo_sha256(in, seedlen + 4, d);
        size_t n = masklen - done < 32 ? masklen - done : 32;
        memcpy(mask + done, d, n);
        done += n; counter++;
    }
}

/* Common front: s^e mod n into em[nlen]. 0 ok, -1 malformed. */
static int rsa_recover(const uint8_t *n, size_t nlen, const uint8_t *e,
                       size_t elen, const uint8_t *sig, size_t siglen,
                       uint8_t *em) {
    if (siglen != nlen) return -1;
    uint64_t N[RSA_MAXW], S[RSA_MAXW], EM[RSA_MAXW];
    int k = bn_from_be(N, n, nlen);
    int ks = bn_from_be(S, sig, siglen);
    if (k < 0 || ks < 0 || (N[0] & 1) == 0) return -1;
    for (int i = ks; i < k; i++) S[i] = 0;
    if (bn_ge(S, N, k)) return -1;
    bn_modexp(EM, S, N, k, e, elen);
    bn_to_be(em, nlen, EM, k);
    return 0;
}

/* ---- RSA private-key ops (rv2 9 phase G1: signing) -----------------------
 * Signing touches the SECRET exponent, so the modexp must be constant-time.
 * bn_modexp above branches on the exponent bit (fine for the public e); this
 * one squares AND multiplies every bit and selects the result with a mask, so
 * the operation sequence is independent of d. */

/* Constant-time conditional move: dst = mask ? src : dst (mask all-ones/zero). */
static void bn_cmov(uint64_t *dst, const uint64_t *src, uint64_t mask, int k) {
    for (int i = 0; i < k; i++) dst[i] = (dst[i] & ~mask) | (src[i] & mask);
}
/* out = base^e mod m, constant-time in e (the secret exponent). */
static void bn_modexp_ct(uint64_t *out, const uint64_t *base, const uint64_t *m,
                         int k, const uint8_t *e, size_t elen) {
    uint64_t n0 = 0 - inv64(m[0]);
    uint64_t rsq[RSA_MAXW], aR[RSA_MAXW], x[RSA_MAXW], one[RSA_MAXW];
    uint64_t sq[RSA_MAXW], prod[RSA_MAXW];
    for (int i = 0; i < k; i++) { rsq[i] = 0; one[i] = 0; }
    rsq[0] = 1; one[0] = 1;
    for (int i = 0; i < 128 * k; i++) {
        uint64_t of = bn_shl1(rsq, k);
        if (of || bn_ge(rsq, m, k)) bn_sub(rsq, rsq, m, k);
    }
    mont_mul(aR, base, rsq, m, n0, k);
    mont_mul(x, one, rsq, m, n0, k);           /* x = R (Montgomery 1) */
    for (size_t bi = 0; bi < elen * 8; bi++) {
        uint8_t bit = (e[bi / 8] >> (7 - (bi % 8))) & 1;
        mont_mul(sq, x, x, m, n0, k);          /* x = x^2 */
        for (int i = 0; i < k; i++) x[i] = sq[i];
        mont_mul(prod, x, aR, m, n0, k);       /* always compute x*base ... */
        bn_cmov(x, prod, (uint64_t)0 - (uint64_t)bit, k); /* ... select on bit */
    }
    mont_mul(out, x, one, m, n0, k);
}

/* RSA-PSS sign over SHA-256 (RFC 8017 §9.1.1 / §8.1.1). The 32-byte message
 * hash and the salt are inputs — the caller supplies fresh salt (a fixed salt
 * makes the KAT deterministic). Private key is (n, d), both big-endian. Writes
 * nlen signature bytes. Requires a top-bit-set (full-length) modulus. 0 ok,
 * -1 on a bad size. */
int wo_rsa_pss_sha256_sign(const uint8_t *n, size_t nlen, const uint8_t *d,
                           size_t dlen, const uint8_t mhash[32],
                           const uint8_t *salt, size_t saltlen, uint8_t *out) {
    const size_t hLen = 32, emLen = nlen;
    if (nlen == 0 || (n[0] & 0x80) == 0) return -1;      /* need modBits = 8*nlen */
    if (saltlen + hLen + 2 > emLen) return -1;

    /* H = SHA256(0x00*8 || mHash || salt) */
    uint8_t mp[8 + 32 + 64];
    if (saltlen > 64) return -1;
    memset(mp, 0, 8);
    memcpy(mp + 8, mhash, 32);
    memcpy(mp + 40, salt, saltlen);
    uint8_t H[32];
    wo_sha256(mp, 8 + 32 + saltlen, H);

    /* EM = maskedDB || H || 0xbc, DB = PS || 0x01 || salt */
    uint8_t em[RSA_MAXW * 8];
    size_t dblen = emLen - hLen - 1;
    memset(em, 0, dblen);
    em[dblen - saltlen - 1] = 0x01;
    memcpy(em + dblen - saltlen, salt, saltlen);
    uint8_t dbmask[RSA_MAXW * 8];
    mgf1_sha256(H, hLen, dbmask, dblen);
    for (size_t i = 0; i < dblen; i++) em[i] ^= dbmask[i];
    em[0] &= 0x7f;                                        /* clear the top bit */
    memcpy(em + dblen, H, hLen);
    em[emLen - 1] = 0xbc;

    /* signature = EM^d mod n */
    uint64_t N[RSA_MAXW], M[RSA_MAXW], SIG[RSA_MAXW];
    int k = bn_from_be(N, n, nlen);
    if (k < 0 || (N[0] & 1) == 0) return -1;
    if (bn_from_be(M, em, emLen) < 0) return -1;
    if (bn_ge(M, N, k)) return -1;
    bn_modexp_ct(SIG, M, N, k, d, dlen);
    bn_to_be(out, nlen, SIG, k);
    return 0;
}

int wo_rsa_pkcs1_sha256_verify(const uint8_t *n, size_t nlen, const uint8_t *e,
                               size_t elen, const uint8_t *sig, size_t siglen,
                               const uint8_t hash[32]) {
    static const uint8_t di[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60,
                                  0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02,
                                  0x01, 0x05, 0x00, 0x04, 0x20 };
    uint8_t em[RSA_MAXW * 8];
    if (nlen > sizeof(em)) return 0;
    if (rsa_recover(n, nlen, e, elen, sig, siglen, em) != 0) return 0;
    size_t tlen = sizeof(di) + 32;
    if (nlen < tlen + 11) return 0;
    size_t pslen = nlen - tlen - 3;
    if (em[0] != 0x00 || em[1] != 0x01) return 0;
    for (size_t i = 0; i < pslen; i++) if (em[2 + i] != 0xff) return 0;
    if (em[2 + pslen] != 0x00) return 0;
    if (memcmp(em + 3 + pslen, di, sizeof(di)) != 0) return 0;
    if (memcmp(em + 3 + pslen + sizeof(di), hash, 32) != 0) return 0;
    return 1;
}

/* EMSA-PSS verify, SHA-256, assuming a full-length modulus (emBits =
 * 8*nlen-1 — true for standard RSA-2048/3072/4096 keys). */
int wo_rsa_pss_sha256_verify(const uint8_t *n, size_t nlen, const uint8_t *e,
                             size_t elen, const uint8_t *sig, size_t siglen,
                             const uint8_t mhash[32], size_t saltlen) {
    uint8_t em[RSA_MAXW * 8];
    if (nlen > sizeof(em) || saltlen > 64) return 0;
    if (rsa_recover(n, nlen, e, elen, sig, siglen, em) != 0) return 0;
    size_t hLen = 32, emLen = nlen;
    if (emLen < hLen + saltlen + 2) return 0;
    if (em[emLen - 1] != 0xbc) return 0;
    if (em[0] & 0x80) return 0; /* the one top bit (8*emLen-1 emBits) must be 0 */
    size_t dbLen = emLen - hLen - 1;
    const uint8_t *H = em + dbLen;
    uint8_t db[RSA_MAXW * 8];
    mgf1_sha256(H, hLen, db, dbLen);
    for (size_t i = 0; i < dbLen; i++) db[i] ^= em[i];
    db[0] &= 0x7f;
    size_t i = 0;
    while (i < dbLen - saltlen - 1 && db[i] == 0) i++;
    if (i != dbLen - saltlen - 1 || db[i] != 0x01) return 0;
    const uint8_t *salt = db + dbLen - saltlen;
    uint8_t mp[8 + 32 + 64], hp[32];
    memset(mp, 0, 8);
    memcpy(mp + 8, mhash, 32);
    memcpy(mp + 40, salt, saltlen);
    wo_sha256(mp, 8 + 32 + saltlen, hp);
    return memcmp(hp, H, 32) == 0 ? 1 : 0;
}

/* ---- ECDSA-P256 verification (rv2 9 phase D, part 2) --------------------
 * NIST P-256 (secp256r1). Verify-only (public data), so not constant-time;
 * reuses the bignum Montgomery multiply and modexp (Fermat inverses). Jacobian
 * point arithmetic with a=-3. Internal C; consumer is the TLS handshake and the
 * X.509 chain. Vectors: python ECDSA-P256 in test_crypto.c. */

/* All big-endian, 32 bytes. */
static const uint8_t P256_P[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff };
static const uint8_t P256_PM2[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfd };
static const uint8_t P256_N[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51 };
static const uint8_t P256_NM2[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x4f };
static const uint8_t P256_GX[32] = {
    0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,
    0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96 };
static const uint8_t P256_GY[32] = {
    0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,
    0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5 };
static const uint8_t P256_B[32] = {
    0x5a,0xc6,0x35,0xd8,0xaa,0x3a,0x93,0xe7,0xb3,0xeb,0xbd,0x55,0x76,0x98,0x86,0xbc,
    0x65,0x1d,0x06,0xb0,0xcc,0x53,0xb0,0xf6,0x3b,0xce,0x3c,0x3e,0x27,0xd2,0x60,0x4b };

typedef uint64_t fp[4];

static void compute_rsq(uint64_t *rsq, const uint64_t *m, int k) {
    for (int i = 0; i < k; i++) rsq[i] = 0;
    rsq[0] = 1;
    for (int i = 0; i < 128 * k; i++) {
        uint64_t of = bn_shl1(rsq, k);
        if (of || bn_ge(rsq, m, k)) bn_sub(rsq, rsq, m, k);
    }
}
static void modadd(uint64_t *o, const uint64_t *a, const uint64_t *b,
                   const uint64_t *m, int k) {
    u128 c = 0;
    for (int i = 0; i < k; i++) { u128 s = (u128)a[i] + b[i] + c; o[i] = (uint64_t)s; c = s >> 64; }
    if (c || bn_ge(o, m, k)) bn_sub(o, o, m, k);
}
static void modsub(uint64_t *o, const uint64_t *a, const uint64_t *b,
                   const uint64_t *m, int k) {
    if (bn_ge(a, b, k)) {
        bn_sub(o, a, b, k);
    } else { /* a < b: compute (a + m) - b, which fits in k limbs since result < m */
        uint64_t t[4];
        u128 c = 0;
        for (int i = 0; i < k; i++) { u128 s = (u128)a[i] + m[i] + c; t[i] = (uint64_t)s; c = s >> 64; }
        bn_sub(o, t, b, k);
    }
}

/* Montgomery-domain field context for a modulus (p or n). */
typedef struct { fp m; fp rsq; fp one_mont; uint64_t n0; } modctx;
static void modctx_init(modctx *c, const uint8_t *mbe) {
    bn_from_be(c->m, mbe, 32);
    compute_rsq(c->rsq, c->m, 4);
    c->n0 = 0 - inv64(c->m[0]);
    fp one = { 1, 0, 0, 0 };
    mont_mul(c->one_mont, one, c->rsq, c->m, c->n0, 4);
}
static void to_mont(const modctx *c, fp o, const fp a) { mont_mul(o, a, c->rsq, c->m, c->n0, 4); }
static void from_mont(const modctx *c, fp o, const fp a) { fp one = {1,0,0,0}; mont_mul(o, a, one, c->m, c->n0, 4); }
static void fpmul(const modctx *c, fp o, const fp a, const fp b) { mont_mul(o, a, b, c->m, c->n0, 4); }
static int fp_eq(const fp a, const fp b) { for (int i=0;i<4;i++) if (a[i]!=b[i]) return 0; return 1; }
static int fp_zero(const fp a) { return (a[0]|a[1]|a[2]|a[3]) == 0; }

/* Jacobian point in Montgomery domain mod p. Z==0 is the point at infinity. */
typedef struct { fp X, Y, Z; } jpt;

static void jdouble(const modctx *P, jpt *o, const jpt *p) {
    if (fp_zero(p->Z)) { *o = *p; return; }
    fp delta, gamma, beta, alpha, t, u, x3, y3, z3, tmp;
    fpmul(P, delta, p->Z, p->Z);
    fpmul(P, gamma, p->Y, p->Y);
    fpmul(P, beta, p->X, gamma);
    modsub(t, p->X, delta, P->m, 4);
    modadd(u, p->X, delta, P->m, 4);
    fpmul(P, tmp, t, u);
    modadd(alpha, tmp, tmp, P->m, 4); modadd(alpha, alpha, tmp, P->m, 4); /* 3*(X-d)(X+d) */
    fpmul(P, x3, alpha, alpha);
    fp b8; modadd(b8, beta, beta, P->m, 4); modadd(b8, b8, b8, P->m, 4); modadd(b8, b8, b8, P->m, 4); /* 8*beta */
    modsub(x3, x3, b8, P->m, 4);
    modadd(z3, p->Y, p->Z, P->m, 4); fpmul(P, z3, z3, z3);
    modsub(z3, z3, gamma, P->m, 4); modsub(z3, z3, delta, P->m, 4);
    fp b4; modadd(b4, beta, beta, P->m, 4); modadd(b4, b4, b4, P->m, 4); /* 4*beta */
    modsub(b4, b4, x3, P->m, 4);
    fpmul(P, y3, alpha, b4);
    fp g2; fpmul(P, g2, gamma, gamma); /* gamma^2 */
    modadd(g2, g2, g2, P->m, 4); modadd(g2, g2, g2, P->m, 4); modadd(g2, g2, g2, P->m, 4); /* 8*gamma^2 */
    modsub(y3, y3, g2, P->m, 4);
    for (int i=0;i<4;i++){ o->X[i]=x3[i]; o->Y[i]=y3[i]; o->Z[i]=z3[i]; }
}

static void jadd(const modctx *P, jpt *o, const jpt *a, const jpt *b) {
    if (fp_zero(a->Z)) { *o = *b; return; }
    if (fp_zero(b->Z)) { *o = *a; return; }
    fp z1z1, z2z2, u1, u2, s1, s2, h, r, tmp;
    fpmul(P, z1z1, a->Z, a->Z);
    fpmul(P, z2z2, b->Z, b->Z);
    fpmul(P, u1, a->X, z2z2);
    fpmul(P, u2, b->X, z1z1);
    fpmul(P, tmp, b->Z, z2z2); fpmul(P, s1, a->Y, tmp);
    fpmul(P, tmp, a->Z, z1z1); fpmul(P, s2, b->Y, tmp);
    modsub(h, u2, u1, P->m, 4);
    modsub(r, s2, s1, P->m, 4);
    if (fp_zero(h)) {
        if (fp_zero(r)) { jdouble(P, o, a); return; }
        for (int i=0;i<4;i++){ o->X[i]=0; o->Y[i]=0; o->Z[i]=0; } /* infinity */
        return;
    }
    fp h2, h3, x3, y3, z3, u1h2;
    fpmul(P, h2, h, h);
    fpmul(P, h3, h2, h);
    fpmul(P, u1h2, u1, h2);
    fpmul(P, x3, r, r);
    modsub(x3, x3, h3, P->m, 4);
    modsub(x3, x3, u1h2, P->m, 4); modsub(x3, x3, u1h2, P->m, 4); /* - 2*u1h2 */
    modsub(tmp, u1h2, x3, P->m, 4);
    fpmul(P, y3, r, tmp);
    fpmul(P, tmp, s1, h3);
    modsub(y3, y3, tmp, P->m, 4);
    fpmul(P, z3, a->Z, b->Z); fpmul(P, z3, z3, h);
    for (int i=0;i<4;i++){ o->X[i]=x3[i]; o->Y[i]=y3[i]; o->Z[i]=z3[i]; }
}

/* R = k*Pt (double-and-add, MSB first; k big-endian 32 bytes). Not CT. */
static void jmul(const modctx *P, jpt *o, const uint8_t k[32], const jpt *pt) {
    jpt acc; for (int i=0;i<4;i++){ acc.X[i]=0; acc.Y[i]=0; acc.Z[i]=0; }
    for (int bit = 255; bit >= 0; bit--) {
        jdouble(P, &acc, &acc);
        if ((k[(255 - bit) / 8] >> (7 - ((255 - bit) & 7))) & 1) jadd(P, &acc, &acc, pt);
    }
    *o = acc;
}

int wo_ecdsa_p256_sha256_verify(const uint8_t qx[32], const uint8_t qy[32],
                                const uint8_t r[32], const uint8_t s[32],
                                const uint8_t hash[32]) {
    modctx P, N;
    modctx_init(&P, P256_P);
    modctx_init(&N, P256_N);
    fp rv, sv;
    bn_from_be(rv, r, 32); bn_from_be(sv, s, 32);
    if (fp_zero(rv) || fp_zero(sv) || bn_ge(rv, N.m, 4) || bn_ge(sv, N.m, 4))
        return 0; /* r,s in [1, n-1] */

    /* Q on curve: y^2 == x^3 - 3x + b (mod p), in Montgomery domain */
    fp qxm, qym, bm, lhs, rhs, x3, three_x;
    fp qxr, qyr, br;
    bn_from_be(qxr, qx, 32); bn_from_be(qyr, qy, 32);
    if (bn_ge(qxr, P.m, 4) || bn_ge(qyr, P.m, 4)) return 0;
    bn_from_be(br, P256_B, 32);
    to_mont(&P, qxm, qxr); to_mont(&P, qym, qyr); to_mont(&P, bm, br);
    fpmul(&P, lhs, qym, qym);
    fpmul(&P, x3, qxm, qxm); fpmul(&P, x3, x3, qxm);
    modadd(three_x, qxm, qxm, P.m, 4); modadd(three_x, three_x, qxm, P.m, 4);
    modsub(rhs, x3, three_x, P.m, 4); modadd(rhs, rhs, bm, P.m, 4);
    if (!fp_eq(lhs, rhs)) return 0;

    /* z = hash mod n */
    fp z; bn_from_be(z, hash, 32);
    if (bn_ge(z, N.m, 4)) bn_sub(z, z, N.m, 4);

    /* w = s^-1 mod n; u1 = z*w mod n; u2 = r*w mod n */
    fp w, u1, u2, tm;
    bn_modexp(w, sv, N.m, 4, P256_NM2, 32);
    to_mont(&N, tm, z); fpmul(&N, u1, tm, w);   /* (z*Rn)*w*Rn^-1 = z*w mod n */
    to_mont(&N, tm, rv); fpmul(&N, u2, tm, w);

    /* R = u1*G + u2*Q  (Jacobian, mont domain mod p) */
    jpt G, Q, A, B, Rp;
    fp gx, gy;
    bn_from_be(gx, P256_GX, 32); bn_from_be(gy, P256_GY, 32);
    to_mont(&P, G.X, gx); to_mont(&P, G.Y, gy); for (int i=0;i<4;i++) G.Z[i]=P.one_mont[i];
    for (int i=0;i<4;i++){ Q.X[i]=qxm[i]; Q.Y[i]=qym[i]; Q.Z[i]=P.one_mont[i]; }
    uint8_t u1b[32], u2b[32];
    bn_to_be(u1b, 32, u1, 4); bn_to_be(u2b, 32, u2, 4);
    jmul(&P, &A, u1b, &G);
    jmul(&P, &B, u2b, &Q);
    jadd(&P, &Rp, &A, &B);
    if (fp_zero(Rp.Z)) return 0;

    /* affine x = X / Z^2 (mod p), then compare (x mod n) to r */
    fp Xn, Zn, zinv, zinv2, xaff, xr;
    from_mont(&P, Xn, Rp.X); from_mont(&P, Zn, Rp.Z);
    bn_modexp(zinv, Zn, P.m, 4, P256_PM2, 32);
    to_mont(&P, tm, zinv); fpmul(&P, zinv2, tm, zinv);   /* zinv^2 (normal domain) */
    to_mont(&P, tm, Xn); fpmul(&P, xaff, tm, zinv2);      /* X * zinv^2 (normal) */
    for (int i=0;i<4;i++) xr[i]=xaff[i];
    if (bn_ge(xr, N.m, 4)) bn_sub(xr, xr, N.m, 4);
    return fp_eq(xr, rv) ? 1 : 0;
}

/* ---- ECDSA-P256 signing (rv2 9 phase G1b) --------------------------------
 * Deterministic nonce (RFC 6979) — no RNG, no nonce-reuse/bias risk, and
 * KAT-able against the published vectors. The scalar multiply k*G and the
 * inversions touch the secret k/d, so they use the constant-time modexp and a
 * double-and-add-always ladder. Known residual: the ladder's point-at-infinity
 * handling leaks k's leading-zero count (a bit-length hint, not the key); a
 * complete-formula / Montgomery-ladder upgrade is a named follow-up. */

static void jpt_cmov(jpt *d, const jpt *s, uint64_t mask) {
    bn_cmov(d->X, s->X, mask, 4);
    bn_cmov(d->Y, s->Y, mask, 4);
    bn_cmov(d->Z, s->Z, mask, 4);
}
/* R = k*pt, constant-time in k (double-and-add-always). */
static void jmul_ct(const modctx *P, jpt *o, const uint8_t k[32], const jpt *pt) {
    jpt acc; for (int i = 0; i < 4; i++) { acc.X[i] = 0; acc.Y[i] = 0; acc.Z[i] = 0; }
    for (int bit = 255; bit >= 0; bit--) {
        jdouble(P, &acc, &acc);
        jpt t; jadd(P, &t, &acc, pt);
        uint64_t m = (uint64_t)0 - (uint64_t)((k[(255 - bit) / 8] >> (7 - ((255 - bit) & 7))) & 1);
        jpt_cmov(&acc, &t, m);
    }
    *o = acc;
}

/* HMAC-SHA256 with a 32-byte key (RFC 6979's DRBG uses fixed-size keys). */
static void hmac32(const uint8_t key[32], const uint8_t *msg, size_t mlen,
                   uint8_t out[32]) {
    wo_hmac_sha256(key, 32, msg, mlen, out);
}

/* ECDSA-P256 sign over SHA-256 with an RFC 6979 deterministic nonce. Private
 * scalar d and 32-byte message hash in; (r,s) out, big-endian. 0 ok, -1 on
 * failure (astronomically unlikely nonce exhaustion, or d out of range). */
int wo_ecdsa_p256_sha256_sign(const uint8_t d[32], const uint8_t hash[32],
                              uint8_t r_out[32], uint8_t s_out[32]) {
    modctx P, N;
    modctx_init(&P, P256_P);
    modctx_init(&N, P256_N);
    fp dfp; bn_from_be(dfp, d, 32);
    if (fp_zero(dfp) || bn_ge(dfp, N.m, 4)) return -1;   /* d in [1, n-1] */

    /* z = hash mod n, and bits2octets(hash) = z as 32 bytes */
    fp z; bn_from_be(z, hash, 32);
    if (bn_ge(z, N.m, 4)) bn_sub(z, z, N.m, 4);
    uint8_t h1o[32]; bn_to_be(h1o, 32, z, 4);

    /* RFC 6979 §3.2 seeding */
    uint8_t V[32], K[32], buf[32 + 1 + 32 + 32];
    memset(V, 0x01, 32); memset(K, 0x00, 32);
    memcpy(buf, V, 32); buf[32] = 0x00; memcpy(buf + 33, d, 32); memcpy(buf + 65, h1o, 32);
    hmac32(K, buf, 97, K); hmac32(K, V, 32, V);
    memcpy(buf, V, 32); buf[32] = 0x01; memcpy(buf + 33, d, 32); memcpy(buf + 65, h1o, 32);
    hmac32(K, buf, 97, K); hmac32(K, V, 32, V);

    /* pre-mont G */
    jpt G; fp gx, gy;
    bn_from_be(gx, P256_GX, 32); bn_from_be(gy, P256_GY, 32);
    to_mont(&P, G.X, gx); to_mont(&P, G.Y, gy);
    for (int i = 0; i < 4; i++) G.Z[i] = P.one_mont[i];

    for (int tries = 0; tries < 64; tries++) {
        hmac32(K, V, 32, V);                              /* T = V (qlen = 256) */
        fp kfp; bn_from_be(kfp, V, 32);
        if (!fp_zero(kfp) && !bn_ge(kfp, N.m, 4)) {
            jpt R; jmul_ct(&P, &R, V, &G);
            if (!fp_zero(R.Z)) {
                /* affine x of R (Z^-2 * X, mod p, all constant-time) */
                fp Xn, Zn, zinv, zinv2, tm, xaff, rr;
                from_mont(&P, Xn, R.X); from_mont(&P, Zn, R.Z);
                bn_modexp_ct(zinv, Zn, P.m, 4, P256_PM2, 32);
                to_mont(&P, tm, zinv); fpmul(&P, zinv2, tm, zinv);
                to_mont(&P, tm, Xn); fpmul(&P, xaff, tm, zinv2);
                for (int i = 0; i < 4; i++) rr[i] = xaff[i];
                if (bn_ge(rr, N.m, 4)) bn_sub(rr, rr, N.m, 4);
                if (!fp_zero(rr)) {
                    /* s = k^-1 (z + r*d) mod n */
                    fp kinv, rd, zrd, ss;
                    bn_modexp_ct(kinv, kfp, N.m, 4, P256_NM2, 32);
                    to_mont(&N, tm, rr); fpmul(&N, rd, tm, dfp);      /* r*d */
                    modadd(zrd, z, rd, N.m, 4);                        /* z + r*d */
                    to_mont(&N, tm, kinv); fpmul(&N, ss, tm, zrd);     /* k^-1*(z+rd) */
                    if (!fp_zero(ss)) {
                        bn_to_be(r_out, 32, rr, 4);
                        bn_to_be(s_out, 32, ss, 4);
                        return 0;
                    }
                }
            }
        }
        /* reject: K = HMAC(K, V||0x00); V = HMAC(K, V) */
        memcpy(buf, V, 32); buf[32] = 0x00;
        hmac32(K, buf, 33, K); hmac32(K, V, 32, V);
    }
    return -1;
}

/* ---- X.509 / ASN.1 DER (rv2 9 phase E, core) ----------------------------
 * A defensive DER reader and the certificate-field extraction TLS needs:
 * tbsCertificate (raw, for signature verification), the signature algorithm,
 * the signature, the SubjectPublicKeyInfo (RSA n/e or EC point), validity, and
 * the dNSName SANs. Single-cert signature verification dispatches to the
 * phase-D verifiers. Every length and bound is checked; a malformed input is a
 * rejection, never an over-read. Internal C; the consumer is the TLS handshake.
 * Vectors: a python-generated cert in test_crypto.c. */

typedef struct { const uint8_t *p, *end; } der;

/* Read one TLV. On success advances d->p past the value and returns the tag,
 * with vp/vl the value span; returns -1 on any malformation. */
static int der_tlv(der *d, const uint8_t **vp, size_t *vl) {
    if (d->p >= d->end) return -1;
    uint8_t tag = *d->p++;
    if (d->p >= d->end) return -1;
    size_t len = *d->p++;
    if (len & 0x80) {
        int nb = len & 0x7f;
        if (nb == 0 || nb > 4 || d->p + nb > d->end) return -1;
        len = 0;
        for (int i = 0; i < nb; i++) len = (len << 8) | *d->p++;
    }
    if (len > (size_t)(d->end - d->p)) return -1;
    *vp = d->p; *vl = len; d->p += len;
    return tag;
}
/* Expect a specific tag; return its value span as a sub-reader. */
static int der_into(der *d, uint8_t want, der *out) {
    const uint8_t *vp; size_t vl;
    int tag = der_tlv(d, &vp, &vl);
    if (tag != want) return -1;
    out->p = vp; out->end = vp + vl;
    return 0;
}
/* Skip one TLV of any tag. */
static int der_skip(der *d) {
    const uint8_t *vp; size_t vl;
    return der_tlv(d, &vp, &vl) < 0 ? -1 : 0;
}

/* Parsed certificate. Spans point into the caller's DER buffer (no copy). */
typedef struct {
    const uint8_t *tbs; size_t tbs_len;       /* raw tbsCertificate (TLV) */
    int sig_alg;                              /* WO_X509_SIG_* */
    const uint8_t *sig; size_t sig_len;       /* signature bytes */
    int key_alg;                              /* WO_X509_KEY_RSA / _EC_P256 */
    const uint8_t *rsa_n; size_t rsa_n_len;
    const uint8_t *rsa_e; size_t rsa_e_len;
    const uint8_t *ec_x, *ec_y;               /* 32 bytes each when EC P-256 */
    /* validity as YYYYMMDDHHMMSSZ-comparable 14-byte strings */
    char not_before[15], not_after[15];
    /* the bytes of tbsCertificate after subjectPublicKeyInfo — optional
     * uniqueIDs then the [3] extensions; walked lazily by the SAN check. */
    const uint8_t *ext_area; size_t ext_area_len;
} x509_cert;

enum { WO_X509_SIG_RSA_PKCS1_SHA256 = 1, WO_X509_SIG_RSA_PSS_SHA256, WO_X509_SIG_ECDSA_P256_SHA256, WO_X509_SIG_UNKNOWN = 0 };
enum { WO_X509_KEY_RSA = 1, WO_X509_KEY_EC_P256, WO_X509_KEY_UNKNOWN = 0 };

static int oid_eq(const uint8_t *a, size_t al, const uint8_t *b, size_t bl) {
    return al == bl && memcmp(a, b, al) == 0;
}
/* DER OID bodies (without the tag/len). */
static const uint8_t OID_RSA_ENC[] = { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01 };
static const uint8_t OID_SHA256_RSA[] = { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b };
static const uint8_t OID_RSASSA_PSS[] = { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0a };
static const uint8_t OID_EC_PUBKEY[] = { 0x2a,0x86,0x48,0xce,0x3d,0x02,0x01 };
static const uint8_t OID_P256[] = { 0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07 };
static const uint8_t OID_ECDSA_SHA256[] = { 0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x02 };

static int alg_from_oid(const uint8_t *o, size_t l) {
    if (oid_eq(o, l, OID_SHA256_RSA, sizeof OID_SHA256_RSA)) return WO_X509_SIG_RSA_PKCS1_SHA256;
    if (oid_eq(o, l, OID_RSASSA_PSS, sizeof OID_RSASSA_PSS)) return WO_X509_SIG_RSA_PSS_SHA256;
    if (oid_eq(o, l, OID_ECDSA_SHA256, sizeof OID_ECDSA_SHA256)) return WO_X509_SIG_ECDSA_P256_SHA256;
    return WO_X509_SIG_UNKNOWN;
}

/* Parse an AlgorithmIdentifier SEQ { OID, params }, return the mapped sig alg. */
static int parse_sigalg(der *d) {
    der ai, oid;
    const uint8_t *op; size_t ol;
    if (der_into(d, 0x30, &ai) < 0) return WO_X509_SIG_UNKNOWN;
    (void)oid;
    if (der_tlv(&ai, &op, &ol) != 0x06) return WO_X509_SIG_UNKNOWN;
    return alg_from_oid(op, ol);
}

/* Copy up to 14 chars of a UTCTime/GeneralizedTime into a YYYYMMDDHHMMSS
 * buffer (UTCTime YY -> 20YY/19YY heuristic). */
static void norm_time(const uint8_t *v, size_t l, int utc, char out[15]) {
    char buf[16]; size_t n = 0;
    if (utc) { /* YYMMDDHHMMSSZ */
        int yy = (v[0]-'0')*10 + (v[1]-'0');
        const char *cent = yy >= 50 ? "19" : "20";
        buf[n++]=cent[0]; buf[n++]=cent[1];
        for (size_t i=0;i<12 && i<l;i++) buf[n++]=(char)v[i];
    } else { /* YYYYMMDDHHMMSSZ */
        for (size_t i=0;i<14 && i<l;i++) buf[n++]=(char)v[i];
    }
    while (n < 14) buf[n++]='0';
    memcpy(out, buf, 14); out[14]=0;
}

/* Parse a Certificate DER into c. 0 ok, -1 malformed/unsupported. */
static int x509_parse(const uint8_t *der_buf, size_t len, x509_cert *c) {
    memset(c, 0, sizeof *c);
    der top, cert;
    top.p = der_buf; top.end = der_buf + len;
    if (der_into(&top, 0x30, &cert) < 0) return -1;      /* Certificate SEQ */
    /* tbsCertificate: capture its full TLV span for signature verification */
    const uint8_t *tbs_start = cert.p;
    der tbs;
    if (der_into(&cert, 0x30, &tbs) < 0) return -1;
    c->tbs = tbs_start; c->tbs_len = (size_t)(cert.p - tbs_start);
    /* signatureAlgorithm, signatureValue */
    c->sig_alg = parse_sigalg(&cert);
    const uint8_t *sp; size_t sl;
    if (der_tlv(&cert, &sp, &sl) != 0x03 || sl < 1 || sp[0] != 0) return -1; /* BIT STRING, 0 unused */
    c->sig = sp + 1; c->sig_len = sl - 1;

    /* inside tbsCertificate */
    const uint8_t *vp; size_t vl;
    /* optional [0] version */
    if (tbs.p < tbs.end && (uint8_t)*tbs.p == 0xa0) { if (der_skip(&tbs) < 0) return -1; }
    if (der_tlv(&tbs, &vp, &vl) != 0x02) return -1;      /* serial INTEGER */
    if (der_skip(&tbs) < 0) return -1;                    /* signature AlgId */
    if (der_skip(&tbs) < 0) return -1;                    /* issuer Name */
    /* validity SEQ { notBefore, notAfter } */
    der val;
    if (der_into(&tbs, 0x30, &val) < 0) return -1;
    int t1 = der_tlv(&val, &vp, &vl); norm_time(vp, vl, t1 == 0x17, c->not_before);
    int t2 = der_tlv(&val, &vp, &vl); norm_time(vp, vl, t2 == 0x17, c->not_after);
    if (t1 < 0 || t2 < 0) return -1;
    if (der_skip(&tbs) < 0) return -1;                    /* subject Name */
    /* SubjectPublicKeyInfo SEQ { AlgId SEQ { OID, params }, BIT STRING } */
    der spki, alg;
    if (der_into(&tbs, 0x30, &spki) < 0) return -1;
    if (der_into(&spki, 0x30, &alg) < 0) return -1;
    const uint8_t *ko; size_t kol;
    if (der_tlv(&alg, &ko, &kol) != 0x06) return -1;
    const uint8_t *keybits; size_t keybitslen;
    if (der_tlv(&spki, &keybits, &keybitslen) != 0x03 || keybitslen < 1 || keybits[0] != 0) return -1;
    keybits++; keybitslen--;
    if (oid_eq(ko, kol, OID_RSA_ENC, sizeof OID_RSA_ENC)) {
        c->key_alg = WO_X509_KEY_RSA;
        der rk; rk.p = keybits; rk.end = keybits + keybitslen;
        der rseq;
        if (der_into(&rk, 0x30, &rseq) < 0) return -1;   /* RSAPublicKey SEQ */
        const uint8_t *np; size_t nl;
        if (der_tlv(&rseq, &np, &nl) != 0x02) return -1; /* modulus */
        while (nl > 0 && np[0] == 0) { np++; nl--; }      /* drop leading 0 */
        c->rsa_n = np; c->rsa_n_len = nl;
        const uint8_t *ep; size_t el;
        if (der_tlv(&rseq, &ep, &el) != 0x02) return -1; /* exponent */
        c->rsa_e = ep; c->rsa_e_len = el;
    } else if (oid_eq(ko, kol, OID_EC_PUBKEY, sizeof OID_EC_PUBKEY)) {
        /* params must be the P-256 OID */
        const uint8_t *cp; size_t cl;
        if (der_tlv(&alg, &cp, &cl) != 0x06 || !oid_eq(cp, cl, OID_P256, sizeof OID_P256)) return -1;
        if (keybitslen != 65 || keybits[0] != 0x04) return -1; /* uncompressed point */
        c->key_alg = WO_X509_KEY_EC_P256;
        c->ec_x = keybits + 1; c->ec_y = keybits + 33;
    } else {
        return -1;
    }
    /* Remaining tbs bytes (optional uniqueIDs + [3] extensions). The chain
     * signature, SPKI and validity are settled above; the SAN/hostname check
     * walks this area on demand (wo_x509_check_host). */
    c->ext_area = tbs.p; c->ext_area_len = (size_t)(tbs.end - tbs.p);
    return 0;
}

static const uint8_t OID_SAN[] = { 0x55, 0x1d, 0x11 };  /* 2.5.29.17 */
static const uint8_t OID_BASIC_CONSTRAINTS[] = { 0x55, 0x1d, 0x13 }; /* 2.5.29.19 */
static const uint8_t OID_EKU[] = { 0x55, 0x1d, 0x25 };  /* 2.5.29.37 */
static const uint8_t OID_EKU_SERVER_AUTH[] = { 0x2b,0x06,0x01,0x05,0x05,0x07,0x03,0x01 };
static const uint8_t OID_EKU_ANY[] = { 0x55, 0x1d, 0x25, 0x00 }; /* anyExtendedKeyUsage */

/* Find the extension with OID `oid` in a parsed cert's extension area; fills
 * val/vallen with its extnValue (the OCTET STRING contents). Returns 1 found,
 * 0 not present, -1 malformed. */
static int x509_find_ext(const x509_cert *c, const uint8_t *oid, size_t oidn,
                         const uint8_t **val, size_t *vallen) {
    der r = { c->ext_area, c->ext_area + c->ext_area_len };
    while (r.p < r.end && (*r.p == 0x81 || *r.p == 0x82))
        if (der_skip(&r) < 0) return -1;
    der exp, exts;
    if (der_into(&r, 0xA3, &exp) < 0) return 0;    /* no [3] extensions */
    if (der_into(&exp, 0x30, &exts) < 0) return -1;
    while (exts.p < exts.end) {
        der ext;
        if (der_into(&exts, 0x30, &ext) < 0) return -1;
        const uint8_t *eo; size_t eol;
        if (der_tlv(&ext, &eo, &eol) != 0x06) return -1;
        if (ext.p < ext.end && *ext.p == 0x01)     /* optional critical */
            if (der_skip(&ext) < 0) return -1;
        const uint8_t *ev; size_t evl;
        if (der_tlv(&ext, &ev, &evl) != 0x04) return -1;
        if (oid_eq(eo, eol, oid, oidn)) { *val = ev; *vallen = evl; return 1; }
    }
    return 0;
}

/* basicConstraints (RFC 5280 §4.2.1.9): SEQUENCE { cA BOOLEAN DEFAULT FALSE,
 * pathLenConstraint INTEGER OPTIONAL }. Fills *is_ca and, when present,
 * *has_pathlen + *pathlen. Absent extension => not a CA. 0 ok, -1 malformed. */
int wo_x509_basic_constraints(const uint8_t *cert_der, size_t cert_len,
                              int *is_ca, int *has_pathlen, int *pathlen) {
    *is_ca = 0; *has_pathlen = 0; *pathlen = 0;
    x509_cert c;
    if (x509_parse(cert_der, cert_len, &c) != 0) return -1;
    const uint8_t *v; size_t vl;
    int f = x509_find_ext(&c, OID_BASIC_CONSTRAINTS, sizeof OID_BASIC_CONSTRAINTS, &v, &vl);
    if (f < 0) return -1;
    if (f == 0) return 0;                            /* absent -> not a CA */
    der bc; der d = { v, v + vl };
    if (der_into(&d, 0x30, &bc) < 0) return -1;      /* SEQUENCE */
    if (bc.p < bc.end && *bc.p == 0x01) {            /* cA BOOLEAN */
        const uint8_t *bv; size_t bvl;
        if (der_tlv(&bc, &bv, &bvl) != 0x01 || bvl != 1) return -1;
        *is_ca = bv[0] != 0;
    }
    if (bc.p < bc.end && *bc.p == 0x02) {            /* pathLenConstraint */
        const uint8_t *pv; size_t pvl;
        if (der_tlv(&bc, &pv, &pvl) != 0x02 || pvl == 0 || pvl > 2) return -1;
        int n = 0;
        for (size_t i = 0; i < pvl; i++) n = (n << 8) | pv[i];
        *has_pathlen = 1; *pathlen = n;
    }
    return 0;
}

/* Extended Key Usage (RFC 5280 §4.2.1.12). Server-usable if EKU is absent, or
 * present and lists id-kp-serverAuth or anyExtendedKeyUsage. Returns 1 if
 * usable as a TLS server cert, 0 otherwise. */
int wo_x509_eku_serverauth_ok(const uint8_t *cert_der, size_t cert_len) {
    x509_cert c;
    if (x509_parse(cert_der, cert_len, &c) != 0) return 0;
    const uint8_t *v; size_t vl;
    int f = x509_find_ext(&c, OID_EKU, sizeof OID_EKU, &v, &vl);
    if (f < 0) return 0;
    if (f == 0) return 1;                            /* absent -> allowed */
    der seq, d = { v, v + vl };
    if (der_into(&d, 0x30, &seq) < 0) return 0;      /* SEQUENCE OF OID */
    while (seq.p < seq.end) {
        const uint8_t *ko; size_t kol;
        if (der_tlv(&seq, &ko, &kol) != 0x06) return 0;
        if (oid_eq(ko, kol, OID_EKU_SERVER_AUTH, sizeof OID_EKU_SERVER_AUTH) ||
            oid_eq(ko, kol, OID_EKU_ANY, sizeof OID_EKU_ANY))
            return 1;
    }
    return 0;                                         /* present, no serverAuth */
}

/* Case-insensitive match of a presented dNSName pattern against a hostname,
 * with a single left-most "*" wildcard (RFC 6125 §6.4.3): "*.example.com"
 * matches one label, never a bare "example.com" or a dotted sub-label. */
static int host_match(const char *pat, size_t patlen, const char *host,
                      size_t hostlen) {
    if (patlen == 0 || hostlen == 0) return 0;
    if (pat[0] == '*') {
        /* pattern is "*"+rest; rest must start with '.'. Match the suffix and
         * require the wildcard to cover exactly one (non-empty, dot-free) label. */
        if (patlen < 2 || pat[1] != '.') return 0;
        const char *rest = pat + 1; size_t restlen = patlen - 1;
        if (hostlen <= restlen) return 0;
        size_t hlead = hostlen - restlen;          /* the part '*' must cover */
        for (size_t i = 0; i < hlead; i++)
            if (host[i] == '.') return 0;           /* no dot in the wildcard */
        /* suffix compare, case-insensitive */
        for (size_t i = 0; i < restlen; i++) {
            char a = rest[i], b = host[hlead + i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) return 0;
        }
        return 1;
    }
    if (patlen != hostlen) return 0;
    for (size_t i = 0; i < patlen; i++) {
        char a = pat[i], b = host[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

/* Verify `hostname` against the certificate's subjectAltName dNSName entries
 * (RFC 6125). Returns 1 if any SAN dNSName matches, 0 otherwise (including no
 * SAN present — a cert without SAN is not accepted for a hostname). The legacy
 * CN fallback is deliberately not implemented. */
int wo_x509_check_host(const uint8_t *cert_der, size_t cert_len,
                       const char *hostname, size_t hostlen) {
    x509_cert c;
    if (x509_parse(cert_der, cert_len, &c) != 0) return 0;
    der r = { c.ext_area, c.ext_area + c.ext_area_len };
    /* skip optional issuerUniqueID [1] (0x81) / subjectUniqueID [2] (0x82) */
    while (r.p < r.end && (*r.p == 0x81 || *r.p == 0x82))
        if (der_skip(&r) < 0) return 0;
    der exp;
    if (der_into(&r, 0xA3, &exp) < 0) return 0;    /* [3] EXPLICIT */
    der exts;
    if (der_into(&exp, 0x30, &exts) < 0) return 0; /* SEQUENCE OF Extension */
    while (exts.p < exts.end) {
        der ext;
        if (der_into(&exts, 0x30, &ext) < 0) return 0;
        const uint8_t *oid; size_t oidlen;
        if (der_tlv(&ext, &oid, &oidlen) != 0x06) return 0;
        /* optional critical BOOLEAN */
        if (ext.p < ext.end && *ext.p == 0x01)
            if (der_skip(&ext) < 0) return 0;
        const uint8_t *val; size_t vallen;
        if (der_tlv(&ext, &val, &vallen) != 0x04) return 0;  /* OCTET STRING */
        if (!oid_eq(oid, oidlen, OID_SAN, sizeof OID_SAN)) continue;
        /* val = SEQUENCE OF GeneralName; dNSName is [2] IMPLICIT IA5String. */
        der names = { val, val + vallen }, seq;
        if (der_into(&names, 0x30, &seq) < 0) return 0;
        while (seq.p < seq.end) {
            const uint8_t *gn; size_t gnlen;
            int tag = der_tlv(&seq, &gn, &gnlen);
            if (tag < 0) return 0;
            if (tag == 0x82 &&                        /* dNSName */
                host_match((const char *)gn, gnlen, hostname, hostlen))
                return 1;
        }
        return 0;                                     /* SAN present, no match */
    }
    return 0;                                          /* no SAN extension */
}

/* Verify `c`'s signature over its tbsCertificate using an issuer public key
 * already parsed into `issuer`. 1 valid, 0 otherwise. */
static int x509_verify_sig(const x509_cert *c, const x509_cert *issuer) {
    uint8_t h[32];
    wo_sha256(c->tbs, c->tbs_len, h);
    if (c->sig_alg == WO_X509_SIG_RSA_PKCS1_SHA256) {
        if (issuer->key_alg != WO_X509_KEY_RSA) return 0;
        return wo_rsa_pkcs1_sha256_verify(issuer->rsa_n, issuer->rsa_n_len,
                                          issuer->rsa_e, issuer->rsa_e_len,
                                          c->sig, c->sig_len, h);
    }
    if (c->sig_alg == WO_X509_SIG_RSA_PSS_SHA256) {
        if (issuer->key_alg != WO_X509_KEY_RSA) return 0;
        return wo_rsa_pss_sha256_verify(issuer->rsa_n, issuer->rsa_n_len,
                                        issuer->rsa_e, issuer->rsa_e_len,
                                        c->sig, c->sig_len, h, 32);
    }
    if (c->sig_alg == WO_X509_SIG_ECDSA_P256_SHA256) {
        if (issuer->key_alg != WO_X509_KEY_EC_P256) return 0;
        /* ECDSA signature is SEQ { r INTEGER, s INTEGER } */
        der s; s.p = c->sig; s.end = c->sig + c->sig_len;
        der sq;
        if (der_into(&s, 0x30, &sq) < 0) return 0;
        const uint8_t *rp, *spp; size_t rl, spl;
        if (der_tlv(&sq, &rp, &rl) != 0x02) return 0;
        if (der_tlv(&sq, &spp, &spl) != 0x02) return 0;
        uint8_t r32[32] = {0}, s32[32] = {0};
        while (rl > 0 && rp[0] == 0) { rp++; rl--; }
        while (spl > 0 && spp[0] == 0) { spp++; spl--; }
        if (rl > 32 || spl > 32) return 0;
        memcpy(r32 + (32 - rl), rp, rl);
        memcpy(s32 + (32 - spl), spp, spl);
        return wo_ecdsa_p256_sha256_verify(issuer->ec_x, issuer->ec_y, r32, s32, h);
    }
    return 0;
}

/* Public: verify one DER cert's signature against a DER issuer cert (or the
 * same cert, for a self-signed root). Also returns the parsed leaf fields via
 * out (may be NULL). 1 valid, 0 otherwise. */
int wo_x509_verify_one(const uint8_t *cert_der, size_t cert_len,
                       const uint8_t *issuer_der, size_t issuer_len) {
    x509_cert c, iss;
    if (x509_parse(cert_der, cert_len, &c) != 0) return 0;
    if (x509_parse(issuer_der, issuer_len, &iss) != 0) return 0;
    return x509_verify_sig(&c, &iss);
}

/* Check a cert's validity window against a caller-supplied current time, given
 * as a 14-char "YYYYMMDDHHMMSS" string (the format norm_time produces, so the
 * comparison is a plain lexicographic memcmp). The current time is the caller's
 * to supply — TLS (phase F) passes wall-clock; the test passes a fixed instant.
 * 1 if not_before <= now <= not_after, 0 otherwise (or on parse failure). */
int wo_x509_check_validity(const uint8_t *cert_der, size_t cert_len,
                           const char now14[14]) {
    x509_cert c;
    if (x509_parse(cert_der, cert_len, &c) != 0) return 0;
    if (memcmp(now14, c.not_before, 14) < 0) return 0;
    if (memcmp(now14, c.not_after, 14) > 0) return 0;
    return 1;
}

/* Extract SPKI: returns key_alg (WO_X509_KEY_*) and fills the key spans via the
 * out params (RSA n/e or EC x/y). 0 alg on parse failure. */
int wo_x509_parse_spki(const uint8_t *cert_der, size_t cert_len, int *key_alg,
                       const uint8_t **rsa_n, size_t *rsa_n_len,
                       const uint8_t **rsa_e, size_t *rsa_e_len,
                       const uint8_t **ec_x, const uint8_t **ec_y) {
    x509_cert c;
    if (x509_parse(cert_der, cert_len, &c) != 0) { *key_alg = 0; return -1; }
    *key_alg = c.key_alg;
    if (rsa_n) *rsa_n = c.rsa_n;
    if (rsa_n_len) *rsa_n_len = c.rsa_n_len;
    if (rsa_e) *rsa_e = c.rsa_e;
    if (rsa_e_len) *rsa_e_len = c.rsa_e_len;
    if (ec_x) *ec_x = c.ec_x;
    if (ec_y) *ec_y = c.ec_y;
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

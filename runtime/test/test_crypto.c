/* test_crypto — the digest cores against published vectors: SHA-1 (RFC
 * 3174), SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 4231 cases 1-4), plus a
 * 63/64/65-byte block-boundary sweep. Boundary constants precomputed with
 * python3: hashlib.sha256(b"a"*63).hexdigest() etc. */
#include <stdio.h>
#include <string.h>

#include "crypto.h"
#include "t.h"

static void hex(const uint8_t *d, size_t n, char *out) {
    static const char *h = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = h[d[i] >> 4];
        out[i * 2 + 1] = h[d[i] & 15];
    }
    out[n * 2] = 0;
}

static void t_sha1(const char *msg, size_t len, const char *want) {
    uint8_t d[20];
    char got[41];
    wo_sha1((const uint8_t *)msg, len, d);
    hex(d, 20, got);
    T_CHECK(strcmp(got, want) == 0);
}

static void t_sha256(const char *msg, size_t len, const char *want) {
    uint8_t d[32];
    char got[65];
    wo_sha256((const uint8_t *)msg, len, d);
    hex(d, 32, got);
    T_CHECK(strcmp(got, want) == 0);
}

static void t_hmac(const uint8_t *key, size_t klen, const char *msg,
                   size_t mlen, const char *want) {
    uint8_t d[32];
    char got[65];
    wo_hmac_sha256(key, klen, (const uint8_t *)msg, mlen, d);
    hex(d, 32, got);
    T_CHECK(strcmp(got, want) == 0);
}

/* rv2 8 phase A: ChaCha20-Poly1305 (RFC 8439 §2.5.2 Poly1305 + §2.8.2 AEAD) */
static void t_poly1305(const uint8_t key[32], const char *msg, size_t mlen,
                       const char *want) {
    uint8_t tag[16];
    char got[33];
    wo_poly1305(key, (const uint8_t *)msg, mlen, tag);
    hex(tag, 16, got);
    T_CHECK(strcmp(got, want) == 0);
}

static size_t unhex(const char *h, uint8_t *out) {
    size_t n = strlen(h) / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
    return n;
}

/* NIST SP 800-38D GCM test vector: seal matches ct||tag, open round-trips,
 * a flipped tag is rejected. `cth` is the expected ciphertext concatenated
 * with the 16-byte tag. */
static void t_aesgcm(const char *kh, const char *ih, const char *ah,
                     const char *ph, const char *cth) {
    uint8_t key[32], iv[12], aad[64], pt[256], expect[272], out[272], back[256];
    size_t klen = unhex(kh, key), alen = unhex(ah, aad);
    unhex(ih, iv);
    size_t plen = unhex(ph, pt), elen = unhex(cth, expect);
    T_CHECK(elen == plen + 16);
    T_CHECK(wo_aes_gcm_seal(key, klen, iv, aad, alen, pt, plen, out) == 0);
    T_CHECK(memcmp(out, expect, plen + 16) == 0);
    T_CHECK(wo_aes_gcm_open(key, klen, iv, aad, alen, out, plen, out + plen,
                            back) == 0);
    T_CHECK(memcmp(back, pt, plen) == 0);
    uint8_t bad[16];
    memcpy(bad, out + plen, 16);
    bad[0] ^= 0x01;
    T_CHECK(wo_aes_gcm_open(key, klen, iv, aad, alen, out, plen, bad, back) == 1);
}

int main(void) {
    /* RFC 3174 */
    t_sha1("abc", 3, "a9993e364706816aba3e25717850c26c9cd0d89d");
    t_sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
           "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    t_sha1("", 0, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    /* the WebSocket handshake's exact input (RFC 6455 §1.3 worked example):
     * sha1("dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11") */
    t_sha1("dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 60,
           "b37a4f2cc0624f1690f64606cf385945b2bec4ea");

    /* FIPS 180-4 */
    t_sha256("abc", 3,
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    t_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    t_sha256("", 0,
             "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    /* block boundaries: python3 -c 'import hashlib
       [print(hashlib.sha256(b"a"*n).hexdigest()) for n in (63,64,65)]' */
    char a65[65];
    memset(a65, 'a', 65);
    t_sha256(a65, 63,
             "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
    t_sha256(a65, 64,
             "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    t_sha256(a65, 65,
             "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0");
    /* same sweep for sha1: hashlib.sha1 */
    t_sha1(a65, 63, "03f09f5b158a7a8cdad920bddc29b81c18a551f5");
    t_sha1(a65, 64, "0098ba824b5c16427bd7a1122a5a442a25ec644d");
    t_sha1(a65, 65, "11655326c708d70319be2610e8a57d9a5b959d3b");

    /* RFC 4231 */
    {
        uint8_t k1[20];
        memset(k1, 0x0b, 20);
        t_hmac(k1, 20, "Hi There", 8,
               "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    }
    t_hmac((const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 28,
           "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    {
        uint8_t k3[20], m3[50];
        memset(k3, 0xaa, 20);
        memset(m3, 0xdd, 50);
        t_hmac(k3, 20, (const char *)m3, 50,
               "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
    }
    {
        uint8_t k4[25], m4[50];
        for (int i = 0; i < 25; i++) k4[i] = (uint8_t)(i + 1);
        memset(m4, 0xcd, 50);
        t_hmac(k4, 25, (const char *)m4, 50,
               "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");
    }
    /* long-key path (key > 64 bytes is hashed first): RFC 4231 case 6 */
    {
        uint8_t k6[131];
        memset(k6, 0xaa, 131);
        t_hmac(k6, 131, "Test Using Larger Than Block-Size Key - Hash Key First",
               54,
               "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    }

    /* RFC 8439 §2.5.2 — Poly1305 */
    {
        uint8_t pk[32];
        for (int i = 0; i < 32; i++) pk[i] = 0;
        static const uint8_t pkv[32] = {
            0x85, 0xd6, 0xbe, 0x78, 0x57, 0x55, 0x6d, 0x33, 0x7f, 0x44, 0x52,
            0xfe, 0x42, 0xd5, 0x06, 0xa8, 0x01, 0x03, 0x80, 0x8a, 0xfb, 0x0d,
            0xb2, 0xfd, 0x4a, 0xbf, 0xf6, 0xaf, 0x41, 0x49, 0xf5, 0x1b };
        memcpy(pk, pkv, 32);
        t_poly1305(pk, "Cryptographic Forum Research Group", 34,
                   "a8061dc1305136c6c22b8baf0c0127a9");
    }

    /* RFC 8439 §2.8.2 — ChaCha20-Poly1305 AEAD: seal matches the vector,
     * open round-trips, and a tampered tag is rejected. */
    {
        uint8_t key[32], nonce[12], aad[12];
        for (int i = 0; i < 32; i++) key[i] = (uint8_t)(0x80 + i);
        static const uint8_t nv[12] = { 0x07, 0x00, 0x00, 0x00, 0x40, 0x41,
                                        0x42, 0x43, 0x44, 0x45, 0x46, 0x47 };
        static const uint8_t av[12] = { 0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1,
                                        0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7 };
        memcpy(nonce, nv, 12);
        memcpy(aad, av, 12);
        const char *pt =
            "Ladies and Gentlemen of the class of '99: If I could offer you "
            "only one tip for the future, sunscreen would be it.";
        size_t ptlen = strlen(pt);
        uint8_t out[114 + 16];
        int rc = wo_chacha20poly1305_seal(key, nonce, aad, 12,
                                          (const uint8_t *)pt, ptlen, out);
        T_CHECK(rc == 0);
        char got[(114 + 16) * 2 + 1];
        hex(out, ptlen + 16, got);
        T_CHECK(strcmp(got,
            "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
            "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
            "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
            "3ff4def08e4b7a9de576d26586cec64b61161ae10b594f09e26a7e902ecbd060"
            "0691") == 0);

        uint8_t back[114];
        rc = wo_chacha20poly1305_open(key, nonce, aad, 12, out, ptlen,
                                      out + ptlen, back);
        T_CHECK(rc == 0);
        T_CHECK(memcmp(back, pt, ptlen) == 0);

        /* flip one tag bit — must be rejected (rc == 1), not decrypted */
        uint8_t tampered[16];
        memcpy(tampered, out + ptlen, 16);
        tampered[0] ^= 0x01;
        rc = wo_chacha20poly1305_open(key, nonce, aad, 12, out, ptlen,
                                      tampered, back);
        T_CHECK(rc == 1);
    }

    /* AES-GCM (rv2 8 phase B) — NIST SP 800-38D test vectors, when hardware
     * AES is present (the bitsliced software fallback is phase C). */
    if (wo_aes_gcm_available()) {
        /* AES-128 GCM test case 4 */
        t_aesgcm("feffe9928665731c6d6a8f9467308308",
                 "cafebabefacedbaddecaf888",
                 "feedfacedeadbeeffeedfacedeadbeefabaddad2",
                 "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721"
                 "c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
                 "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e2"
                 "1d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091"
                 "5bc94fbc3221a5db94fae95ae7121a47");
        /* AES-256 GCM test case 16 */
        t_aesgcm("feffe9928665731c6d6a8f9467308308"
                 "feffe9928665731c6d6a8f9467308308",
                 "cafebabefacedbaddecaf888",
                 "feedfacedeadbeeffeedfacedeadbeefabaddad2",
                 "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721"
                 "c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
                 "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8"
                 "cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662"
                 "76fc6ece0f4e1768cddf8853bb2d551b");
    }

    return t_report("test_crypto");
}

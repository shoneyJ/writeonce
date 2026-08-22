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

    return t_report("test_crypto");
}

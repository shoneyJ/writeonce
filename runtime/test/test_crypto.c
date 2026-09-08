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

static void t_x25519(const char *kh, const char *uh, const char *wanth);

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

static void t_x25519(const char *kh, const char *uh, const char *wanth) {
    uint8_t k[32], u[32], out[32];
    char got[65];
    unhex(kh, k); unhex(uh, u);
    wo_x25519(out, k, u);
    hex(out, 32, got);
    T_CHECK(strcmp(got, wanth) == 0);
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

    /* rv2 8 phase C — the portable constant-time software path, forced on
     * (works regardless of hardware), against the same NIST vectors. */
    wo_aes_force_software = 1;
    t_aesgcm("feffe9928665731c6d6a8f9467308308",
             "cafebabefacedbaddecaf888",
             "feedfacedeadbeeffeedfacedeadbeefabaddad2",
             "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721"
             "c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
             "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e2"
             "1d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091"
             "5bc94fbc3221a5db94fae95ae7121a47");
    t_aesgcm("feffe9928665731c6d6a8f9467308308"
             "feffe9928665731c6d6a8f9467308308",
             "cafebabefacedbaddecaf888",
             "feedfacedeadbeeffeedfacedeadbeefabaddad2",
             "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721"
             "c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
             "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8"
             "cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662"
             "76fc6ece0f4e1768cddf8853bb2d551b");
    wo_aes_force_software = 0;

    /* HKDF-SHA256 (rv2 9 phase B): RFC 5869 Test Case 1 (Extract + Expand). */
    {
        uint8_t ikm[22], salt[13], info[10], prk[32], okm[42];
        char got[85];
        memset(ikm, 0x0b, 22);
        for (int i = 0; i < 13; i++) salt[i] = (uint8_t)i;
        for (int i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);
        wo_hkdf_sha256_extract(salt, 13, ikm, 22, prk);
        hex(prk, 32, got);
        T_CHECK(strcmp(got,
            "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5") == 0);
        T_CHECK(wo_hkdf_sha256_expand(prk, info, 10, okm, 42) == 0);
        hex(okm, 42, got);
        T_CHECK(strcmp(got,
            "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
            "34007208d5b887185865") == 0);
    }
    /* HKDF-Expand-Label (RFC 8446 §7.1), reference values from a known-good
     * HKDF-Expand over the tls13 label struct. secret = 0x00..0x1f. */
    {
        uint8_t secret[32], out[32], h[32];
        char got[65];
        for (int i = 0; i < 32; i++) secret[i] = (uint8_t)i;
        T_CHECK(wo_hkdf_sha256_expand_label(secret, "key", 3, NULL, 0, out, 16) == 0);
        hex(out, 16, got);
        T_CHECK(strcmp(got, "9c9783cf77ea32d44f369da41f19f3cc") == 0);
        T_CHECK(wo_hkdf_sha256_expand_label(secret, "iv", 2, NULL, 0, out, 12) == 0);
        hex(out, 12, got);
        T_CHECK(strcmp(got, "2f41c846a431a163814bcd71") == 0);
        /* with a context = SHA-256("") (a Derive-Secret shape) */
        wo_sha256((const uint8_t *)"", 0, h);
        T_CHECK(wo_hkdf_sha256_expand_label(secret, "derived", 7, h, 32, out, 32) == 0);
        hex(out, 32, got);
        T_CHECK(strcmp(got,
            "a5b1caa258481fdf573ac069f281e534e4a2379ec9e457e0c8494c227efb40e6") == 0);
    }

    /* X25519 (rv2 9 phase C) — RFC 7748 §5.2 direct vectors */
    t_x25519("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
             "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
             "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
    t_x25519("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
             "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
             "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957");
    /* RFC 7748 §5.2 iterated test: k=u=9, iterate; check after 1 and 1000. */
    {
        uint8_t k[32] = { 9 }, u[32] = { 9 }, r[32];
        char got[65];
        for (int it = 1; it <= 1000; it++) {
            wo_x25519(r, k, u);
            memcpy(u, k, 32);
            memcpy(k, r, 32);
            if (it == 1) {
                hex(k, 32, got);
                T_CHECK(strcmp(got,
                    "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079") == 0);
            }
        }
        hex(k, 32, got);
        T_CHECK(strcmp(got,
            "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51") == 0);
    }

    /* RSA-2048 signature verification (rv2 9 phase D) — vectors from
     * python cryptography (PKCS#1 v1.5 and PSS over SHA-256). */
    {
        static const char *N = "dacbb5ff5cc8c341b7027135cca2e76619abf23c4a043b25d1fe424eae008923a007836775669bf783683f6ef4ed4f25d492078a4557e737f7ec680131f7f5ab0bcc7e11554edeefbb2b5d2c8c2deb6af6e2054a3b75f580b2a1d16186b12fb5c1715991e9bb5d2646631b4bd14484157f5b300c61f902f74727813b5d0e5b872bbf7ea72e0f7b80d47bb5049f1f54cca186b9d1055767be79403a9e0796c45e17dd00de5cca8ee2e315525267f0369fdca6d18a0c06b651de59913d6a7d227400e12cd8e0b67121cf26dad92073bfa67b8d9b0016ae9dd2de058baa07f0bcde7bfa7786aafe7ff82046f71f48960ce81a68ff62bdd4658cca3837dfb1ed1b19";
        static const char *E = "010001";
        static const char *H = "1e1f1eb2f15f5ba5f16363e4c45d0e58ee171e7050bb088dd5125d1f536afe25";
        static const char *S1 = "a7bfe20510abd104f6eae7c440a1851c6f7cbd15266f671eff6096fa22b5cd61bb45cdaa819b39a25a20d08e019391282f00dd4ad4d02dbfc3da6e12930dbced8e0cbd65004b8955ae8f7eb8bf9fed477f6502e2a0e523665295ceb212155499dac4f40cca9c5038920678afda12f8f0591be4c7a3167efa0e30566dc207bbde47ca52e061ed7c557d37899698d9c5947b4ddc90286e50ca2d57114371307b50bf603759fd592b8e815398889ab6664b898126d56171acee58b1cd3130b0f2dcc85d0f0ccdcf586914bdb8a53a2985095206cb5bed3712531438b5b9b2861ac25819549b6bc6a7f7b19557c7825563b38e302996d317d4e5f9c6dc6356460268";
        static const char *SP = "9c66f584c7781cc0a599585a01ef2d892eba67005e353e51ff677e3e64b3d45543703118872d76bebf6e17a1b0b8a6a08186ef2bde6f4f9b264952c62cde8c1ea2ab2635fd022f5b0d358e98835871e4212112a47445796e87c0d7df4f674c9ed726a90f92bfe72c99b8015a786e08f3176b296c70c8bb815bfd32869a795a9d8b046416d145eb476ea6a02ecac046f7f8da4365e047cd2ea1e5da78fb76ca8c5f762db1136599e423beb864a24be7f6344aeb51e1973fe6885d2d1de378cc3ffa3e5a3cbcd95331397c4650c792f3003bffb5418d2df833298c6a6f2dcd86e07e5684a46ab25c3ba6cb712fecce77750a1a2e8baecf41455b837569423a3a71";
        uint8_t n[256], e[8], h[32], s[256];
        size_t nl = unhex(N, n), el = unhex(E, e);
        unhex(H, h);
        size_t sl = unhex(S1, s);
        T_CHECK(wo_rsa_pkcs1_sha256_verify(n, nl, e, el, s, sl, h) == 1);
        s[10] ^= 0x01; /* tamper */
        T_CHECK(wo_rsa_pkcs1_sha256_verify(n, nl, e, el, s, sl, h) == 0);
        sl = unhex(SP, s);
        T_CHECK(wo_rsa_pss_sha256_verify(n, nl, e, el, s, sl, h, 32) == 1);
        s[10] ^= 0x01;
        T_CHECK(wo_rsa_pss_sha256_verify(n, nl, e, el, s, sl, h, 32) == 0);
        unhex(H, h); h[0] ^= 0x01; sl = unhex(S1, s); /* wrong hash rejected */
        T_CHECK(wo_rsa_pkcs1_sha256_verify(n, nl, e, el, s, sl, h) == 0);
    }

    return t_report("test_crypto");
}

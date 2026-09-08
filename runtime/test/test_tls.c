/* test_tls.c — TLS 1.3 record layer (runtime-v2 9 phase F1). KAT against
 * python's AEAD as oracle (tls_record_vectors.h), plus seal/open round-trip,
 * a tamper-rejection, and the sequence-number nonce advancing. ASan/UBSan. */
#include <stdint.h>
#include <string.h>

#include "tls.h"
#include "t.h"
#include "tls_record_vectors.h"

/* hex string -> bytes; returns the byte count. */
static size_t unhex(const char *h, uint8_t *out) {
    size_t n = 0;
    for (; h[0] && h[1]; h += 2) {
        unsigned v;
        sscanf(h, "%2x", &v);
        out[n++] = (uint8_t)v;
    }
    return n;
}

int main(void) {
    uint8_t iv[12], aeskey[16], chakey[32], pt[TLSREC_PTLEN];
    memcpy(iv, TLSREC_IV, 12);
    memcpy(aeskey, TLSREC_AESKEY, 16);
    memcpy(chakey, TLSREC_CHAKEY, 32);
    memcpy(pt, TLSREC_PT, TLSREC_PTLEN);

    /* AES-128-GCM: sealed record must equal python's byte-for-byte. */
    {
        uint8_t out[TLSREC_PTLEN + WO_TLS_RECORD_OVERHEAD];
        int n = wo_tls_record_seal(WO_TLS_AES_128_GCM_SHA256, aeskey, 16, iv,
                                   TLSREC_SEQ, TLSREC_CT, pt, TLSREC_PTLEN, out);
        T_CHECK(n == TLSREC_AESLEN);
        T_CHECK(memcmp(out, TLSREC_AES, TLSREC_AESLEN) == 0);
    }
    /* ChaCha20-Poly1305: same. */
    {
        uint8_t out[TLSREC_PTLEN + WO_TLS_RECORD_OVERHEAD];
        int n = wo_tls_record_seal(WO_TLS_CHACHA20_POLY1305_SHA256, chakey, 32,
                                   iv, TLSREC_SEQ, TLSREC_CT, pt, TLSREC_PTLEN,
                                   out);
        T_CHECK(n == TLSREC_CHALEN);
        T_CHECK(memcmp(out, TLSREC_CHA, TLSREC_CHALEN) == 0);
    }

    /* open() recovers the record python sealed: content, type, length. */
    {
        uint8_t rec[TLSREC_AESLEN], out[TLSREC_AESLEN]; uint8_t ct = 0;
        memcpy(rec, TLSREC_AES, TLSREC_AESLEN);
        int n = wo_tls_record_open(WO_TLS_AES_128_GCM_SHA256, aeskey, 16, iv,
                                   TLSREC_SEQ, rec, TLSREC_AESLEN, out, &ct);
        T_CHECK(n == TLSREC_PTLEN);
        T_CHECK(ct == TLSREC_CT);
        T_CHECK(memcmp(out, pt, TLSREC_PTLEN) == 0);
    }

    /* Round-trip both suites over several sequence numbers (nonce advances). */
    for (int suite = 1; suite <= 2; suite++) {
        const uint8_t *k = suite == WO_TLS_AES_128_GCM_SHA256 ? aeskey : chakey;
        size_t kl = suite == WO_TLS_AES_128_GCM_SHA256 ? 16 : 32;
        for (uint64_t seq = 0; seq < 5; seq++) {
            uint8_t msg[40], rec[40 + WO_TLS_RECORD_OVERHEAD];
            uint8_t got[sizeof rec]; uint8_t ct = 0;
            for (size_t i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)(i + seq);
            int n = wo_tls_record_seal(suite, k, kl, iv, seq,
                                       WO_TLS_CT_APPLICATION_DATA, msg,
                                       sizeof msg, rec);
            T_CHECK(n > 0);
            int m = wo_tls_record_open(suite, k, kl, iv, seq, rec, (size_t)n,
                                       got, &ct);
            T_CHECK(m == (int)sizeof msg);
            T_CHECK(ct == WO_TLS_CT_APPLICATION_DATA);
            T_CHECK(memcmp(got, msg, sizeof msg) == 0);
        }
    }

    /* A tampered record fails to open; a wrong sequence number fails too. */
    {
        uint8_t rec[TLSREC_AESLEN], out[TLSREC_AESLEN]; uint8_t ct = 0;
        memcpy(rec, TLSREC_AES, TLSREC_AESLEN);
        rec[10] ^= 0x01;
        T_CHECK(wo_tls_record_open(WO_TLS_AES_128_GCM_SHA256, aeskey, 16, iv,
                                   TLSREC_SEQ, rec, TLSREC_AESLEN, out, &ct) == -1);
        memcpy(rec, TLSREC_AES, TLSREC_AESLEN);
        T_CHECK(wo_tls_record_open(WO_TLS_AES_128_GCM_SHA256, aeskey, 16, iv,
                                   TLSREC_SEQ + 1, rec, TLSREC_AESLEN, out,
                                   &ct) == -1);
        /* A length-field lie is rejected before the AEAD. */
        memcpy(rec, TLSREC_AES, TLSREC_AESLEN);
        rec[4] ^= 0x01;
        T_CHECK(wo_tls_record_open(WO_TLS_AES_128_GCM_SHA256, aeskey, 16, iv,
                                   TLSREC_SEQ, rec, TLSREC_AESLEN, out, &ct) == -1);
    }

    /* A bad suite id is rejected, not misdispatched. */
    {
        uint8_t out[64];
        T_CHECK(wo_tls_record_seal(99, aeskey, 16, iv, 0, 23, pt, TLSREC_PTLEN,
                                   out) == -1);
    }

    /* Key schedule (phase F2) against RFC 8448 §3 "Simple 1-RTT Handshake". */
    {
        uint8_t ecdhe[32], hash_ch_sh[32], hash_ch_sf[32];
        uint8_t c_hs[32], s_hs[32], c_ap[32], s_ap[32], master[32];
        uint8_t s_hs_key[16], s_hs_iv[12];
        unhex("8bd4054fb55b9d63fdfbacf9f04b9f0d35e6d63f537563efd46272900f89492d", ecdhe);
        unhex("860c06edc07858ee8e78f0e7428c58edd6b43f2ca3e6e95f02ed063cf0e1cad8", hash_ch_sh);
        unhex("9608102a0f1ccc6db6250b7b7e417b1a000eaada3daae4777a7686c9ff83df13", hash_ch_sf);
        unhex("b3eddb126e067f35a780b3abf45e2d8f3b1a950738f52e9600746a0e27a55a21", c_hs);
        unhex("b67b7d690cc16c4e75e54213cb2d37b4e9c912bcded9105d42befd59d391ad38", s_hs);
        unhex("18df06843d13a08bf2a449844c5f8a478001bc4d4c627984d5a41da8d0402919", master);
        unhex("9e40646ce79a7f9dc05af8889bce6552875afa0b06df0087f792ebb7c17504a5", c_ap);
        unhex("a11af9f05531f856ad47116b45a950328204b4f44bfb6b3a4b4f1f3fcb631643", s_ap);
        unhex("3fce516009c21727d0f2e4e86ee403bc", s_hs_key);
        unhex("5d313eb2671276ee13000b30", s_hs_iv);

        wo_tls_key_schedule ks;
        wo_tls_derive_handshake(&ks, ecdhe, 32, hash_ch_sh);
        T_CHECK(memcmp(ks.client_hs_traffic, c_hs, 32) == 0);
        T_CHECK(memcmp(ks.server_hs_traffic, s_hs, 32) == 0);
        T_CHECK(memcmp(ks.master_secret, master, 32) == 0);

        wo_tls_derive_application(&ks, hash_ch_sf);
        T_CHECK(memcmp(ks.client_ap_traffic, c_ap, 32) == 0);
        T_CHECK(memcmp(ks.server_ap_traffic, s_ap, 32) == 0);

        /* Traffic key + iv from the server hs traffic secret. */
        uint8_t k[16], vv[12];
        wo_tls_traffic_keys(ks.server_hs_traffic, 16, k, vv);
        T_CHECK(memcmp(k, s_hs_key, 16) == 0);
        T_CHECK(memcmp(vv, s_hs_iv, 12) == 0);
    }

    return t_report("test_tls");
}

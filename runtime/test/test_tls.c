/* test_tls.c — TLS 1.3 record layer (runtime-v2 9 phase F1). KAT against
 * python's AEAD as oracle (tls_record_vectors.h), plus seal/open round-trip,
 * a tamper-rejection, and the sequence-number nonce advancing. ASan/UBSan. */
#include <stdint.h>
#include <string.h>

#include "tls.h"
#include "crypto.h"
#include "t.h"
#include "tls_record_vectors.h"
#include "tls_hs_vectors.h"
#include "tls_driver_vectors.h"

/* RFC 8448 §3 recorded ServerHello handshake message (90 octets). */
#define SH_MSG "\x02\x00\x00\x56\x03\x03\xa6\xaf\x06\xa4\x12\x18\x60\xdc\x5e\x6e\x60\x24\x9c\xd3\x4c\x95\x93\x0c\x8a\xc5\xcb\x14\x34\xda\xc1\x55\x77\x2e\xd3\xe2\x69\x28\x00\x13\x01\x00\x00\x2e\x00\x33\x00\x24\x00\x1d\x00\x20\xc9\x82\x88\x76\x11\x20\x95\xfe\x66\x76\x2b\xdb\xf7\xc6\x72\xe1\x56\xd6\xcc\x25\x3b\x83\x3d\xf1\xdd\x69\xb1\xb0\x4e\x75\x1f\x0f\x00\x2b\x00\x02\x03\x04"
#define SH_MSG_LEN 90

/* naive subsequence search (test-only). */
static int contains(const uint8_t *hay, size_t hn, const uint8_t *needle, size_t nn) {
    if (nn > hn) return 0;
    for (size_t i = 0; i + nn <= hn; i++)
        if (memcmp(hay + i, needle, nn) == 0) return 1;
    return 0;
}

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

    /* ServerHello parser (phase F3) against the RFC 8448 recorded message. */
    {
        uint8_t sh[SH_MSG_LEN]; memcpy(sh, SH_MSG, SH_MSG_LEN);
        int suite = 0; uint8_t spub[32];
        T_CHECK(wo_tls_parse_server_hello(sh, SH_MSG_LEN, &suite, spub) == 0);
        T_CHECK(suite == WO_TLS_AES_128_GCM_SHA256);   /* 0x1301 */
        uint8_t want_spub[32];
        unhex("c9828876112095fe66762bdbf7c672e156d6cc253b833df1dd69b1b04e751f0f", want_spub);
        T_CHECK(memcmp(spub, want_spub, 32) == 0);

        /* Malformed inputs are rejected, never over-read. */
        T_CHECK(wo_tls_parse_server_hello(sh, 10, &suite, spub) == -1);   /* truncated */
        uint8_t bad[SH_MSG_LEN]; memcpy(bad, SH_MSG, SH_MSG_LEN);
        bad[0] = 0x01;                                  /* wrong handshake type */
        T_CHECK(wo_tls_parse_server_hello(bad, SH_MSG_LEN, &suite, spub) == -1);
        memcpy(bad, SH_MSG, SH_MSG_LEN);
        bad[39] = 0x02;                                 /* cipher suite 0x1302 unsupported */
        T_CHECK(wo_tls_parse_server_hello(bad, SH_MSG_LEN, &suite, spub) == -1);
    }

    /* ClientHello builder (phase F3): structural checks + SNI/keyshare present. */
    {
        uint8_t cpub[32], rnd[32], sid[32];
        for (int j = 0; j < 32; j++) { cpub[j] = (uint8_t)j; rnd[j] = (uint8_t)(j + 1); sid[j] = (uint8_t)(j + 2); }
        const char *host = "api.anthropic.com";
        uint8_t ch[512]; size_t chl = 0;
        T_CHECK(wo_tls_build_client_hello(host, strlen(host), cpub, rnd, sid,
                                          ch, sizeof ch, &chl) == 0);
        T_CHECK(ch[0] == 1);                            /* client_hello */
        size_t declared = ((size_t)ch[1] << 16) | ((size_t)ch[2] << 8) | ch[3];
        T_CHECK(declared == chl - 4);                   /* length field consistent */
        T_CHECK(contains(ch, chl, (const uint8_t *)host, strlen(host)));  /* SNI */
        T_CHECK(contains(ch, chl, cpub, 32));           /* x25519 key share */
        /* Our ServerHello parser must not accept a ClientHello. */
        int suite; uint8_t spub[32];
        T_CHECK(wo_tls_parse_server_hello(ch, chl, &suite, spub) == -1);
        /* Too-small buffer refuses cleanly. */
        uint8_t tiny[32]; size_t tl;
        T_CHECK(wo_tls_build_client_hello(host, strlen(host), cpub, rnd, sid,
                                          tiny, sizeof tiny, &tl) == -1);
    }

    /* Full offline handshake verification (phase F3b) against RFC 8448 §3:
     * CertificateVerify (RSA-PSS), server Finished, and the client Finished we
     * would send — driven from the recorded handshake messages. */
    {
        /* running transcripts over the recorded handshake messages */
        uint8_t buf[2048]; size_t n = 0;
        #define ADD(a) do { memcpy(buf + n, a, sizeof a); n += sizeof a; } while (0)
        uint8_t th_cert[32], th_cv[32], th_sf[32];
        n = 0; ADD(hs_ch); ADD(hs_sh); ADD(hs_ee); ADD(hs_cert);
        wo_sha256(buf, n, th_cert);                    /* CH..Certificate */
        memcpy(buf + n, hs_cv, sizeof hs_cv); n += sizeof hs_cv;
        wo_sha256(buf, n, th_cv);                      /* CH..CertificateVerify */
        memcpy(buf + n, hs_sfin, sizeof hs_sfin); n += sizeof hs_sfin;
        wo_sha256(buf, n, th_sf);                      /* CH..server Finished */
        #undef ADD

        /* leaf cert out of the Certificate message; sig out of CertificateVerify */
        size_t p = 4; p += 1 + hs_cert[4];             /* skip ctx (len 0) */
        p += 3;                                         /* cert_list length */
        size_t clen = ((size_t)hs_cert[p] << 16) | ((size_t)hs_cert[p+1] << 8) | hs_cert[p+2];
        p += 3;
        const uint8_t *leaf = hs_cert + p;
        uint16_t scheme = ((uint16_t)hs_cv[4] << 8) | hs_cv[5];
        size_t siglen = ((size_t)hs_cv[6] << 8) | hs_cv[7];
        const uint8_t *sig = hs_cv + 8;
        T_CHECK(scheme == 0x0804);                     /* rsa_pss_rsae_sha256 */

        T_CHECK(wo_tls_verify_cert_verify(leaf, clen, scheme, sig, siglen, th_cert) == 1);
        /* wrong transcript hash and tampered signature both reject */
        uint8_t bad_th[32]; memcpy(bad_th, th_cert, 32); bad_th[0] ^= 1;
        T_CHECK(wo_tls_verify_cert_verify(leaf, clen, scheme, sig, siglen, bad_th) == 0);
        uint8_t bad_sig[256]; memcpy(bad_sig, sig, siglen); bad_sig[5] ^= 1;
        T_CHECK(wo_tls_verify_cert_verify(leaf, clen, scheme, bad_sig, siglen, th_cert) == 0);
        /* a scheme that mismatches the RSA leaf key is refused */
        T_CHECK(wo_tls_verify_cert_verify(leaf, clen, 0x0403, sig, siglen, th_cert) == 0);

        /* server Finished: recompute verify_data, compare to the recorded value
         * (skip the 4-byte handshake header). */
        uint8_t vd[32];
        wo_tls_finished_verify(hs_s_traffic, th_cv, vd);
        T_CHECK(memcmp(vd, hs_sfin + 4, 32) == 0);
        /* client Finished we would send matches the recorded one. */
        wo_tls_finished_verify(hs_c_traffic, th_sf, vd);
        T_CHECK(memcmp(vd, hs_cfin + 4, 32) == 0);
    }

    /* Sans-io client driver (phase F3c): the whole handshake driven offline
     * against the RFC 8448 record trace, then application data both ways. */
    {
        static wo_tls_client c;   /* ~40 KB — keep off the stack */
        uint8_t priv[32], ch[256];
        memcpy(priv, drv_client_priv, 32);
        memcpy(ch, drv_ch_msg, sizeof drv_ch_msg);
        T_CHECK(wo_tls_client_start_with(&c, ch, sizeof drv_ch_msg, priv) == 0);
        /* it framed a ClientHello record to send */
        uint8_t sent[512];
        size_t sn = wo_tls_client_take_output(&c, sent, sizeof sent);
        T_CHECK(sn == 5 + sizeof drv_ch_msg && sent[0] == 22);

        uint8_t rsh[128]; memcpy(rsh, drv_rec_sh, sizeof drv_rec_sh);
        T_CHECK(wo_tls_client_push_record(&c, rsh, sizeof drv_rec_sh) == WO_TLS_WANT_MORE);

        uint8_t rfl[1024]; memcpy(rfl, drv_rec_flight, sizeof drv_rec_flight);
        T_CHECK(wo_tls_client_push_record(&c, rfl, sizeof drv_rec_flight) == WO_TLS_ESTABLISHED);

        /* the client Finished record we emit matches RFC 8448 byte-for-byte */
        uint8_t fin[128];
        size_t fn = wo_tls_client_take_output(&c, fin, sizeof fin);
        T_CHECK(fn == sizeof drv_rec_cfin);
        T_CHECK(memcmp(fin, drv_rec_cfin, sizeof drv_rec_cfin) == 0);

        /* application encrypt: our first app record equals the recorded one */
        uint8_t app[128];
        int an = wo_tls_client_encrypt(&c, drv_capp_pt, sizeof drv_capp_pt, app, sizeof app);
        T_CHECK(an == (int)sizeof drv_rec_capp);
        T_CHECK(memcmp(app, drv_rec_capp, sizeof drv_rec_capp) == 0);

        /* server sends NewSessionTicket first (server app seq 0) — decrypt it
         * (a post-handshake handshake message), advancing the read seq. */
        uint8_t nst[256]; uint8_t ct2 = 0;
        memcpy(nst, drv_rec_nst, sizeof drv_rec_nst);
        int nn = wo_tls_client_decrypt(&c, nst, sizeof drv_rec_nst, nst, sizeof nst, &ct2);
        T_CHECK(nn > 0 && ct2 == 22);                  /* handshake (ticket) */

        /* then the server's application data (seq 1) decrypts to the plaintext */
        uint8_t sapp[128]; uint8_t ct3 = 0;
        int dn = wo_tls_client_decrypt(&c, drv_rec_sapp, sizeof drv_rec_sapp,
                                       sapp, sizeof sapp, &ct3);
        T_CHECK(dn == (int)sizeof drv_sapp_pt && ct3 == 23);
        T_CHECK(memcmp(sapp, drv_sapp_pt, sizeof drv_sapp_pt) == 0);
    }

    /* Driver rejects a tampered server flight (auth failure -> FAILED). */
    {
        static wo_tls_client c;
        uint8_t priv[32], ch[256];
        memcpy(priv, drv_client_priv, 32); memcpy(ch, drv_ch_msg, sizeof drv_ch_msg);
        wo_tls_client_start_with(&c, ch, sizeof drv_ch_msg, priv);
        uint8_t rsh[128]; memcpy(rsh, drv_rec_sh, sizeof drv_rec_sh);
        wo_tls_client_push_record(&c, rsh, sizeof drv_rec_sh);
        uint8_t rfl[1024]; memcpy(rfl, drv_rec_flight, sizeof drv_rec_flight);
        rfl[100] ^= 0x01;                              /* corrupt the ciphertext */
        T_CHECK(wo_tls_client_push_record(&c, rfl, sizeof drv_rec_flight) == WO_TLS_FAILED);
    }

    return t_report("test_tls");
}

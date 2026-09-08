/* test_tls.c — TLS 1.3 record layer (runtime-v2 9 phase F1). KAT against
 * python's AEAD as oracle (tls_record_vectors.h), plus seal/open round-trip,
 * a tamper-rejection, and the sequence-number nonce advancing. ASan/UBSan. */
#include <stdint.h>
#include <string.h>

#include "tls.h"
#include "t.h"
#include "tls_record_vectors.h"

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

    return t_report("test_tls");
}

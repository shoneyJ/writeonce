---
track: runtime-v2
iteration: "9"
status: in-progress
readiness: ready
review_pending: "forks auto-approved 2026-09-08 for autonomous execution — developer second review before this ships. Landed: A–E crypto, F1 record layer, F2 key schedule, F3a message layer, F3b offline handshake verification, F3c-core sans-io client driver, SAN/hostname (all KAT'd vs RFC 8448 / real certs). Remaining F3c-net: random ephemeral for production, system CA trust-anchor chain walk, net.connect_tls VM plumbing (live-gated). Then G inbound server"
---

# runtime-v2 9 — in-process TLS: retiring the proxy-termination doctrine

> Created 2026-09-07 from the gap [`jarvis`](../jarvis/00-story.md) surfaces — an
> assistant must dial an LLM over HTTPS, and the runtime has no outbound TLS. The
> developer chose the **full overturn**: the runtime gains TLS **both
> directions**, and the standing "TLS is the proxy's job" doctrine is retired.
> **Brainstormed to `ready` 2026-09-07**: hand-rolled TLS 1.3, RSA+ECDSA+X.509
> cert verification, decomposed into the bottom-up phase ladder below. The load-
> bearing implementation fork is settled — hand-roll, not vendor — with eyes open
> to the risk (Info).

## Why this exists — and what it overturns

Three documents record the same standing decision, and this story reverses it:

- *"TLS — permanently the proxy's job (framework doctrine)"* —
  [language 34](../language-runtime-database/34-crypto-builtins.md) (crypto
  builtins, line ~83).
- *"TLS — proxy-terminated, by doctrine, unchanged… the story says so out loud
  rather than implying HTTPS clients"* —
  [language 38](../language-runtime-database/38-content-platform-capabilities.md)
  (which adds `net.connect` as **plaintext** outbound TCP and explicitly refuses
  HTTPS).
- *"TLS, HTTP/2 | nobody — proxy-terminated by doctrine"* —
  [porch](../porch/00-story.md)'s "what this track does NOT own".

The doctrine was reasonable while nothing in-tree needed to *dial* anything: a
front proxy terminates inbound TLS, and there were no outbound callers. jarvis
breaks that — its whole job is to reach a remote API — and the developer's
direct-HTTPS choice for it means the runtime, not a companion, owns the
connection. Rather than carve out a one-directional exception, the decision is to
give the runtime TLS in **both** directions: outbound so a `.wo` program can dial
HTTPS, and inbound so porch can terminate TLS itself instead of mandating a
proxy in front of every deployment.

This is **not a builtin-sized seam** like the rest of this track. TLS 1.3 plus
X.509 certificate validation is a large, security-critical subsystem — the one
place the runtime's hand-roll-everything habit (the sha256 precedent) should not
be assumed to extend. That tension is the load-bearing fork below.

## Decisions locked (brainstorm 2026-09-07)

1. **Hand-roll TLS 1.3 in C — no vendored library.** The developer chose the
   hand-roll over vendoring mbedTLS/BearSSL, extending the runtime's
   hand-roll-everything habit (the sha256 precedent) to the hardest place it has
   reached. This keeps the pure single-static-binary, zero-external-dependency
   story intact — and it is, stated plainly, the largest and highest-risk
   undertaking in the project. See the risk note in Info; it is not a caveat to
   bury.
2. **TLS 1.3 only.** No 1.2 legacy — smallest attack surface, one handshake to
   get right.
3. **Cert verification is full: RSA + ECDSA + X.509.** To reach real endpoints
   (Anthropic, OpenAI and most HTTPS servers present RSA-signed chains), the
   verifier does RSA-PSS and RSA-PKCS#1v1.5 plus ECDSA-P256, over a real
   ASN.1/DER + X.509 chain validator with a system trust store, validity-date and
   hostname (SAN) checks. This is the biggest, most CVE-prone slice, and it is in
   scope because EC-only cannot talk to the APIs jarvis needs.
4. **Bottom-up, outbound-first.** Build the primitives before the protocol, and
   the client (jarvis's need) before the server (porch's), because the primitives
   are shared and only the role differs.

## The phase ladder

Each rung is a security-critical slice; C, D and E are each large enough that
they may split into their own runtime-v2 iterations as they are picked up.

| Phase | Delivers | Notes |
| --- | --- | --- |
| A — AEAD | AES-128/256-GCM (TLS 1.3 mandates AES-128-GCM) and ChaCha20-Poly1305 | **is runtime-v2 [8](08-symmetric-cipher.md)** — so 8 must include AES-GCM, not only ChaCha; this rung consumes it |
| B — key schedule | ✅ **LANDED 2026-09-08** — `wo_hkdf_sha256_extract`/`expand` (RFC 5869) + `expand_label` (RFC 8446 §7.1), internal C over `hmac_sha256`; SHA-256 (the mandatory suites' hash; SHA-384 a later add). KAT-gated in `test_crypto.c` (RFC 5869 case 1 + Expand-Label vectors), ASan/UBSan clean. No builtin, no compiler change |
| C — key exchange | ✅ **LANDED 2026-09-08** — `wo_x25519` (RFC 7748), constant-time Montgomery ladder + mask-based cswap, radix-2⁵¹ field arithmetic (curve25519-donna-c64, `__int128`). Internal C. KAT-gated in `test_crypto.c`: RFC 7748 §5.2 both direct vectors **and the 1000-iteration test**, ASan/UBSan clean |
| D — signatures | ✅ **LANDED 2026-09-08** — **RSA** `wo_rsa_pkcs1_sha256_verify` + `wo_rsa_pss_sha256_verify` (bignum Montgomery modexp) and **ECDSA-P256** `wo_ecdsa_p256_sha256_verify` (Jacobian point arithmetic, a=-3, on-curve check, Fermat inverses reusing the bignum). Verification is public data so **not** constant-time by design. Both match python vectors (RSA-2048 PKCS1+PSS; P-256), tamper/wrong-hash rejected, KAT-gated, ASan/UBSan clean |
| E — X.509 | 🔄 **CORE LANDED 2026-09-08** — a defensive ASN.1/DER reader (every length/bound checked, malformation is rejection not over-read) + certificate parse (tbsCertificate span, sig-alg OID, signature, SubjectPublicKeyInfo→RSA n/e or EC P-256 x/y, validity) + `wo_x509_verify_one` (one chain link's signature, dispatching to D's RSA-PKCS1/PSS + ECDSA-P256) + `wo_x509_parse_spki` + `wo_x509_check_validity` (caller supplies the time). KAT-gated in `test_crypto.c` against **real python-generated chains** — RSA CA+leaf (SHA256withRSA) and EC P-256 CA+leaf (ecdsa-with-SHA256): leaf-vs-CA, self-signed CA, wrong-issuer/tampered/truncated rejected, validity window, SPKI extraction — ASan/UBSan clean. **Deferred to F**: SAN/hostname match (needs the target host) and the multi-cert chain walk to a system CA bundle | notoriously bug-prone; consumes D |
| F — record + handshake (client) | 🔄 **F1–F3b LANDED 2026-09-08** — new `tls.c`/`tls.h`. **F1 record layer** (`wo_tls_record_seal`/`open`, RFC 8446 §5.2, per-record nonce = iv XOR seq, both suites) KAT'd byte-for-byte vs python. **F2 key schedule** (`wo_tls_derive_handshake`/`_application`/`_traffic_keys`/`_finished_verify`, §7.1) KAT'd byte-for-byte vs **RFC 8448 §3**. **F3a message layer** (`wo_tls_parse_server_hello` — attacker input, bounded, rejects HRR/bad suite/truncation; `wo_tls_build_client_hello` — SNI, x25519, sig-algs) KAT'd vs RFC 8448 SH + validated by an independent parser. **F3b offline handshake verification** (`wo_tls_verify_cert_verify` over phase E+D; server + client Finished) — the whole handshake **crypto** proven end-to-end offline vs RFC 8448. **F3c-core sans-io driver** (`wo_tls_client` — pure FSM, caller frames records: CH→SH→flight→Finished, message reassembly, per-message transcript timing, constant-time Finished, application encrypt/decrypt) KAT'd against the **full RFC 8448 record trace** — client Finished + first app record byte-for-byte, NewSessionTicket + server app data decrypt, tampered flight refused. **SAN/hostname** (`wo_x509_check_host`, RFC 6125) + driver enforcement landed. **Remaining F3c-net**: random ephemeral for production start, the multi-cert chain walk to a **system CA trust anchor**, and the `net.connect_tls` builtin + `net.read_tls`/`net.write_tls` VM plumbing (record framing over a real fd), gated live against `openssl s_server` | jarvis's path; the reason the story exists |
| G — server (inbound) | the server handshake half, cert+key loading, signing CertificateVerify; porch terminates TLS | retires the inbound proxy requirement, and the doctrine docs |

## F3c-net — the remaining slice (decisions auto-approved 2026-09-08, review pending)

Everything security-critical is landed and offline-KAT'd. What is left is I/O
integration that can only be gated **live** (against a local `openssl s_server`
/ python TLS server), so it is a single cohesive slice, not further split:

1. **Random ephemeral.** A `getrandom(2)`-backed source for the per-connection
   X25519 private key (and the ClientHello random / session id). No `.wo`
   randomness builtin is assumed; this is internal to the connect path.
2. **CA-bundle loader.** Parse the system PEM bundle
   (`/etc/ssl/certs/ca-certificates.crt`, confirmed present on the dev box) into
   DER trust anchors for `wo_tls_verify_chain`. Built **with** its consumer, not
   ahead of it (its memory model is the connect path's to own).
3. **`net.connect_tls(host, port)` builtin.** TCP-connects (reusing the
   `net.connect` path), generates the ephemeral, runs the sans-io driver —
   framing records off the socket (read the 5-byte header, then the body) and
   flushing `take_output` — until ESTABLISHED, then validates the chain
   (`wo_tls_verify_chain` with the loaded anchors + the host). Plus
   `net.read_tls` / `net.write_tls` for application data.
   - **Handle representation (default, auto-approved):** mirror `net.connect` —
     the builtin returns the **TCP fd as an Int**, and the runtime keeps the
     `wo_tls_client` state in a side table keyed by fd; `net.read_tls` /
     `net.write_tls` / `net.close` look it up and free it on close. This is the
     smallest change to the language surface (no new class) and matches the
     existing fd-based net verbs. The alternative — a first-class `TlsConn`
     language object — is heavier and deferred unless the developer prefers it.
   - **Blocking model (default, auto-approved):** the handshake and app I/O
     block, exactly as today's `net.connect` does; the park-plane async refit is
     a later refinement, not a v1 requirement.
   - VM wiring: new `WO_B_NET_CONNECT_TLS` / `_READ_TLS` / `_WRITE_TLS` ids in
     `wob.h`, `emit.ml` / `types.ml` registration, `loader.c` arities,
     `builtin.c` dispatch, `sysio.c` implementation.
4. **Live gate.** A `just` recipe dialing a local TLS server: full handshake,
   chain+host validation, a request/response round-trip, and the negative cases
   (wrong host, untrusted chain, expired cert) each refused.

## Consumers

Named, so this is not a capability shipped as decoration:

- **[jarvis 1](../jarvis/00-story.md)** — outbound HTTPS to the LLM API (the
  reason this story exists).
- **porch** — inbound TLS termination, retiring the mandatory front proxy for a
  single-binary deployment.
- **language 38** — the outbound HTTPS half it excluded by doctrine; this story
  is where that exclusion is lifted.

## Dependencies

- **runtime-v2 [8](08-symmetric-cipher.md)** — the AEAD (phase A). This story
  forces 8 to include **AES-GCM** (TLS 1.3 mandates AES-128-GCM), not ChaCha
  alone — a consequence to record in 8's own fork.
- **language [34](../language-runtime-database/34-crypto-builtins.md)** —
  SHA-256/HMAC for the key schedule (phase B) and the transcript hash.
- **`net.connect`** (id 110, **landed 2026-09-07**) — the outbound TCP socket the
  client handshake runs over; the client half sits directly on it.

## Out of scope

- **HTTP/2.** A separate protocol concern, parked behind language iteration 23
  regardless; TLS is its prerequisite, not its owner.
- **Mutual TLS / client certificates.** A later slice if a consumer asks; the
  first cut authenticates the server, not the client.
- **Updating the doctrine documents.** Retiring "TLS is the proxy's job" means
  correcting [language 34](../language-runtime-database/34-crypto-builtins.md),
  [language 38](../language-runtime-database/38-content-platform-capabilities.md)
  and [porch](../porch/00-story.md) when this lands — a follow-up bookkeeping
  pass, named here so it is not forgotten, not part of the runtime work.

## Risk and test strategy

**This is the highest-risk work in the project, and hand-rolling it raises that
risk, not lowers it.** Hand-rolled RSA, ECDSA, X25519 and ASN.1/X.509 are the
classic sources of real-world CVEs (timing side-channels, padding oracles, chain-
validation bypasses, parser memory bugs). The decision to hand-roll is recorded
and owned; the mitigations are non-negotiable:

- **Constant-time** for every secret-dependent operation (X25519, RSA/ECDSA,
  AEAD) — verified, not assumed.
- **Reference-tested**: every phase gated against a reference implementation —
  `openssl s_client`/`s_server`, real published cert chains, and the RFC 8448
  TLS 1.3 test vectors — plus an ASan/UBSan leg on the parser and bignum code.
- **Negative tests as first-class**: an expired cert, a wrong hostname, a broken
  chain, a tampered CertificateVerify and a downgrade attempt must each be
  refused, with a test that fails if they are accepted.
- **No partial-trust states**: a validation that cannot complete refuses the
  connection; there is no "warn and continue".

## Info

This is the heaviest iteration in the runtime-v2 track by a wide margin — a
subsystem, not a builtin-sized seam — and the only one that reverses a project
doctrine. It is pure I/O-plane and compute work (a handshake layer over the
existing socket verbs plus the crypto ladder); no actors, so it is not exposed to
the lang-41 hang. It gates jarvis entirely: until at least phases A–F land,
jarvis cannot reach a model at all. Realistically it is a multi-phase effort
measured in weeks, and phases C (X25519), D (signatures/RSA) and E (X.509) may
each become their own iteration when picked up. Implementation order is the
ladder, bottom-up: A (via rv2 8) → B → C → D → E → F, with G (inbound server)
last.

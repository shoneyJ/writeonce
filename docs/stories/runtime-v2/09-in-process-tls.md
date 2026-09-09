---
track: runtime-v2
iteration: "9"
status: in-progress
readiness: ready
review_pending: "forks auto-approved 2026-09-08/09 for autonomous execution — developer second review before this ships. OUTBOUND CLIENT COMPLETE + live-gated (just tls, 5/0): A–E crypto, F1 record, F2 key schedule, F3a messages, F3b offline verify, F3c-core sans-io driver, SAN/hostname, F3c-net chain validation + basicConstraints/EKU, and the net.connect_tls/read_tls/write_tls builtins (ids 115-117). Six integration forks implemented as locked. REMAINING: G inbound server (porch); deferred park-based handshake + TlsConn object + connection pooling; correcting the doctrine docs (34/38/porch)"
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
| F — record + handshake (client) | ✅ **COMPLETE 2026-09-08/09** (client). F1–F3b LANDED 2026-09-08 — new `tls.c`/`tls.h`. **F1 record layer** (`wo_tls_record_seal`/`open`, RFC 8446 §5.2, per-record nonce = iv XOR seq, both suites) KAT'd byte-for-byte vs python. **F2 key schedule** (`wo_tls_derive_handshake`/`_application`/`_traffic_keys`/`_finished_verify`, §7.1) KAT'd byte-for-byte vs **RFC 8448 §3**. **F3a message layer** (`wo_tls_parse_server_hello` — attacker input, bounded, rejects HRR/bad suite/truncation; `wo_tls_build_client_hello` — SNI, x25519, sig-algs) KAT'd vs RFC 8448 SH + validated by an independent parser. **F3b offline handshake verification** (`wo_tls_verify_cert_verify` over phase E+D; server + client Finished) — the whole handshake **crypto** proven end-to-end offline vs RFC 8448. **F3c-core sans-io driver** (`wo_tls_client` — pure FSM, caller frames records: CH→SH→flight→Finished, message reassembly, per-message transcript timing, constant-time Finished, application encrypt/decrypt) KAT'd against the **full RFC 8448 record trace** — client Finished + first app record byte-for-byte, NewSessionTicket + server app data decrypt, tampered flight refused. **SAN/hostname** (`wo_x509_check_host`, RFC 6125) + driver enforcement landed. **F3c-net chain validation** (`wo_tls_verify_chain`) + **basicConstraints/EKU** hardening KAT'd offline. **F3c-net socket/VM ✅ LANDED 2026-09-09**: `getrandom` ephemeral, per-shard lazy CA-bundle loader (`WO_CA_BUNDLE`), and the `net.connect_tls` / `net.read_tls` / `net.write_tls` builtins (ids 115–117; blocking deadline-bounded connect+handshake then a parked data plane; per-shard fd-keyed slot table, no locks). **Live-gated** (`just tls`, 5/0) from `.wo` against a local TLS 1.3 stub incl. untrusted-chain + hostname-mismatch negatives. Client side complete | jarvis's path; the reason the story exists |
| G — server (inbound) | the server handshake half, cert+key loading, signing CertificateVerify; porch terminates TLS | retires the inbound proxy requirement, and the doctrine docs |

## F3c-net — the socket/VM slice (✅ **LANDED 2026-09-09**; decisions locked, forks auto-approved, `review_pending`)

> **Landed and live-gated.** `net.connect_tls` / `net.read_tls` / `net.write_tls`
> (ids 115–117) are wired into the VM and dial a real TLS 1.3 server end to end
> from `.wo`: the hand-rolled handshake, the chain + hostname + basicConstraints/
> EKU validation against the system (or `WO_CA_BUNDLE`) trust store, and an
> application round-trip. The `just tls` gate (`scripts/tls-accept.sh`,
> `docs/examples/tls-client`) proves the happy path against a local TLS 1.3 stub
> and refuses the untrusted-chain and hostname-mismatch negatives — 5 checks, 0
> failures, no live network. All six decisions below were implemented as locked.
> **This completes the outbound client; jarvis is unblocked.**

Everything security-critical is landed and offline-KAT'd. What is left is I/O
integration that can only be gated **live** (a local `openssl s_server` / python
TLS server), so it is one cohesive slice, not further split. The integration
forks are settled below — the first four grounded in the existing runtime, the
last two added 2026-09-09 from a comparison against **gofiber v3's client**
(fasthttp + Go `crypto/tls`/`crypto/x509`, in `.dev/reference/fiber`), which
bounds every request with a timeout and delegates full chain checks to
`crypto/x509`. This section is `ready`: the decisions are locked, the acceptance
criteria are stated, and code may start once a developer signs off the
`review_pending` marker.

### The locked decisions

1. **Blocking connect + blocking handshake, then park the data plane.** This
   mirrors `net.connect` exactly (`sysio.c` `WO_B_NET_CONNECT`): the socket is
   **blocking** through TCP connect and the whole TLS handshake, then switched to
   `O_NONBLOCK` once ESTABLISHED. `net.connect`'s own comment already accepts a
   blocking connect ("can stall the shard during the handshake, tolerable while
   connect is rare"); a TLS connection is likewise rare and long-lived (jarvis
   streams a whole conversation over one), so the extra few handshake round-trips
   are the same tolerable stall. Application I/O then **parks the fiber** exactly
   like `net.read`/`net.write` (`O_NONBLOCK` + `park_fd` on POLLIN/POLLOUT +
   retry). A **park-based handshake** is a named follow-up — the same deferral
   `net.connect` made for its `_dl`/park variant, not a v1 requirement.
2. **Per-shard fd-keyed slot table, no locks.** TLS connection state lives in a
   `wo_tls_conn` slot array **in the shard's own vm**, keyed by fd — the exact
   pattern of `wo_child children[WO_PROC_MAX]` (`vm.h`: "live in the owning
   shard's vm — no locks, one thread"). One pinned OS thread per shard and fds
   that never migrate cross-shard make this thread-safe by construction, with no
   new locking. Each slot holds the `wo_tls_client` (keys, seqs, driver state), a
   **partial-record read buffer** (a record may arrive in fragments over a
   non-blocking socket), and a **leftover-plaintext buffer** (a decrypted record
   larger than the caller's `max`). Capped like `WO_PROC_MAX`.
3. **Failures trap `WO_T_IO`, loudly.** Every failure of a secure connect —
   DNS, TCP connect, the handshake, and critically the **certificate chain and
   hostname** checks (and any later record auth failure) — returns a `WO_T_IO`
   trap with a descriptive message, mirroring `net.connect`. A secure-connection
   failure is never a silent `nil`; this is the "refuse loudly / no partial
   trust" rule made concrete. `net.read_tls`/`net.write_tls` otherwise mirror
   `net.read`/`net.write` (EOF is the empty Bytes; a partial write resumes via a
   `park_wr_at`-style cursor; a decrypt/auth failure traps).
4. **Per-shard, lazy, read-only CA bundle.** On the first `net.connect_tls` a
   shard loads the system PEM bundle into its own vm (read-only thereafter) and
   reuses it for every later dial — no cross-shard sharing, no locks, consistent
   with (2). Path: `/etc/ssl/certs/ca-certificates.crt` (confirmed present on the
   dev box), overridable by the `WO_CA_BUNDLE` environment variable — which is
   also how the live gate points the client at its self-signed test CA.
5. **A bounded handshake deadline (no unbounded shard stall).** The blocking
   model of decision (1) would otherwise let a slow or hostile server stall the
   shard's one thread indefinitely during connect + handshake — the DoS that
   gofiber closes with `DoTimeout`. So `net.connect_tls` bounds the whole
   connect+handshake by a deadline: **non-blocking `connect()` + `poll` for the
   TCP step, and `SO_RCVTIMEO`/`SO_SNDTIMEO` on the blocking socket across the
   handshake**, capping the stall without needing the full park refit. Default
   from `WO_TLS_HANDSHAKE_MS` (10 000 ms if unset); expiry aborts and traps
   `WO_T_IO` ("tls: handshake timeout"). A per-call `_dl` variant and the
   park-based handshake remain the named follow-ups.
6. **Chain hardening: basicConstraints + EKU (not just signatures).** Signature
   + validity + SAN is not enough — Go's `crypto/x509` also enforces the
   constraints that stop a leaf from masquerading as a CA. So the phase-E
   extension walk and `wo_tls_verify_chain` gain: every **non-leaf** cert must
   assert `basicConstraints` CA:TRUE and satisfy `pathLenConstraint`, and the
   **leaf** must carry Extended Key Usage `id-kp-serverAuth` (or omit EKU
   entirely). A `keyUsage` `keyCertSign` check on issuers is included where
   present. Failure is a rejection like any other chain fault (no partial trust).

### The builtin surface

Three new builtins on the `net` module (one numeric id space; `WO_B_MAX` moves
114 → 117):

- `net.connect_tls(host, port) -> Int` — id **115**, arity 2. Blocking TCP
  connect (reusing the `net.connect` DNS/connect path), `getrandom(2)` ephemeral
  X25519 key + ClientHello random/session-id, run the sans-io driver over the
  blocking socket (frame each record: read the 5-byte header, then the body;
  flush `take_output`) to ESTABLISHED, set the host on the driver so the leaf
  SAN is enforced, then `wo_tls_verify_chain` against the lazily-loaded anchors
  (with the decision-6 basicConstraints/EKU checks). The whole connect+handshake
  is bounded by the decision-5 deadline. Returns the fd (a slot is claimed for
  it); traps on any failure.
- `net.read_tls(fd, max) -> Bytes` — id **116**, arity 2. Reads/decrypts one
  application record via the slot, returning up to `max` plaintext bytes (EOF is
  the empty Bytes), buffering a partial record and parking on POLLIN, and
  draining any leftover plaintext first.
- `net.write_tls(fd, bytes) -> Int` — id **117**, arity 2. Seals `bytes` into an
  application record and writes it, parking on POLLOUT for a partial write.

`net.close` (existing) additionally frees any `wo_tls_conn` slot for the fd.
VM wiring touches `wob.h` (ids + `WO_B_MAX`), `emit.ml`/`types.ml`
(registration + return types), `loader.c` (arities), `builtin.c` (sysio dispatch
range), and `sysio.c` (the implementations + the slot/bundle helpers). No `.wob`
consumer change beyond the id additions.

### Acceptance criteria

- **Given** a reachable TLS 1.3 server with a chain to a trusted anchor, **when**
  a `.wo` program calls `net.connect_tls` for its hostname, **then** the
  handshake completes, the chain + hostname validate, and an fd is returned.
- **Given** that fd, **when** the program `net.write_tls`es a request and
  `net.read_tls`es, **then** it exchanges application data, and `net.close`
  frees the socket and the slot.
- **Given** a server whose certificate does not chain to a trusted anchor, whose
  SAN does not match the host, or is expired, **when** `net.connect_tls` runs,
  **then** it traps `WO_T_IO` — no connection is returned.
- **Given** a server that accepts the TCP connection but then stalls (never
  finishing the handshake), **when** the decision-5 deadline elapses,
  **then** `net.connect_tls` aborts and traps `WO_T_IO` rather than stalling the
  shard indefinitely — proven with a stub that connects then sleeps.
- **Given** a chain whose issuer lacks `basicConstraints` CA:TRUE (a leaf used
  to sign another cert), or a leaf lacking EKU `serverAuth`, **when**
  `net.connect_tls` validates it, **then** it is rejected — with negative KATs
  in `test_tls` alongside the existing chain cases.
- **Given** two shards each dialing TLS, **when** they run concurrently, **then**
  neither reads the other's slot or bundle (per-shard, no locks), proven under
  ASan/TSan.
- **Given** the live gate, **when** it runs, **then** it dials a local TLS
  server (trusting a test CA via `WO_CA_BUNDLE`), does a request/response
  round-trip, and refuses each negative (wrong host, untrusted chain, expired).

### Out of scope (named, deferred)

- **A park-based handshake** — the async refit of decision (1); a first-class
  `TlsConn` language object over the fd — both later, only if measured need or
  the developer prefers them.
- **The HTTP layer.** `net.connect_tls` is a TLS byte pipe; HTTP/1.1 framing
  over it is the caller's (jarvis 1's `.wo`), not this slice's.
- **Inbound TLS (server).** Phase **G**, a separate slice for porch.

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

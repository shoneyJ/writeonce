---
track: runtime-v2
iteration: "8"
status: done
readiness: ready
review_pending: "fork auto-approved 2026-09-09 for autonomous execution — developer second review: phase D (the encrypted-cookie wrapper) re-homed to the porch track as the consumer's phase (pure .wo on porch 2's cookie machinery + random_bytes, which do not exist yet), the same split rv2 7 makes for its /metrics phase; the runtime primitive closes here with phase E"
---

# runtime-v2 8 — AEAD ciphers: authenticated encryption for cookies, data at rest, and TLS

> Created 2026-09-06 from the porch-vs-fiber scope-gap analysis
> ([exploration](../../plan/exploration/fiber/01-porch-vs-fiber-scope-gap.md)) as
> a runtime-v2 iteration — a hand-rolled cipher builtin with a `types.ml` row is
> exactly the track's builtin-sized-seam shape (the iteration 42 precedent). (It
> is not a language-track iteration; the language number 43 was already spent on
> the wmux foundation.)
>
> **Brainstormed to `ready` 2026-09-08.** A second consumer appeared after the
> first draft — runtime-v2 [9](09-in-process-tls.md)'s TLS phase A — and it
> reshaped the forks: TLS 1.3 **mandates AES-128-GCM** (RFC 8446), so the
> original ChaCha-only lean is out, and TLS constructs its own per-record nonce,
> so the primitive takes a **caller-supplied** nonce.

## Why this exists

The runtime has digests only — SHA-1, SHA-256, HMAC-SHA256, base64
([iteration 34](../language-runtime-database/34-crypto-builtins.md)) — and, from the porch track,
`random_bytes` ([porch 2](../porch/02-randomness-and-cookies.md)). Those let a
program *authenticate* and *sign* a value, and *mint* a random one. None of them
let it *encrypt* — turn a plaintext into a ciphertext only the key-holder can
read.

That absence is a named porch limit:
[porch 2](../porch/02-randomness-and-cookies.md) scopes out **encrypted
cookies** explicitly — "Fiber's `encryptcookie` needs a symmetric cipher, and
the runtime has digests only. Signed-and-readable is honest and sufficient for a
session id; encrypting a payload is a separate ask with a separate primitive
behind it." This is that separate primitive.

Signed-and-readable (what porch has) is correct for a session id — the client
may see it, it just may not forge it. Encryption is for the case where the
*payload itself* must be hidden from the client: an encrypted cookie carrying
app state, or a database field encrypted at rest.

## Decisions locked (brainstorm 2026-09-08)

1. **Two AEAD ciphers: AES-GCM (128 and 256) and ChaCha20-Poly1305.** TLS 1.3
   mandates AES-128-GCM, so it is in whatever happens; ChaCha20-Poly1305
   (RFC 8439) rides along because it is a valid TLS 1.3 suite, is far easier to
   get constant-time, is preferred where no AES hardware exists, and is the clean
   default for cookies and data-at-rest. TLS negotiates whichever the server
   picks; application code defaults to ChaCha.
2. **AES is made constant-time by hardware, with a software fallback.** AES-NI on
   x86-64 (`<wmmintrin.h>`) and the ARMv8 crypto extension give constant-time AES
   and GHASH (CLMUL/PMULL) with zero external dependency — these are compiler
   intrinsics, not a library. A bitsliced constant-time software AES + a
   constant-time GHASH covers CPUs without the extension. ChaCha20-Poly1305 is
   naturally constant-time in portable C and needs no hardware path.
3. **The primitive takes a caller-supplied nonce.** Shape:
   `<cipher>_seal(key, nonce, aad, plaintext) -> Bytes` (ciphertext‖tag) and
   `<cipher>_open(key, nonce, aad, ciphertext) -> ?Bytes` (`nil` on any
   authentication failure). Caller-supplied because TLS builds its own per-record
   nonce (static IV XOR sequence number); the random-nonce convenience for
   cookies is a **wrapper** on top (phase D), not the primitive. Named per cipher
   (matching the existing `sha1`/`sha256`/`hmac_sha256` style), AES variant
   inferred from key length (16 → AES-128, 32 → AES-256). New builtin ids start
   at 111 (after `net.connect` = 110); confirm against `wob.h` at implementation.
4. **Raw key with a length check.** A raw key (16 or 32 bytes, from
   `random_bytes`, carried as base64 in config — the `encryptcookie.GenerateKey`
   shape), length-validated. A passphrase-plus-KDF is a separate ask.
5. **Hand-rolled, no vendored library** — consistent with rv2 9's decision and
   the SHA-256 precedent. AEAD, never a bare cipher: unauthenticated encryption
   is a footgun that will not ship.

## Phases

Easy cipher first, so a working AEAD exists before the hard constant-time AES
work; TLS's ChaCha suite and the cookie consumer unblock at phase A.

| Phase | Delivers |
| --- | --- |
| A — ChaCha20-Poly1305 | ✅ **LANDED 2026-09-08** — `chacha20poly1305_seal`/`open` (ids 111/112, bare-name crypto family). Hand-rolled ChaCha20 + poly1305-donna-32 + the RFC 8439 §2.8 AEAD, caller-supplied 12-byte nonce, 32-byte key, constant-time tag compare, `open` returns nil on auth failure. Matches the RFC 8439 §2.8.2 vector byte-for-byte; gated in `test/test_crypto.c` (§2.5.2 Poly1305 + §2.8.2 seal/open/tamper), ASan/UBSan clean |
| B — AES-GCM via hardware | ✅ **LANDED 2026-09-08** (x86-64) — `aes_gcm_seal`/`open` (ids 113/114), AES-128/256 (by key length) on AES-NI + PCLMULQDQ, constant-time by hardware, CPUID-gated with target-attributed functions so the binary stays portable (no-AES-NI traps until phase C). Matches NIST SP 800-38D cases 4 & 16 byte-for-byte; KAT-gated in `test_crypto.c`, ASan/UBSan clean. **ARMv8 crypto-ext path deferred** (untestable on the x86-64 dev host) — folds into phase C |
| C — AES-GCM portability | ✅ **software fallback LANDED 2026-09-08** — portable constant-time AES (S-box via the GF(2⁸)-inverse power ladder, no tables) + bit-by-bit constant-time GHASH; same `aes_gcm_seal`/`open`, dispatched to AES-NI when present else this path. Matches NIST cases 4 & 16 byte-for-byte (test forces the software path via `wo_aes_force_software`), ASan/UBSan clean. AES-GCM is now available on any CPU (the phase-B no-AES-NI trap is retired). **ARMv8 crypto-ext hardware path still deferred** (untestable on the x86-64 dev host) — a follow-up when an ARM host exists |
| D — the cookie wrapper | **re-homed to porch 2026-09-09** (`review_pending`) — an `encryptcookie`-equivalent is pure `.wo` on porch [2](../porch/02-randomness-and-cookies.md)'s cookie machinery: random nonce (from `random_bytes`) prepended to the ciphertext, default ChaCha. It is the **consumer's** phase (as rv2 7's `/metrics` is porch's) and cannot land before porch 2 ships `random_bytes` + `SetCookie`; porch 2 names it as its follow-on. The primitives it wraps are complete here |
| E — the gate | ✅ **LANDED 2026-09-09** — RFC 8439 + NIST GCM known-answer vectors on both AES paths (`test_crypto`), ASan/UBSan clean, and the reference cross-check is **real-protocol interop, not `openssl enc`** (which cannot do AEAD modes): `just tls-server` pins `openssl s_client` to `TLS_CHACHA20_POLY1305_SHA256`, to `TLS_AES_128_GCM_SHA256`, and to openssl's default list (server must skip the unimplemented AES-256 suite) — 5/0; `just tls` reports the suite the Python/OpenSSL peer negotiated (ChaCha under its server-preference default) — 5/0 |

## Consumers

- **runtime-v2 [9](09-in-process-tls.md), TLS phase A** — the record-layer AEAD;
  needs AES-GCM (mandatory) and gets ChaCha too. This is why AES-GCM is in scope.
- **porch encrypted cookies** — the original consumer (phase D), the
  `encryptcookie`-equivalent porch [2](../porch/02-randomness-and-cookies.md)
  scoped out.
- **database field-at-rest** — a plausible third, unbuilt until a workload asks.

## Dependencies

- **porch [2](../porch/02-randomness-and-cookies.md)** — `random_bytes`, for the
  cookie wrapper's random nonce (phase D only). The AEAD primitives themselves
  are self-contained.
- **AES-NI / ARMv8 crypto** — compiler intrinsics, not an external dependency.

## Out of scope

- **Asymmetric crypto** (RSA, ECDH, signatures). A different, much larger
  surface — owned by rv2 [9](09-in-process-tls.md)'s TLS phases C/D, not here.
- **Key rotation, a KMS, envelope encryption.** Operational key management is its
  own story if a consumer appears.
- **A passphrase KDF** (fork 4) — raw keys only; a KDF is a separate ask.
- **Nonce-misuse-resistant modes** (AES-GCM-SIV). The caller-supplied-nonce
  contract stands; misuse resistance is a later slice if a consumer needs it.
- **Compression before encryption** (the CRIME/BREACH interaction). A caller
  concern to document, not a primitive.

> The earlier draft listed "TLS — proxy-terminated by doctrine" here. That
> doctrine was **retired** by rv2 [9](09-in-process-tls.md); TLS is now a
> first-class consumer of this cipher, which is what pulled AES-GCM into scope.

## Risk and test strategy

Encryption code is get-it-exactly-right code, and the risk is timing side
channels and a reused nonce:

- **Constant-time is mandatory** for every key/plaintext-dependent operation —
  hardware AES/GHASH by construction, and the bitsliced fallback and ChaCha/
  Poly1305 verified table-free and branch-free. The Poly1305/GHASH tag compare is
  constant-time (`ct_eq`-style).
- **Known-answer vectors gate every cipher**: RFC 8439 for ChaCha20-Poly1305,
  the NIST GCM test vectors for AES-GCM, on both the hardware and software paths;
  plus a reference cross-check and an ASan/UBSan leg.
- **The reused-nonce hazard is documented loudly.** A key+nonce pair must never
  repeat; the cookie wrapper (phase D) draws a fresh random nonce per seal, and
  TLS owns its own per-record nonce discipline — the primitive trusts the caller
  and says so.

## Info

Two named consumers now — TLS (rv2 9 phase A, the reason AES-GCM is in) and porch
encrypted cookies — with database-field-at-rest a plausible third. Pure compute,
no actors, not exposed to the lang-41 hang. It is the **first rung of the TLS
ladder**, so it gates rv2 9: nothing above TLS phase A can be built until this
lands. Implementation order was A (ChaCha, unblocks the most for the least risk) →
B (hardware AES-GCM) → C (software AES fallback) → E (gate); D (cookie wrapper)
is porch's, after porch 2. **Done 2026-09-09** as a runtime primitive.

---
track: runtime-v2
iteration: "8"
status: pending
readiness: refine
---

# runtime-v2 8 — a symmetric cipher: authenticated encryption for cookies and data at rest

> Created 2026-09-06 from the porch-vs-fiber scope-gap analysis
> ([exploration](../../plan/exploration/fiber/01-porch-vs-fiber-scope-gap.md)) as
> a runtime-v2 iteration — a hand-rolled cipher builtin with a `types.ml` row is
> exactly the track's builtin-sized-seam shape (the iteration 42 precedent). (It
> is not a language-track iteration; the language number 43 was already spent on
> the wmux foundation.) **`readiness: refine`** — the
> gap, its consumer and its forks are named; not brainstormed to `ready`.

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

## What it should deliver (scope to be refined)

- **An AEAD primitive** — authenticated encryption with associated data — as one
  or two builtins in the crypto family beside `hmac_sha256`: encrypt (key,
  nonce, associated-data, plaintext) → ciphertext+tag, and decrypt returning the
  plaintext or nil on any authentication failure. AEAD, not a bare cipher,
  because unauthenticated encryption is a footgun that ships.
- **The porch consumer**: an `encryptcookie`-equivalent — a cookie whose value is
  encrypted, not merely signed — layered on iteration 2's cookie machinery.

## Forks the brainstorm must settle

1. **Which cipher? This is the load-bearing fork.** AES-256-GCM is what browsers,
   fiber and every peer expect — but constant-time AES in pure software (no
   AES-NI intrinsics) is genuinely hard to get right. ChaCha20-Poly1305
   (RFC 8439) is modern, is far easier to implement constant-time in portable C,
   and is what a from-scratch no-dependency runtime should probably prefer — at
   the cost of being less "expected." The runtime hand-rolls its crypto (the
   SHA-256 precedent, no external dependency), which weighs toward ChaCha.
2. **Nonce management.** A reused nonce is catastrophic for both GCM and ChaCha.
   Caller-supplied nonces put that footgun in every app; a builtin-generated
   random nonce (drawing on iteration 2's `random_bytes`, prepended to the
   ciphertext, as fiber's `NewGCMWithRandomNonce` does) removes it. Leaning
   builtin-generated — so this iteration is ordered after porch 2's builtin.
3. **Key handling.** A raw 32-byte key (from `random_bytes`, carried as base64 in
   config, the `encryptcookie.GenerateKey` shape) with a length check, versus a
   passphrase-plus-KDF. Leaning raw key with validation; a KDF is its own ask.
4. **Hand-roll versus vendor.** Doctrine is no external dependencies. A
   hand-rolled ChaCha20-Poly1305 in C is bounded and well-specified; hand-rolled
   AES-GCM is more error-prone. This fork is the practical face of fork 1.

## Out of scope

- **Asymmetric crypto** (RSA, ECDH, signatures beyond HMAC). A different, much
  larger surface with no current consumer.
- **Key rotation, a KMS, envelope encryption.** Operational key management is its
  own story if a consumer appears.
- **TLS.** Proxy-terminated by doctrine; this cipher is for application payloads,
  not the transport.
- **Compression before encryption** (the CRIME/BREACH interaction). A caller
  concern to document, not a primitive.

## Info

One named consumer today (encrypted cookies), with database-field-at-rest as a
plausible second — enough to not be decoration, not so much as to over-build.
Depends on iteration 2's `random_bytes` (for the nonce, fork 2) and extends
iteration 34's crypto builtins. Pure compute — no actors, not exposed to the
lang-41 hang.

---
iteration: "34"
status: refine
---

# Iteration 34 — crypto builtins: digests and HMAC in the runtime

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).
>
> **Inserted 2026-08-22** — the framework ledger's oldest unowned gap
> gets an owner. PREMISE UPDATE (same day, post-merge): iteration 36
> landed bitwise `& | ^ << >>` + hex literals, so digests ARE now
> expressible in pure `.wo` — the original "no bitwise" impossibility
> is gone. The fork is now a real choice for this story's brainstorm:
> hand-rolled C builtins (bounded, fast, libc-only doctrine permits) vs
> pure-`.wo` (no runtime surface growth; interpreter-speed hashing).
> Off the concurrency chain but **gates chain position 4**: iteration
> 24's WebSocket handshake needs SHA-1 before chat can land.

## Why this iteration exists

Four consumers already wait on it, none able to proceed:
[iteration 24](24-chat-websocket-workload.md)'s upgrade handshake
(`Sec-WebSocket-Accept` = base64(SHA-1(key + GUID)) — SHA-1
specifically, not a choice); the framework's ETag/conditional-request
row (wants a content hash); HMAC-signed tokens the auth core can grow;
and held [iteration 21](21-keypair-attach-auth.md), whose
challenge–response needs primitives that "do not exist" (its demotion
note). Bytes and base64 landed with iteration 19 — the carriers exist,
only the digests are missing.

## Goals

- **Digest builtins over Bytes**: SHA-1 (the WS handshake's hard
  requirement), SHA-256 (the modern default for ETag/HMAC), each
  `Bytes -> Bytes`, streaming not required (whole-value, like every
  existing builtin).
- **HMAC-SHA256** (`key: Bytes, msg: Bytes -> Bytes`) — the one
  composition real services need (signed tokens, webhook signatures);
  expressible in `.wo` since iteration 36's bitwise set — C-builtin vs
  pure-`.wo` is this story's brainstorm call.
- **Test vectors are the acceptance**: FIPS 180 / RFC 2202 / RFC 4231
  vectors in a corpus fixture — a digest that "looks right" is worth
  nothing.
- **The contract doc row**: names, arities, Bytes-in/Bytes-out, and the
  explicit note that SHA-1 exists for protocol compatibility (WS), not
  for new designs.

## Acceptance Criteria (draft — the spec refines)

- **Given** the published test vectors for SHA-1, SHA-256, and
  HMAC-SHA256, **when** the corpus fixture runs them through the
  builtins, **then** every output matches byte-for-byte (via the
  existing base64/Bytes surface).
- **Given** the WS handshake's worked example from RFC 6455
  (`dGhlIHNhbXBsZSBub25jZQ==` → `s3pPLMBiTxaQ9kYGzzhZRbK+xOo=`),
  **when** composed in pure `.wo` from `sha1` + `base64_encode`,
  **then** the exact accept token comes out — iteration 24's handshake
  is provably one expression away.
- **Given** the full battery, **when** it runs, **then** nothing
  regresses — new builtin ids only, no opcode, no `.wob` version bump
  (the iteration-19/`time.ticks` precedent).

## Out Of Scope

- Asymmetric crypto (ed25519 signatures/keypairs) — held iteration 21's
  spec decides what it needs when it unholds; this iteration lays the
  digest floor it will stand on.
- TLS — permanently the proxy's job (framework doctrine).
- CRC32 — the ledger lists it, but no consumer is blocked on it; it
  joins only if 24's spec finds a real need (rejecting speculative
  surface).
- A password-hashing story (bcrypt/argon2) — no workload asks yet.
- Bitwise operators in the language — a separate, bigger surface
  decision this iteration deliberately routes around.

## Info

Forks the spec must settle:

1. **The namespace.** The reserved stdlib namespaces are exactly
   `fs`/`proc`/`net`/`time`/`json`/`env` (compiler-enforced list) — a
   new `crypto` namespace touches that list plus the typechecker
   table, or the functions ride an existing namespace. Leaning: a real
   `crypto` namespace (the list exists to be grown deliberately; this
   is deliberate).
2. **Surface shape**: `crypto.sha1(b: Bytes) -> Bytes`,
   `crypto.sha256(b: Bytes) -> Bytes`,
   `crypto.hmac_sha256(key: Bytes, msg: Bytes) -> Bytes` — Text
   convenience overloads rejected (the caller has `bytes_of_text`).
3. **Implementation source**: hand-rolled C from the FIPS pseudocode
   (~200 lines for both digests; well-trodden, vector-verified) — the
   doctrine's shape. No linking against OpenSSL, ever.
4. **Where the code lives**: `runtime/src/crypto.c` beside sysio, or
   inside builtin.c — file layout, the executor decides.

## Proposed Solution

Brainstorm → (small) spec settling the four forks → implement with the
`time.ticks` slice's shape (ids, one table row per compiler surface,
contract-doc rows, vector fixtures). Lands any time before iteration
24's spec; independent of 31/22/23.

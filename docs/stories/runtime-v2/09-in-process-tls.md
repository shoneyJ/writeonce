---
track: runtime-v2
iteration: "9"
status: pending
readiness: refine
---

# runtime-v2 9 — in-process TLS: retiring the proxy-termination doctrine

> Created 2026-09-07 from the gap [`jarvis`](../jarvis/00-story.md) surfaces — an
> assistant must dial an LLM over HTTPS, and the runtime has no outbound TLS. The
> developer chose the **full overturn**: the runtime gains TLS **both
> directions**, and the standing "TLS is the proxy's job" doctrine is retired.
> **`readiness: refine`** — the gap, its consumers and its forks are named here;
> the implementation is deliberately left as this story's load-bearing fork, not
> settled.

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

## What it should deliver (scope to be refined)

- **Outbound TLS client** — a `.wo` program dials an HTTPS endpoint: a TLS
  handshake over the TCP socket `net.connect` (language 38) provides, with server
  certificate validation against a trust store. jarvis's direct path, and
  language 38's deliberately-excluded HTTPS half.
- **Inbound TLS server** — porch terminates TLS on its own listener (cert + key
  loaded at startup), retiring the "put a proxy in front" requirement for a
  single-binary deployment.
- **Certificate validation and a trust store** — X.509 chain verification,
  hostname/SNI checks outbound; certificate + private-key loading inbound. This
  is where most of the risk and most of the code live.

## Forks the brainstorm must settle

1. **Implementation source — the load-bearing fork (left open by direction).**
   Vendor a small, audited TLS library (mbedTLS or BearSSL) compiled into the
   single static binary — correct and maintainable, but a build-time external
   dependency, an explicit exception to the no-external-deps doctrine the runtime
   otherwise holds — versus hand-rolling a TLS 1.3 subset plus X.509 in C, which
   matches the sha256 precedent but is thousands of security-critical lines and a
   hand-rolled certificate validator is a CVE factory (strongly discouraged). The
   honest lean is vendor-a-lib; TLS is exactly the thing not to hand-roll. This
   fork decides whether the whole "single static binary, no external deps" story
   gains a footnote.
2. **Phasing.** Outbound first (jarvis's actual need) with inbound to follow, or
   both together since the handshake machinery and the vendored library are
   shared and only the client-vs-server role and validation direction differ.
3. **Trust store and cert provisioning.** Where the outbound trust anchors come
   from (the system CA bundle, and its path across distros), and how the inbound
   side is handed its certificate and key (files, env, a reload story).
4. **TLS version and cipher policy.** TLS 1.3 only (simplest, modern, smallest
   attack surface) versus 1.2+1.3 (broader reach). Leaning 1.3-only.

## Consumers

Named, so this is not a capability shipped as decoration:

- **[jarvis 1](../jarvis/00-story.md)** — outbound HTTPS to the LLM API (the
  reason this story exists).
- **porch** — inbound TLS termination, retiring the mandatory front proxy for a
  single-binary deployment.
- **language 38** — the outbound HTTPS half it excluded by doctrine; this story
  is where that exclusion is lifted.

## Dependencies

- **[language 38](../language-runtime-database/38-content-platform-capabilities.md)**
  — `net.connect` (outbound TCP) is the socket the outbound handshake runs over;
  the client half of this story sits directly on it.

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

## Info

This is the heaviest iteration in the runtime-v2 track and the only one that
forces a doctrine reversal and, most likely, an external-dependency exception —
both flagged above rather than buried. It is pure I/O-plane work (a handshake
layer over the existing socket verbs); no actors, so it is not exposed to the
lang-41 hang. It gates jarvis entirely: until it lands, jarvis cannot reach a
model at all.

# Iteration 24 — chat: the WebSocket pub/sub driving workload

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](../00-story.md).
>
> **Inserted 2026-08-20** (concurrency-chain refinement): the 8+11 arc's
> driving workload, the role log-watcher played for iterations 3–7. Needs
> its spec AFTER the arc's — it lands at the arc's end and proves it.
>
> **RE-SEQUENCED 2026-08-21**: fourth in the chain,
> **stage 3 → 22 → 31 → 24 → 23 → 32** — chat cannot be written honestly
> before [iteration 31](31-actor-lifecycle.md) (request/response,
> bounded mailboxes, actor death, timers). Iteration 19 LANDED
> 2026-08-20, so Bytes is available for frame parse/serialize.

## Why this iteration exists

Everything the framework ledger parks behind concurrency — WebSockets,
pub/sub, streaming, per-request cancellation, the keep-alive parking
retirement — needs a workload that actually exercises long-lived
connections and cross-shard broadcast, or the arc ships mechanism without
proof. Chat is the smallest honest such workload: rooms, N concurrent
clients, fan-out on every message, presence on connect/disconnect — one
binary, no broker.

## Goals

- `docs/examples/chat`: rooms + broadcast + presence over WebSocket,
  served by the framework through `[deps]` exactly as the web-app is.
- Framework grows the WS mechanism in pure `.wo`: the HTTP/1.1 upgrade
  handshake (Sec-WebSocket-Accept needs SHA-1/base64 — the crypto-builtin
  fork's first real consumer), frame parse/serialize (text, close, ping),
  fiber-per-connection serving (iteration 11), and in-memory channels
  whose delivery is cross-shard message send (iteration 8).
- The arc's acceptance teeth: 1k concurrent clients across shards,
  broadcast latency measured, starvation-free under one hot room,
  SIGTERM drains every connection cleanly.

## Acceptance Criteria (draft — the spec after the arc refines)

- **Given** two clients in one room on DIFFERENT shards, **when** one
  sends, **then** the other receives the frame (cross-shard ownership-move
  delivery), and a third client in another room receives nothing.
- **Given** 1k connected clients with one hot sender, **when** the
  reduction budget preempts, **then** every room keeps making progress
  (no starvation) on one OS thread per core, verified by TID.
- **Given** SIGTERM with clients connected, **when** the server drains,
  **then** every connection gets a close frame, every fiber unwinds its
  drop maps (ASan zero leaks), and the process exits 0.
- **Given** the web-app running beside chat features, **when** the
  standing gates run, **then** nothing regresses — HTTP and WS share the
  serve loop honestly.

## Out Of Scope

Message persistence/history (a `@table` an app adds if it wants — not the
workload's point); auth beyond the existing bearer mechanism; permessage
compression; binary frames beyond echo coverage; wss (TLS stays at the
proxy — the proxy story extends to WS pass-through, documented).

## Info

- Dependencies: the 8+11 arc (fibers + cross-shard send — stages 1+2
  landed 2026-08-20, stage 3 pending), [iteration 31](31-actor-lifecycle.md)
  (request/response, backpressure, death, timers — the mechanisms rooms
  and presence are made of), iteration 19 (LANDED 2026-08-20 — Bytes
  carries the frames), the crypto builtins fork (SHA-1 for the upgrade
  handshake — note: the ledger's crypto slice lists SHA-256/512; the WS
  handshake specifically needs SHA-1, so the builtin set must include
  it), and the framework's parse seam (upgrade is an HTTP request until
  it isn't).
- Unparks on landing: the framework ledger's WebSocket/pub-sub rows and
  the iteration-18 rejection note ("pub/sub REJECTED until 8/11").

## Proposed Solution

Brainstorm → spec → plan after the arc's spec exists; the arc's plan and
this iteration's are written against each other (the arc names chat as
its acceptance, chat names the arc as its substrate).

---
iteration: "35"
status: done
readiness: ready
---

# Iteration 35 — `net` runtime seams: timeouts, Unix sockets, peer address

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).
>
> **LANDED 2026-08-23** (branch `framework-v1b`, with the serving slice
> riding it): per-call `_dl` deadlines (nil/false = the expected
> timeout; ids 91–93), `net.listen_unix` (unlink-before-bind, id 94),
> `net.peer` (id 95). Plane: shard-tick TIMEOUT + expiry sweep +
> POLL_REMOVE tombstone on uring, extended deadline scan on epoll,
> fibers POOLED against stale-CQE UAF — the full design in
> [the review spec](../../superpowers/specs/2026-08-23-net-seams-park-design.md).
> Proof: all five seams probe-verified on BOTH `WO_IO` backends;
> `serve_conn` + web-app's fiber-per-connection pattern gate parallel
> requests, idle eviction, and slow-loris tearing (web-app 41 checks).
>
> **Inserted 2026-08-22** — the framework ledger's three 🔧 rows get one
> owner: "Read/write/idle timeouts — `net` has no timeout surface",
> "Unix socket binding — `net.listen` is TCP-only", and "Trusted-proxy
> client IP — needs a peer-address runtime seam". Each is a small
> builtin-surface addition; the framework knobs built ON them stay
> framework slices. Off the concurrency chain; no chain item depends on
> it, but a production-shaped deployment (proxy in front, sockets not
> ports, slow-client defense) needs all three.

## Why this iteration exists

The framework cannot defend against a slow client (no read deadline —
one stalled socket parks a fiber forever), cannot sit behind a
same-host proxy the idiomatic way (Unix socket beats a loopback port),
and cannot TRUST `X-Forwarded-For` (parsing is expressible in `.wo`
today, but verifying the peer actually IS the proxy needs the peer's
address, which no builtin exposes). All three are runtime seams by
nature: the information or mechanism lives at the fd level.

## Goals

- **Deadlines on parked net I/O.** A read/accept/write that would park
  can carry a deadline; expiry resumes the fiber with a distinguishable
  timeout result (nil-or-trap decided by the spec, consistent with the
  stdlib's nil-vs-trap contracts). On serving shards this composes with
  the existing plane — a park is ALREADY a POLL_ADD or TIMEOUT
  submission (arc T4); the seam arms both and takes whichever fires.
  Program mode gets the same surface over blocking syscalls.
- **Unix-domain listeners and connections** beside the TCP ones — same
  accept/read/write/close builtins afterward (an fd is an fd; only the
  bind/connect shape differs).
- **The peer's address, readable** — for an accepted connection, enough
  to answer "is this my trusted proxy?" (address + family; port where
  meaningful). The framework's trusted-proxy middleware then becomes a
  pure-`.wo` candidate slice.
- **The framework knobs are explicitly NOT here** — timeout defaults,
  proxy allowlists, socket-path config all live in framework slices
  that consume these seams.

## Acceptance Criteria (draft — the spec refines)

- **Given** a fiber reading a socket whose peer sends nothing, **when**
  the declared deadline expires, **then** the fiber resumes with the
  timeout result (never a hang), other fibers having run throughout
  (TID-verified), and the fd is still usable or cleanly closed per the
  spec's stated semantics.
- **Given** a listener on a Unix socket path, **when** a client
  connects and exchanges bytes, **then** the whole existing net surface
  works unchanged over it, and the log-watcher/web-app gates stay
  byte-identical on TCP.
- **Given** an accepted connection, **when** the handler asks for the
  peer address, **then** loopback TCP and Unix-socket peers are both
  identifiable, and the answer round-trips into the trusted-proxy
  check's comparison.
- **Given** the full battery plus a soak with deliberately stalled
  clients, **when** it runs, **then** zero leaked fds and flat RSS —
  timeouts must CLEAN UP, not merely return.

## Out Of Scope

- Framework policy (default timeout values, proxy allowlist shape,
  keep-alive idle policy) — framework slices on top.
- TLS, h2c — unchanged owners (proxy; parked behind 23).
- Connect-side timeouts for outbound clients beyond what the deadline
  seam gives free — no workload asks yet.
- Cancellation as a general mechanism — iteration 31's
  request/response + timers own actor-level cancellation; this is
  strictly fd-level deadlines.

## Info

Forks the spec must settle:

1. **Timeout result shape**: nil result vs a distinguishable trap —
   must follow the stdlib's existing nil-vs-trap doctrine
   (`07`-series contracts; a timeout is an EXPECTED outcome, which
   argues nil).
2. **Deadline plumbing on the plane**: one park may need BOTH a
   POLL_ADD and a TIMEOUT in flight (io_uring linked ops vs two
   submissions + first-wins cancel; epoll fallback = the existing
   deadline scan). The park protocol contract
   ([`03-concurrency-coroutines.md`](../../plan/oop-vm/03-concurrency-coroutines.md))
   gains the rule.
3. **Surface shape**: per-call deadline argument vs per-fd setting
   (`net.set_deadline(fd, ms)`); leaning per-call — no hidden fd state,
   matches the no-coloring doctrine.
4. **Unix-socket path semantics**: unlink-before-bind? stale-socket
   handling on restart (the never-stopping-runtime doctrine says a
   restart must not need manual cleanup).

## Proposed Solution

Brainstorm → small spec settling the four forks → implement in the
`time.ticks`/34 shape (builtin ids + park.c deadline arming + contract
rows + fixtures incl. a stalled-client corpus case). Independent of the
chain; natural pairing is right before or with iteration 24 (chat wants
read deadlines for dead-client eviction even before lifecycle timers).

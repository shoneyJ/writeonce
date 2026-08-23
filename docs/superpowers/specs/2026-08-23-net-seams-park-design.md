# Iteration 35 — net seams: the SQE/CQE plane design (for review)

> **Status: LANDED 2026-08-23** (branch `framework-v1b`) — implemented
> as designed; one addition found by the probe: `listen_unix` must set
> O_NONBLOCK on the LISTENER (accept4's flag covers only accepted
> sockets). Covers story 35's fork 2 (deadline plumbing on the plane)
> plus the surface decisions taken with it. Normative park-protocol
> home once approved:
> [`../../plan/oop-vm/03-concurrency-coroutines.md`](../../plan/oop-vm/03-concurrency-coroutines.md).

## Decisions taken (the story's four forks)

1. **Timeout result**: nil/false, never a trap — a timeout is an
   EXPECTED outcome (`parse_int` doctrine). `net.read_dl -> ?Text`
   (nil = deadline, "" = EOF), `net.accept_dl -> ?Int`,
   `net.write_dl -> Bool` (false = torn mid-write, close the fd).
2. **Plane plumbing**: shard-tick TIMEOUT + expiry sweep + POLL_REMOVE
   tombstone (the table below) — NOT per-fiber second ops, NOT linked
   ops.
3. **Surface**: per-CALL deadline argument (`_dl` builtin variants,
   ids 91–95; 89/90 stay reserved for monitor/time.after). No hidden
   fd state; `ms <= 0` = the old blocking behavior bit for bit.
4. **Unix sockets**: `net.listen_unix(path)` unlinks a stale socket
   file before bind — a restart never needs manual cleanup.
   `net.peer(fd) -> Text`: `"ip:port"` (TCP), `"unix"`, `""` on error.

## The ring today (arc T4, landed) and the additions

| SQE | user_data | CQE consumer action |
| --- | --- | --- |
| POLL_ADD fd, oneshot — net accept/read/write park | fiber pointer | `state == PARKED` → wake; else ignore (stale, benign) |
| TIMEOUT from `fb->park_ts` — time.sleep (`park_done=1`: resume continues PAST the builtin) | fiber pointer | same wake path; the op IS the waker — 1 op, 1 CQE, consumed exactly at wake |
| POLL_ADD wake_efd, oneshot — inbox envelopes arrived | `EFD_SENTINEL` (1) | drain eventfd, re-arm, return "adopt-needed" |
| **NEW** TIMEOUT, one per SHARD ("tick"), armed for the NEAREST fd-park deadline (`vm->tick_ts`) | `TICK_SENTINEL` (2) | `tick_armed = 0`; the sweep after the CQE batch wakes every expired fd-park |
| **NEW** POLL_REMOVE, `addr` = the expired park's fiber pointer (matches its POLL's user_data) | `CANCEL_SENTINEL` (3) | nothing — the tombstone's own completion |

## Why this shape

- **Standing invariant (arc T4):** one park = one SQE, and its CQE is
  consumed precisely when the fiber wakes — no op ever outlives its
  fiber.
- **The problem a deadline'd read creates:** two racing wait sources
  (fd readiness, timer). Two per-fiber ops would let the LOSER's CQE
  land after the fiber is freed — a use-after-free on the `user_data`
  dereference one wait later.
- **The fix, three parts:**
  1. fd-parks keep exactly ONE op (their POLL_ADD); deadlines ride the
     shard tick, whose user_data is a sentinel and can never dangle;
  2. the post-CQE sweep wakes expired fd-parks and submits POLL_REMOVE
     to tombstone the orphaned poll (its -ECANCELED CQE arrives later
     with the fiber's user_data and is dropped by the
     `state == PARKED` guard);
  3. dead fibers are POOLED, never freed mid-run (`vm->fib_pool`,
     freed at vm teardown) — even a post-mortem `state` read hits live
     memory; the worst outcome anywhere is a SPURIOUS wake, which the
     re-execute protocol absorbs (the builtin re-checks EAGAIN and the
     deadline). Steady-state pool size = peak live fiber count.
- **epoll fallback:** zero ops — the existing parked-list deadline scan
  (previously sleeps only) now also covers fd-parks with
  `park_deadline > 0`; expiry = `EPOLL_CTL_DEL` + wake.
- **Rejected:** `IOSQE_IO_LINK` POLL→TIMEOUT chains (kernel cancels the
  loser) — tighter, but linked-op error semantics are subtle and the
  ring stays on 5.1-safe ops by doctrine. Also rejected: per-fd
  deadline setting (`net.set_deadline`) — hidden fd state, against the
  no-coloring lean the story records.

## The `_dl` builtin state machine

- FIRST entry stamps the absolute deadline into the fiber
  (`fb->dl_active`, `fb->dl_at` — wall ms). The park protocol
  RE-EXECUTES a parked builtin, and these fields are how the retry
  remembers the original deadline.
- Every entry retries the syscall. Success/EOF/error → clear
  `dl_active`, answer as the plain builtin would.
- EAGAIN with the deadline passed → clear `dl_active`, answer the
  timeout result (nil / false).
- EAGAIN before the deadline → park with `park_fd` AND
  `park_deadline = dl_at` both set; whichever fires first resumes the
  builtin, which loops back to "every entry retries".
- Existing plain parks (`net.read`/`accept`/`write`) now explicitly
  zero `park_deadline` — the sweep must never read a stale sleep
  deadline off a reused fiber.

## Consumers this unblocks (the serving slice, same branch)

- framework `serve_conn(fd, dispatcher, read_ms, idle_ms)`: the
  keep-alive loop where an idle parked connection is finally LEGAL
  (idle deadline evicts it — close-when-idle retires); slow-client
  reads bounded by `read_ms`.
- App-owned acceptor pattern: `accept_dl` loop (stop-flag checked per
  tick) + the app spawns ITS conn-worker actor per connection, each
  building its own App (interface-typed actor state — spike-proven).
- `client_ip` verification (`net.peer`) and unix-socket deployment
  become pure framework/app slices.

## Proof plan (pending)

- Full battery (byte-identical: `ms <= 0` paths and untouched
  builtins).
- New gate: stalled client evicted at the deadline (fds + RSS flat
  over a soak), parallel requests complete out of order, unix listener
  + peer round-trip — web-app gate additions + both `WO_IO` backends.

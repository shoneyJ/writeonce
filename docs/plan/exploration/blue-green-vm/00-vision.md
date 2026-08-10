# Blue/Green VMs — a self-hosting, agent-managed runtime

> **Partially superseded (2026-08-03):** the deployment subsystem (§5–§6 here)
> is now specified in
> [`docs/superpowers/specs/2026-08-03-blue-green-vm-design.md`](../../../superpowers/specs/2026-08-03-blue-green-vm-design.md)
> — developer + `wo` CLI as the management client (agent/MCP becomes a later
> wrapper), schema migration folded into the approval step (additive-only
> auto-diff in v1), fixed slots with alternating activity, HTTP+JSON+SSE.
> §1–§4 (transports, recipe box, fibers, source-in-binary) remain current
> thinking feeding plans 3/4/6.

> Thought-process capture (2026-08-02). Not a phase plan yet — the vision that
> shapes how the wovm runtime grows past milestone 1, recorded before the
> details harden. Related: the OOP spec
> ([`../../../superpowers/specs/2026-08-01-oop-compiler-vm-design.md`](../../../superpowers/specs/2026-08-01-oop-compiler-vm-design.md)),
> plan 4 (shard-actor runtime), plan 15 (MCP streamable HTTP), and the
> single-binary trailer already shipped by `woc build`.

## The idea, in five sentences

The writeonce executable is a **systemd service that never stops**. It embeds
its own **source code**, not just its bytecode. An external **Claude agent**
reads and edits that source through a managed channel; an approved change is
compiled **inside the runtime** and loaded into the idle VM slot. The runtime
holds **exactly two VMs — Blue (active) and Green (previous version)** — and
deployment is an atomic switch between them. Rollback is the same switch in
reverse, because the previous version never left memory.

## 1. A runtime is not a port

The runtime core is the VM pair + engine + scheduler — it must run with zero
listeners. Ports are **transports**, attached at boot like modules: an HTTP
listener, a unix socket, an MCP endpoint, stdio. Consequences:

- The same binary serves as web app, CLI batch runner, or agent-managed
  service depending on which transports the deployment attaches — one of the
  recipes a custom web framework builds from (§2).
- **systemd socket activation** fits exactly: the unit owns the socket
  (`LISTEN_FDS`), the runtime accepts on whatever fds it inherits. The
  "always running" property (§6) and the "no port of its own" property come
  from the same mechanism.

## 2. The runtime is a recipe box for web frameworks

Everything a custom web framework needs in later phases must exist as a
separable runtime capability, not a monolith: transports (§1), fibers (§3),
routing surface (plan 6), the subscription registry (plan 7), the DB engine
(plan 5), and the deploy/rollback machinery (§5). A "framework" in a later
phase is a `.wo` library that composes these recipes — the runtime itself
stays framework-agnostic.

## 3. Fibers (green threads)

Concurrency inside a shard is **cooperative fibers scheduled by the VM**, not
OS threads — the Erlang shape on the wovm substrate:

- A fiber is exactly the execution state `wo_vm` already isolates: a register
  window stack + frame stack + a current pc. Making that state per-fiber
  instead of per-VM turns the interpreter into a fiber scheduler almost for
  free.
- **Preemption by reduction budget**: the dispatch loop decrements a counter
  per instruction (or per call/back-edge); at zero, the fiber parks and the
  scheduler picks the next runnable one. No signals, no stack switching
  tricks, deterministic and debuggable.
- Fibers **park on I/O**: a blocked read hands the fd to the shard's event
  loop (`wo-rt.c`'s epoll/io_uring machinery) and the fiber resumes when the
  completion arrives. One OS thread per core (plan 4's shard), thousands of
  fibers per shard.
- Fits the ownership model: a fiber is an actor mailbox owner; cross-fiber
  sends follow the same ownership-move rule as cross-shard sends.

## 4. The binary contains its source

`woc build` already appends the `.wob` image to a copy of `wovm` with an
offset trailer. The trailer grows one more section: **the `.wo` source tree**
(paths + contents, compressed). Why:

- The deployed artifact is self-describing — no "which commit is prod
  running?" class of question. `wovm --dump-source` can always reproduce
  exactly what is executing.
- The agent workflow (§5) needs a source of truth that travels with the
  binary, not a checkout that can drift from it.
- After a deployment, the runtime rewrites its own source section (write to
  temp, fsync, rename) so the artifact on disk always matches the Blue VM.

## 5. Agent-managed source — how Claude fits

The runtime exposes a **management transport** (MCP over streamable HTTP —
plan 15's machinery, localhost + bearer token, the log-watcher posture).
Claude Code connects as an MCP client. Tools the runtime serves:

| Tool | What it does |
| --- | --- |
| `source_list` / `source_read` | browse the embedded source tree of the running (Blue) version |
| `source_propose` | submit a changed file set as a **proposal** — staged, never applied |
| `proposal_diff` | render the pending proposal against Blue's source |
| `proposal_check` | run `woc check` on the proposal inside the runtime — diagnostics come back to the agent |
| `proposal_approve` | **human-only gate** (separate credential or out-of-band confirmation) — approval triggers compile + green-slot load |
| `deploy_switch` | atomic Blue↔Green switch after health checks |
| `deploy_rollback` | the same switch back — Green still holds the previous version |
| `deploy_status` | which version is Blue, which is Green, in-flight drain state |

Properties worth pinning now:

- **The agent proposes; a human approves.** `proposal_approve` is not
  reachable with the agent's token. Approval is the compile trigger, not the
  edit.
- **Every step is WAL-logged** — proposals, diagnostics, approvals, switches,
  rollbacks form an audit trail that survives crashes like any other commit.
- **The compiler lives with the runtime** for this loop to work: either
  `woc` embedded in the binary (adds OCaml runtime weight) or shipped beside
  it in the service directory (lighter; the systemd unit owns both files).
  Open question in §8 — start with "beside it".

## 6. Blue/Green VM lifecycle

Exactly **two VM slots** per runtime, never more:

- **Blue** — the active VM: all new requests/fibers dispatch into it.
- **Green** — the previous version, loaded and warm: the instant-rollback
  target. After a successful deploy the roles swap; the old Blue becomes the
  new Green.

The critical separation: **VMs own code, the engine owns data.** Tables,
WAL, subscriptions, and the arena slabs live in the engine layer beneath both
VMs; a switch swaps which bytecode handles requests, never the data. That is
what makes the switch cheap and rollback safe — no state migration on the
happy path (and schema changes are exactly the hard part, §8).

Deploy sequence:

1. Approved proposal compiles (`woc emit`) — failure ends the deploy,
   Blue untouched.
2. New image loads + validates into the idle slot (loader is the same
   validation battery as always — a bad image cannot boot).
3. Health gate: entry smoke / conformance subset runs against the idle VM.
4. **Switch at the dispatch boundary**: new work enters the new Blue;
   in-flight fibers on the old VM drain to completion (bounded timeout).
5. Old Blue becomes Green (rollback target); the binary's source section is
   rewritten to match (§4).
6. `deploy_rollback` at any later point is step 4 in reverse — no compile,
   no load, the code is already resident.

## 7. Always running

The executable maps to a **systemd service**: `Restart=always`, socket
activation for the transports (§1), the hardening posture proven in the
log-watcher units (unprivileged user, read-only system, `StateDirectory`
for WAL/data). Deployment never restarts the unit — that is the whole point
of the VM pair. The unit restarting (crash, host reboot) boots Blue from the
binary's current source/bytecode section and reloads Green only when the
next deploy happens.

## 8. Open questions (deliberately unresolved here)

1. **Schema migrations.** Code switches atomically; data does not. A
   proposal that changes a class's fields needs a migration story between
   Green-shaped and Blue-shaped rows — the wo-seg migration doc's
   dual-write thinking applies inside one process. Hardest problem in this
   vision; needs its own exploration.
2. **Live subscriptions across a switch.** Do WebSocket subscribers survive
   a deploy (registry lives in the engine layer → yes, by design), and what
   do they see mid-drain?
3. **`woc` placement** — beside the binary vs embedded (§5).
4. **Fiber preemption granularity** — per-instruction counter vs
   call/back-edge only (cheaper, coarser).
5. **Does Green count against the heap budget** (two arenas resident) or
   does Green hibernate (bytecode resident, heap lazily rebuilt on
   rollback)?

## 9. Where this lands in the plan sequence

- Fibers (§3): extends **plan 4** (shard-actor runtime) — same scheduler
  work, one more scheduling unit.
- Transports-not-ports (§1): shapes **plan 6** (HTTP/service layer) — the
  listener becomes one attachable transport among several.
- Management MCP (§5): builds on **plan 15**'s streamable-HTTP machinery.
- Source-in-binary (§4): extends plan 3's `woc build` trailer.
- Blue/Green switch (§6) + agent loop (§5): a new phase after those land —
  needs spec + plan of its own once this vision stabilizes.

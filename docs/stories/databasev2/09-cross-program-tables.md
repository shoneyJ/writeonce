---
track: databasev2
iteration: "9"
was_language_iteration: "20"
status: hold
readiness: refine
---

# databasev2 9 — cross-program tables: attach to a running program's database

> **Moved 2026-08-26** from the language track, where this was iteration 20.
> Part of [Story — databasev2: the database beyond RAM](00-story.md). Content unchanged by
> the move; its dependencies are restated in that track index.

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](../language-runtime-database/00-story.md)
> — the track this iteration was authored in before the 2026-08-26 move.
>
> **Inserted 2026-08-15**, hence `20`. It follows 9b because a program
> attaching to another's tables wants the same typed statements and queries
> the owner has — a surface that must exist before it can be shared — and
> precedes iteration 25 because HTTP is the *external* face of a program;
> this iteration is the *writeonce-native* face, program to program on the
> same machine.
>
> **No spec exists yet.** This iteration frames the outcome and records the
> open forks; the design must be brainstormed before a plan is written. The
> forks in *Info* are genuine decisions, not details.

## Goals

- A running program **A** with a persistent database (`WO_DATA`, iteration 9)
  can be **attached** by a second writeonce program **B**: B names A's
  IPC connection string in its own `wo.toml`, and from then on reads and
  writes `A.Table` rows with the same typed statements it uses on its own
  tables — checked by B's compiler against A's declared table shapes.
- **A stays the single writer.** B never opens A's WAL, never maps A's
  slabs: every statement B issues travels the IPC channel and executes
  inside A's engine, through the same choke-point row API A's own
  statements use. The ownership doctrine survives contact with a second
  process because the second process never touches the memory.
- **Access is granted, never assumed.** A's manifest *registers* B by name
  with explicit rights (read, or read+write); an unregistered client is
  refused at connect, an under-privileged statement is refused at execute
  with a trap B can catch. No registration, no access — including on the
  same uid.

## Acceptance Criteria

- What to achieve?
    - **Given** A running with `[share]` registering client "b" as
      read+write, and B's `wo.toml` carrying `[connect.a]` with A's IPC
      string,
    - **when** B executes `insert a.AuditLog { … }` and a query over
      `a.AuditLog`,
    - **then** the row exists in A (visible to A's own queries, WAL-logged
      before B's insert acknowledges), and B's query returns it — with B's
      compiler having checked every field name against A's declared shape.
- What to achieve?
    - **Given** A registers client "c" as read-only,
    - **when** C executes a query it succeeds, and when C attempts an
      insert,
    - **then** the insert traps with the access-denied code inside C
      (catchable), and A's log records the refusal; nothing was applied,
      nothing was WAL-logged.
- What to achieve?
    - **Given** a program with no registration in A's manifest,
    - **when** it presents A's IPC string and attempts to attach,
    - **then** the connect itself is refused — rights are checked at the
      door, not per statement only.
- What to achieve?
    - **Given** B attached and mid-statement,
    - **when** A shuts down cleanly (SIGTERM) or crashes,
    - **then** B's in-flight statement traps with a connection error B can
      catch (never a hang), and B can re-attach after A reboots and
      replays — with every previously acknowledged write still present.
- What to achieve?
    - **Given** the employee sample running as A with its departments and
      employees tables,
    - **when** a second sample program (a thin reporting client) attaches
      read-only and runs the GroupBy report over `a.Employee`,
    - **then** it prints the same report the owner prints — the
      demonstration that attach + query compose.

## Out Of Scope

- **Remote machines.** The IPC string names a local channel; cross-host
  access is the HTTP/service layer's job (iteration 25) or a much later
  network protocol. Same-machine is what "attach" means here.
- **B caching A's rows.** Every read crosses the channel; a client-side
  cache (and its invalidation) is a later performance iteration, if ever.
- **Cross-program transactions.** A statement is atomic inside A exactly as
  A's own statements are; B cannot open a transaction spanning its own
  tables and A's. That is 2PC territory, recorded with the database track's
  deferred items.
- **`LIVE` subscriptions over the channel** — composes with the
  subscription registry later (the client-api phase doc already sketches
  the wire shape).
- **Schema migration while attached** — a blue-green swap in A while B
  holds an attachment is iteration 26's compatibility problem; this
  iteration may simply drop attachments on swap.

## Info

Prior art in the tree: `docs/runtime/database/04-client-api.md` already
designs a native binary wire protocol for external clients (length-prefixed,
typed, subscription-ready) — this iteration's channel should be its
same-machine profile, not a new invention. The WAL's typed value encoding
(`database/src/wal.c`, iteration 9 Task 2) is a working engine-value wire
format today: statements and rows can ride the same encoding the log already
uses. The `wo.toml` manifest exists and is compiler-read (`woc <dir>`), so
both ends' declarations have a natural home.

Forks the spec must settle:

**1. What carries the channel — and what does the IPC string name?**
A unix domain socket is the obvious carrier (peer credentials for free,
`net`-stdlib adjacency); the string would be `unix:/path/a.sock` in B's
`[connect.a]` and A would listen beside its `WO_DATA` directory. The
alternatives — a FIFO pair, shared memory + doorbell — buy latency at the
cost of the credential story and the crash-detection story (a dead socket
peer is unambiguous; a dead shm peer is a protocol). Leaning: unix socket,
one connection per attached client, A serving requests on its event loop
(iteration 8's shard-actor loop when it lands; a dedicated accept loop
until then — which is also the fork's dependency question: how much of
iteration 8 does this need?).

**2. How does B's compiler know A's table shapes?** B typechecks
`a.Employee { … }` against A's declarations, so B needs them at compile
time. Options: B's `[connect.a]` names A's **project directory** and `woc`
reads A's types straight from A's source (simple, but couples B's build to
A's checkout); A **exports a schema file** (a `.wob`-adjacent digest of its
class table) that B's manifest points at (decoupled, but a new artifact
with a staleness story); or shared type definitions in a common module both
import (cleanest language story, needs the module system to span projects).
A runtime schema handshake must exist regardless — B's compiled expectation
of `a.Employee`'s shape is verified against A's live class table at attach,
and a mismatch refuses the attachment with both sides' shapes named.
Leaning: project-directory reference for the milestone plus the mandatory
handshake; the export artifact when the staleness story matters.

**3. What exactly does A's registration grant?** The request's shape is
per-client rights: `[share] clients = [{ name = "b", rights = "rw" }]` or
per-table refinement (`tables = ["AuditLog"]`). Identity: the client NAME
must be bound to something a peer cannot fake — unix peer credentials
(uid), a token A mints, or both. Leaning: name + uid via `SO_PEERCRED` for
the milestone (same-machine, same-trust-domain), rights whole-database
read or read+write (per-table refinement deferred until a workload needs
it), and the registration is A's manifest so a grant is a config change +
restart, not an API. **Superseded as the end state (2026-08-15):**
identity is a keypair and grants name public keys — iteration
[21](10-keypair-attach-auth.md) owns that; the uid check is only this
iteration's bootstrap and must be flagged pre-21 wherever it ships.

**4. What does B's statement actually block on?** B's insert crosses the
channel, executes in A (RAM + WAL + fsync), and acknowledges back — a
blocking round-trip on B's thread, exactly like B's own `WO_DATA` inserts
block on their own fsync. Queries stream results back whole (materialized;
no cursors over the wire this iteration). The alternative — async
statements with completion callbacks — has no language surface to stand on
(no function values) and waits for fibers (iteration 11). Leaning:
blocking, with the stop-flag rule from the log-watcher work applying (a
SIGTERM'd B parked on a channel read exits cleanly).

## Proposed Solution

- **Brainstorm the spec first**, settling the four forks; then a plan.
  Expected shape: A-side — a listener beside the engine, a request
  dispatcher that executes through the same statement executors iteration
  9 built (`database/src/db.c`), the registration check at accept and per
  statement; B-side — `[connect.<name>]` manifest surface, compiler
  namespace `<name>.Table` binding table statements/queries to channel
  stubs instead of local engine builtins; both — the client-api phase
  doc's wire protocol, profiled for unix sockets, values in the WAL's
  encoding.
- **The acceptance workload extends the employee sample**: A = the employee
  program with `[share]`; B = `docs/examples/employee-list` (pre-authored
  2026-08-15, sample-first — both manifests designed as a pair), attaching
  read-only for the list/report/staff modes and proving the rights matrix
  with its `probe-write` mode. The sample stays the test.
- Depends on iterations 9 (engine, WAL — done through Task 3 as of
  2026-08-15) and 9b (typed statements and queries worth sharing); wants
  iteration 8's event loop for A's serving side but can prototype on a
  dedicated accept loop the way the MCP sample serves today.

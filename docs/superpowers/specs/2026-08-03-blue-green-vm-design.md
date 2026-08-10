# Blue/Green VM deployment — design spec

**Date:** 2026-08-03
**Status:** approved (2026-08-03); implementation plan deferred until plans 5 + 6 ship
**Scope:** the writeonce runtime's in-process deployment subsystem
**Supersedes:** §5–§6 of [`docs/plan/exploration/blue-green-vm/00-vision.md`](../../plan/exploration/blue-green-vm/00-vision.md)

## Motivation

The writeonce executable is a systemd service that never stops. It embeds its
own source; a developer manages that source remotely through the `wo` CLI over
an exposed management port. An approved change — code **and** its database
schema consequence — compiles and migrates inside the runtime, loads into the
idle VM slot, and deploys by an atomic switch. If anything fails, the active
VM is untouched. Rollback is the same switch back, because the previous
version never left memory.

## Decisions locked during brainstorming

| Question | Decision |
| --- | --- |
| Blue/Green semantics | **Fixed slots; activity alternates.** Blue and Green are two physical VM slots; each deploy loads the idle one and switches to it. "Which is live" is status info (`wo remote status`). |
| Schema migration | **Auto-diff, additive-only in v1.** Additive changes migrate automatically; destructive changes reject the deploy. Script-based migrations are a recorded later phase. |
| Management protocol | **HTTP + JSON + SSE** on a loopback management transport, bearer token — hand-rollable on the runtime's own HTTP machinery, curl-debuggable. MCP wrapper for agents is a later layer over the same endpoints. |
| Edit model | **Local-first, CLI push.** `wo remote pull` fetches the embedded source; the developer edits locally; `wo remote propose` pushes a staged proposal. The runtime never hosts a mutable workspace. |
| Architecture | **Approach A: management plane as a runtime module + thin CLI.** In-process slots; rejected: external deployer daemon (violates two-VMs-in-one-runtime, splits data), self-hosted `.wo` deploy logic (bootstrap problem — later). |

## 1. Scope & positioning

**In scope:** two VM slots, the deploy state machine, additive schema
migration, the management HTTP+SSE surface, `wo remote` CLI verbs, the
source-in-binary trailer section.

**Out of scope (recorded extensions):** script-based/destructive migrations,
an in-runtime editing workspace, the MCP/agent wrapper, fibers (plan 4 owns
them).

**Prerequisites:** plan 5 (DB engine binding — a catalog to diff), plan 6
(HTTP machinery). This spec is written ahead; its implementation plan is
authored when those ship.

## 2. Runtime anatomy

- **Two fixed VM slots — Blue and Green.** One is *active*: all new requests
  dispatch into it. The other is *standby*: the previous version, loaded and
  warm, the instant-rollback target. Activity alternates on each deploy.
- **VMs own code; the engine owns data.** Tables, WAL, subscriptions, and
  arena slabs live below both slots. A switch swaps the dispatch pointer at
  the request boundary; in-flight work drains on the old slot under a bounded
  timeout. No data moves on deploy or rollback.
- **A runtime is not tied to a port.** Transports (application HTTP, the
  management endpoint, future ones) attach at boot; systemd **socket
  activation** is supported — the unit owns the sockets, the runtime accepts
  whatever fds it inherits. The runtime also runs with zero listeners.
- **The binary contains its source.** The `woc build` trailer carries the
  `.wo` source tree beside the `.wob` image. After a successful deploy the
  runtime rewrites its own trailer (write temp, fsync, rename) so the binary
  on disk always matches the active slot. `wo remote pull` serves from this
  section; there is no "which commit is prod running" question.
- **Always running.** The executable maps to a systemd service
  (`Restart=always`). Deploys never restart the process. After a crash or
  reboot, the unit boots the active version from the trailer; the standby
  slot refills on the next deploy.
- **Recipe-box rule.** Each capability here (transports, slots, proposal
  store, differ, deploy machine) stays a separable runtime module — a custom
  web framework in later phases composes them; the runtime stays
  framework-agnostic.

## 3. Deploy pipeline

States, WAL-logged at every transition:

```
DRAFT ──propose──► STAGED ──approve──► COMPILING ──ok──► MIGRATING ──ok──► LOADING
                     │                     │fail             │fail            │fail
                     │reject               ▼                 ▼                ▼
                     ▼                  FAILED (active slot untouched, standby unchanged)
                  REJECTED
LOADING ──ok──► HEALTH ──ok──► SWITCHING ──drained──► DEPLOYED
                  │fail            (atomic dispatch swap;
                  ▼                 old active becomes standby)
               FAILED
```

- **propose** — the CLI pushes the changed file set plus the base-version
  hash it was edited against. A stale base (someone deployed since the
  `pull`) rejects at propose time, not after approval. The runtime computes
  the catalog diff immediately and stores it with the proposal.
- **approve** — an explicit CLI verb covering BOTH the code change and the
  migration plan: `wo remote diff` shows the source diff *and* the schema
  consequence, so approval is informed. Approval triggers the pipeline.
- **compile** — in-runtime `woc emit` against the proposal. Any diagnostic →
  `FAILED`; diagnostics stream to the developer; the active slot is never
  touched.
- **migrate** — additive catalog changes apply to the engine (§4), **before**
  the switch, while old code still serves. Safe because additive changes are
  invisible to old code.
- **load + health** — the new image passes the standard loader validation
  battery (a bad image cannot boot), then a smoke subset runs against the
  idle slot.
- **switch** — dispatch-pointer swap; drain with bounded timeout; the old
  active becomes standby; the trailer rewrites.
- **rollback** — `wo remote rollback` switches back to standby: no compile,
  no load, code already resident. The schema stays as migrated —
  additive-only (§4) guarantees the older code runs correctly against it.
- **streaming** — every stage's output (compiler diagnostics, migration
  progress, health results, switch/drain status) streams to the developer as
  it happens (§5).

Failure semantics, uniform: a failure at any stage leaves the active slot
serving and the standby slot unchanged; the proposal lands in `FAILED` with
its stream and WAL trail intact.

## 4. Migration rules (additive-only v1)

The engine diffs the old catalog against the proposal's catalog at propose
time:

| Change | Verdict |
| --- | --- |
| new class or type | auto-migrates |
| new field **with a default** | auto-migrates — backfilled with the default in RAM + WAL |
| new index | auto-migrates (built before switch) |
| new interface / method / service block | auto-migrates (code-only) |
| new field **without a default** | reject: "add a default" |
| drop / rename / retype a field | reject in v1 |
| drop a class; remove a union variant; flip `@gc` | reject in v1 |

- Rejections happen at **propose** time and name the offending declaration —
  the developer never waits for an approval to learn the change can't ship.
- The migration itself is a WAL-logged engine transaction: backfills run per
  shard and ack like any commit. A crash mid-migration replays or discards
  the whole migration on boot — never half.
- The additive-only rule is exactly what makes rollback unconditional: the
  previous version ignores fields and classes it never knew.
- Script-based migrations (destructive changes, data transforms, down
  scripts) are the recorded follow-up phase; nothing in this design blocks
  them.

## 5. Management surface & CLI

Endpoints under `/manage` on the management transport (loopback by default):

| Endpoint | Verb | Purpose |
| --- | --- | --- |
| `/manage/source` | GET | the embedded source tree (paths + contents) of the active slot |
| `/manage/proposals` | POST | submit a proposal (file set + base hash) → id, catalog diff computed |
| `/manage/proposals/<id>` | GET | proposal state, source diff, schema diff |
| `/manage/proposals/<id>/approve` | POST | approve → pipeline starts |
| `/manage/proposals/<id>/stream` | GET | **SSE**: stage events, compiler diagnostics, migration progress, health, switch/drain — replayable from the WAL after reconnect |
| `/manage/deploy/rollback` | POST | switch back to standby |
| `/manage/status` | GET | which slot is active, versions (source hashes), drain state, last deploy record |

`wo` CLI verbs mapping 1:1: `wo remote pull / status / propose / diff /
approve / deploy-log / rollback`. `propose` prints the id; `approve` attaches
to the SSE stream and renders it live — the developer watches compile,
migration, health, and switch scroll by; a failure shows the exact
diagnostics inline.

## 6. Security & audit

- Management transport binds loopback by default; reaching it remotely is an
  SSH tunnel (the log-watcher posture). Bearer token from the service's
  environment file — config stays world-readable, the secret does not.
- The application transport and management transport are separate listeners:
  app traffic can never reach `/manage` routes.
- Approval is a deliberate second step, not implied by propose — and the two
  verbs can carry distinct tokens later (agent proposes, human approves)
  without changing the design.
- Everything is WAL-logged: proposals, diffs, approvals, every stage
  transition, switches, rollbacks. The deploy history survives crashes like
  any other committed data and is queryable via `/manage/status`.

## 7. Testing

- **State-machine unit tests** (runtime): every transition and every failure
  edge — compile fail, migration reject, load reject, health fail, drain
  timeout — asserting the active slot is untouched after each.
- **Catalog-differ table tests:** one fixture per row of the §4 verdict
  table, both verdicts.
- **Deploy e2e** (corpus harness): boot a fixture app, propose an additive
  change, approve, assert the SSE stream's stage sequence, assert new code
  serves and old data survives; then rollback and assert the previous
  behavior returns with the migrated schema intact.
- **Crash battery:** kill the process in COMPILING / MIGRATING / SWITCHING /
  trailer-rewrite; on reboot the runtime serves the correct version and the
  WAL shows whole migrations only.
- **Stale-base test:** two pulls, one deploys, the other's propose rejects.
- ASan on the runtime side throughout, as established by the wovm gate.

## Success criteria

1. A fixture app deploys an additive change with zero dropped requests
   (in-flight drain proven by the e2e harness).
2. Every failure stage leaves the active slot serving and is visible in the
   SSE stream and the WAL audit trail.
3. `wo remote rollback` restores the previous version in under one second
   with no compile and no data change.
4. Kill -9 at any pipeline stage: reboot serves a consistent version; no
   half-applied migration exists.
5. The binary's trailer always matches the active slot after DEPLOYED
   (verified by `wo remote pull` hash comparison in the e2e).

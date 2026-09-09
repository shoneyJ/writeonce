---
iteration: "38"
status: pending
readiness: refine
---

# Iteration 38 — content collaboration workload: the file-mutation and outbound-socket gaps

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).
>
> **Inserted 2026-08-26** (developer ask: "can the language create an
> application like Nextcloud, a content collaboration platform?"). The
> answer was no, and the reason was not missing `.wo` effort: two whole
> capability families are unowned by any iteration. Read off the runtime
> contract, not off prose — `runtime/src/wob.h` allocates exactly six
> `fs` builtins, ids 40–45 (`exists`, `list`, `stat`, `read_all`,
> `read_at`, `append`; `append` "creates the file if absent"), so a file
> can be **created and grown and then never replaced, truncated, deleted
> or renamed**. It allocates ten `net` builtins — 51–55
> (`listen`/`accept`/`read`/`write`/`close`) and 91–95 (the three `_dl`
> deadline twins, `listen_unix`, `peer`) — and **no `connect`**;
> `grep -rn 'connect(' runtime/src/` returns nothing, so a program cannot
> open an outbound connection to anything, ever. This iteration is the
> workload that forces both into the open, in the mould of
> [iteration 28](28-skillhost-host-workload.md).

## Goals

- **A writeonce program shaped like a content platform.** `docs/examples/vault`:
  authenticated users, folders and documents as `@table` classes related by
  `ref`/`backlink`, multipart upload intake, content-addressed blobs, share
  grants with expiry, a version chain per document, a trash bucket, an
  activity feed, and a server-rendered UI over `porch` +
  `writeonce-view`. Everything above the storage line is expressible on
  today's toolchain and is the part that ships.
- **`fs` grows the mutation verbs.** The gap is not ergonomic. A blob store
  that can only `append` can never reclaim a byte: a cancelled upload, a
  deleted document and a superseded version all leak their file forever,
  and there is no atomic-publish primitive (write-temp-then-`rename`) so
  every write is torn-visible to a concurrent reader. Name the minimum set
  the workload actually needs and no more. The syscalls are not the work —
  `sysio.c:669` already calls `unlink()` on the `listen_unix` path to clear
  a stale socket, so `remove` is a builtin-table row over a call the
  runtime already links.
- **`net` grows the client side.** Every federation, identity and
  notification story in a collaboration platform is an outbound call —
  OIDC token exchange, SMTP for share mail, an object-store backend, a
  webhook, an antivirus or preview service. All of them are one missing
  builtin. [Iteration 35](35-net-runtime-seams.md) declared
  connect-side timeouts out of scope "no workload asks yet" — this is the
  workload asking.
- **Block honestly on the rest, in writing.** WebDAV (the protocol every
  desktop and mobile sync client speaks), collaborative editing, previews
  and full-text search each get a named verdict in this story — owned
  elsewhere, deliberately rejected, or deferred with the reason — so the
  sample's README never implies a feature the stack cannot serve.

## Acceptance Criteria

- **Given** the `vault` sample and today's `fs`, **when** a user deletes a
  document and the acceptance script inspects the blob directory, **then**
  the blob is gone from disk and the disk footprint returns to its
  pre-upload size — the criterion that is impossible today and is the
  whole point of the fs half.
- **Given** two concurrent readers of a document while a new version is
  being written, **when** the write completes, **then** every read
  returned either the whole old version or the whole new one and never a
  partial file — atomic publish, proven by a racing reader, not asserted.
- **Given** an upload that is abandoned mid-stream (client disconnect),
  **when** the next request arrives, **then** no partial blob and no
  orphan row survive — cleanup is reachable from `.wo`.
- **Given** a share notification configured against a local SMTP sink and
  an identity provider stub, **when** a share is created, **then** the
  program opened an outbound connection, spoke the protocol and recorded
  delivery — the connect half proven end to end, offline, with a fixture
  server the gate starts itself (the `deps-accept`/`web-app` precedent:
  no network in CI).
- **Given** an outbound peer that accepts the connection and then never
  answers, **when** the configured deadline expires, **then** the call
  returns the expected-timeout shape, the fd is closed, and the fiber is
  not parked forever — the same cleanup contract iteration 35 set for the
  listen side.
- **Given** the full sample under the standing battery, **when**
  `oop-accept`, `web-app`, `site` and the new gate run, **then** all are
  green and ASan-clean at both shard counts.

## Out Of Scope

- **Raw stdin/stdout byte I/O, bounded/killable subprocesses, recursive
  directory walk, executable-bit and realpath confinement** — every one of
  these is [iteration 28](28-skillhost-host-workload.md)'s
  Blockers B and C and its gap list. This story must not re-own them. It
  *does* depend on 28's killable-subprocess work for previews, which is
  exactly why previews are deferred here rather than attempted.
- **WAL checkpoint and disk reclamation** —
  [databasev2 3](../databasev2/03-wal-checkpoint.md). A metadata store whose boot replays
  every write ever made is a real ceiling for this workload, and naming it
  here is the point; fixing it is 32's. This story's gate should record the
  replay time it observes so 32 inherits a number.
- **WebDAV.** Sync clients speak PROPFIND/PROPPATCH/MKCOL/MOVE/LOCK over
  XML bodies. The language has `json` and no XML parser, and `MKCOL`/`MOVE`
  have nothing to land on until the fs half exists. Verdict for the spec to
  confirm: out of this slice, revisited only once file mutation ships, and
  never as a compiler concern.
- **Collaborative editing (OT/CRDT).** Wants ordered trees and cheap
  lookups; `multi` is an array, `map` is a documented linear scan, there
  are no closures and no generics beyond those two. Deliberately not
  attempted in `.wo` at this stage — and the external-document-server
  route Nextcloud itself takes is unreachable until `connect` lands.
- **Full-text search.** The engine indexes equality probes on declared
  columns; there is no prefix scan or FTS. Query-grammar growth is
  [databasev2 8](../databasev2/08-query-grammar-corpus.md)'s.
- **TLS** — ~~proxy-terminated, by doctrine~~ **RETIRED 2026-09-09.** The
  runtime now speaks TLS 1.3 in-process both directions — this iteration's
  `net.connect` gained an HTTPS sibling `net.connect_tls`, and porch can
  terminate inbound TLS with `net.accept_tls`, all in
  [runtime-v2 9](../runtime-v2/09-in-process-tls.md). The plaintext-to-a-sidecar
  framing above no longer holds.
- **A plugin/app ecosystem.** In-runtime recompile is
  [iteration 26](26-blue-green-deploy.md)'s; nothing here loads
  code at run time.
- **Scale claims.** Rows are resident by default (64 MiB arena default,
  `WO_HEAP_MB`), so this is a team-scale platform and the README states the
  row ceiling it was measured at.

## Info

The gap ledger this story exists to resolve, with current ownership:

Every "today" cell below was read from the source named beside it, not
from another document.

| Capability | Today | Read from | Owner |
| --- | --- | --- | --- |
| overwrite / write-at-offset a file | absent; `append` (id 45) creates-if-absent and grows, nothing more | `wob.h:353-358` | **this story** |
| remove, rename, mkdir, truncate | absent from the builtin table | `wob.h:353-358` | **this story** |
| outbound TCP / unix connect | absent; no `connect()` call in the runtime at all | `wob.h:364-368,477-485`; `grep` over `runtime/src/` | **this story** |
| outbound deadline + cleanup | `_dl` seam exists, listen side only (ids 91–93) | `wob.h:477-482` | **this story**, on 35's contract |
| stdin/stdout bytes | no builtin; only `<stdint.h>` matches `stdin` | `sysio.c`, `wob.h` | 28, Blocker B |
| bounded, killable subprocess | `proc.run` (id 56) takes `(cmd, args, cls)` — no deadline, no signal | `wob.h:369`; `sysio.c:718` | 28, Blocker C |
| directory walk, `X_OK`, realpath | absent | `sysio.c` | 28, gap list |
| WAL checkpoint / bounded replay | no checkpoint, snapshot or truncate anywhere in the WAL; `fdatasync` per commit | `wal.c:403`, `wal.h` | 32 |
| io_uring on the WAL write path | rings exist in the fiber/net plane only; the WAL is plain `fdatasync` | `park.c:22-54` vs `wal.c:403` | 23 |
| `WO_DATA` as a file | hardcoded `"%s/shard-0.wal"` directory form | `main.c:202` | 33 |
| prefix scan / FTS / aggregates | equality probe only; group-by rejected in the typechecker | `types.ml:2220,2241` | 27, parked drain |

Builtin id allocation, so this story's work does not collide: `WO_B_MAX`
is `95u` and ids **89 and 90 are reserved holes** — iteration 31's
`monitor` and `time.after`, named in the active slice's marker and absent
from both `wob.h` and `types.ml`. New builtins here start at **96**. No
`.wob` version bump is implied: `WOB_VERSION` is `6u` and moved last for
iteration 36's opcodes 42–46; stdlib builtins are a table row, exactly as
iteration 35's 91–95 were.

What already exists and needs no new capability, verified against the tree:
`http/multipart` and `http/auth` in `porch`; `sha1`/`sha256`/
`hmac_sha256` (builtin ids 85–87) for content addressing, ETags and session
tokens; `base64_encode`/`decode` and the `Bytes` carrier; `ws_accept` plus
the pure-`.wo` RFC 6455 frame codec for the activity feed; `Component`/
`Layout` with `{{ }}` escaping for the UI; `@table` with `@unique`, `ref`,
`backlink` and FK-restrict for the whole permission and version model.

Forks the spec must settle:

1. **How small is the fs set?** Two candidate shapes. Minimal-atomic:
   `fs.write` (create-or-replace whole file), `fs.remove`, `fs.rename`,
   `fs.mkdir` — four verbs, no offsets, atomic publish by write-temp +
   rename, and a blob store is expressible. Streaming: add
   `fs.write_at`/`fs.truncate` so a large upload can be resumed and a
   sparse file assembled. Leaning minimal-atomic: it closes the deletion
   hole, it is the one shape that composes with 32's own rename-swap, and
   offsets can be argued later by a workload that measures the need.
2. **What does a handle look like — or is there one?** Today every `fs`
   member takes a path and does the whole operation. A `write` that takes a
   path keeps that property and keeps handles out of the ownership model;
   a resumable upload wants an open fd, which means a droppable
   `fs.File` scalar in the shape of `net.Conn`. This fork decides fork 1.
3. **Does `connect` return `net.Conn` and nothing more?** Cheapest honest
   answer is yes: `net.connect(host, port)` and `net.connect_unix(path)`
   yielding the same fd-scalar `accept` already yields, so every existing
   `read`/`write`/`close`/`_dl` member composes unchanged and the client
   side costs two builtins, not a subsystem.
4. **Path confinement.** A content platform maps user-supplied names onto
   paths. Without realpath (28's gap) the sample must confine by
   construction — content-addressed filenames the user never names — or the
   fs half hands `.wo` code a traversal footgun the day it lands. This is
   the security fork and it gates the fs half, not the sample.
5. **Where does the workload stop?** The sample is the acceptance test, so
   its edge is the story's edge. Leaning: upload / download / delete /
   rename / version / share / trash / activity feed, one share
   notification over `connect`, and no sync-client protocol at all.

## Proposed Solution

Two capability halves and a workload, and the workload is written first so
the capability set is argued from a program that needs it rather than from
a wish list. Order: brainstorm the five forks (fork 1 and 2 together, fork
4 before any fs code), then the `vault` sample against today's toolchain to
establish exactly where it blocks, then the fs verbs, then `connect`, each
with corpus fixtures and its own gate leg. Big enough to want a spec and a
plan document — this is not a bounded slice like
[7](../databasev2/07-single-file-db.md).

Precedence: independent of the concurrency chain (stage 3 → 22 → 31 → 24 →
23 → 32) and startable beside it, with one caveat — the honest disk story
needs [3](../databasev2/03-wal-checkpoint.md), so if this lands first its gate records
the replay number rather than claiming the platform is operationally done.

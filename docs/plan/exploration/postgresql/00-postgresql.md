# PostgreSQL — storage subsystem reference

These cards exist to make the Postgres backend a useful **library of patterns** for writeonce's storage, constraint, and index work without inviting a multi-process port. (The storage cards originally fed the Rust-era plans 10–12, removed with that track 2026-08-18; the patterns fed the shipped C engine and remain the reference.) Each card pulls one subsystem out of [`reference/postgresql/src/backend/`](../../../../.dev/reference/postgresql/src/backend/) — paths into the Postgres tree, the underlying *idea*, and the writeonce translation.

The symlink is user-specific:

```bash
ln -s /home/shoney/projects/postgresql reference/postgresql
```

Gitignored — see [`.gitignore`](../../../../.gitignore). Pair it with [`reference/linux`](../../../../.dev/reference/linux) and [`reference/go`](../../../../.dev/reference/go) if not already linked.

## Per-subsystem cards

| # | Postgres area | What writeonce takes | What writeonce skips |
| --- | --- | --- | --- |
| [wal](./wal.md)                                  | `access/transam/xlog*.c`              | append-only sequential log, LSN-as-byte-offset, segment rollover, group commit, `pwrite` + `fsync` at commit | replication/archiver, multi-process WAL writer, GUC matrix |
| [smgr-and-md](./smgr-and-md.md)                  | `storage/smgr/{md,smgr,bulk_write}.c` | one file per relation, segments capped at `RELSEG_SIZE`, immediate vs deferred fsync | multi-fork abstraction (main/fsm/vm), shared-memory descriptor cache |
| [buffer-and-checkpoint](./buffer-and-checkpoint.md) | `storage/buffer/{bufmgr,freelist}.c` + `postmaster/{checkpointer,bgwriter}.c` | page cache + dirty bit + LRU; checkpoint flushes then advances control-file LSN | shared-buffer pinning/unpinning, separate writer processes, latches |
| [page-format](./page-format.md)                  | `storage/page/{bufpage,checksum}.c`   | page header (LSN, checksum, free-space markers); CRC32C trailers | MVCC visibility (xmin/xmax/ctid), access-method-specific opaque space |
| [constraints-and-grammar](./constraints-and-grammar.md) | `parser/gram.y`, `catalog/pg_constraint.h`, `utils/adt/ri_triggers.c` | PK = blessed unique index; FK forward-only catalog + inline-check semantics; ON DELETE action set; backlink-implies-index (our improvement) | trigger machinery, deferrable constraints, MATCH PARTIAL, composite keys |
| [indexing-and-point-lookup](./indexing-and-point-lookup.md) | `access/{nbtree,hash}/README`, `optimizer/path/costsize.c`, `storage/itemptr.h` | hash-bucket point lookup (expected O(1)), index-entry-as-row-address (TID ↔ our slot), selectivity beats seqscan by arithmetic | btree/gin/gist/spgist/brin AMs, cost-based planner, index paging |

## The lift-vs-skip filter

Postgres is multi-process by birth: a postmaster forks one backend per connection plus dedicated checkpointer / bgwriter / walwriter / archiver / autovacuum processes. Most of `src/backend/storage/ipc/`, `storage/lmgr/`, the latch system, and the `proc.c` family exist to coordinate **between those processes** — shared-memory regions, semaphores, condition variables, lock manager partitions, signal forwarding. Writeonce is single-process and single-threaded, so all of that mechanism is dead weight here. The *concepts* underneath (fairness, deadlock detection, request batching) generalize anyway, but writeonce satisfies them with single-thread invariants instead of IPC primitives.

What carries over cleanly:

1. **Sequential WAL with fsync at commit** — applicable to any durable store regardless of process model.
2. **Page cache abstraction** — even single-threaded engines need a dirty/clean bit and an LRU eviction story; the kernel page cache covers most of it via `mmap` / buffered I/O, but the dirty-tracking + flush-batching policy is something we own.
3. **Control file with last-safe-LSN** — small, fixed-size, atomically updated via rename-on-write. Survives multi-process and single-process alike.
4. **Recovery = replay WAL from last checkpoint** — the algorithm is identical; what writeonce skips is the postmaster signaling that says "ok, recovery is done, accept connections."
5. **CRC32C on every record + page** — the cost is a few cycles per write, the pay-off is silent-corruption detection. Worth it.

What stays out:

- **Shared-memory / dynamic-shmem coordination** (`storage/ipc/dsm*.c`, `storage/lmgr/`). Single-thread loop has no co-tenants.
- **Multi-version concurrency control** (`access/transam/clog.c`, xmin/xmax tuple headers). The locked architecture (`docs/runtime/database/02-wo-language.md` § Concurrency Model) commits to MVCC for snapshot isolation, but the version chains are not what makes single-thread writes durable. Layered in later, when LIVE subscribers want pre-commit views.
- **Separate writer processes** (`postmaster/{walwriter,bgwriter,checkpointer,archiver,autovacuum}.c`). Each becomes a per-tick chunk of work in the same loop, gated by deadlines.

## Phase mapping

The implementation phases that lean on this material:

- The Rust-era consumers (plans 10/11/12: storage foundations, WAL and
  recovery, disk cutover) were removed with that track 2026-08-18; their
  ideas shipped in `database/src/` (typed WAL + replay) and the rest wait
  on [iteration 32](../../../stories/language-runtime-database/refine/32-wal-checkpoint.md)
  (checkpoint) — the wal/buffer cards are its entry material.
- Current consumers: [constraints-and-grammar](./constraints-and-grammar.md)
  (the `@table` PK/FK grammar direction) and
  [indexing-and-point-lookup](./indexing-and-point-lookup.md) (the
  O(1) read-path slice iteration 22's numbers demand).

Pair each card with [`docs/plan/exploration/linux/12-pwrite-fsync.md`](../linux/12-pwrite-fsync.md) for the actual syscalls — these cards are about *design patterns*, that one is about *kernel calls*.

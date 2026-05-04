# Page format & checksums

`storage/page/bufpage.h` defines Postgres' on-disk page layout: a fixed `BLCKSZ` (8 KiB by default), with a 24-byte header at the front and tuples filling from the back, line pointers pointing into them. `storage/page/checksum.c` adds an optional CRC32C-derived checksum embedded in the header — turned on at `initdb --data-checksums` time, off by default for historical performance reasons.

Writeonce's phase 10 starts simpler — variable-length records, no pages. Phase 12+ may revisit a page-style layout if range scans become hot enough that record-level reads aren't enough. Either way, the **header + checksum pattern** carries over and the `bufpage.h` header is worth understanding.

## Postgres source

| File | Responsibility |
| --- | --- |
| [`storage/page/bufpage.c`](../../../../reference/postgresql/src/backend/storage/page/bufpage.c) | Page initialization (`PageInit`), line-pointer manipulation, free-space accounting. |
| [`storage/page/checksum.c`](../../../../reference/postgresql/src/backend/storage/page/checksum.c) | The page checksum algorithm — CRC32C-style with a Postgres-specific finalization. Optional, enabled at cluster init. |
| [`storage/page/itemptr.c`](../../../../reference/postgresql/src/backend/storage/page/itemptr.c) | Item pointer (`ItemPointerData`) — `(block_number, offset_within_page)` 6-byte tuple address. The on-disk equivalent of writeonce's `(TypeName, SegmentOffset)`. |
| [`include/storage/bufpage.h`](../../../../reference/postgresql/src/include/storage/bufpage.h) | The header-file definition. Read this first — it's the spec. |
| [`storage/page/README`](../../../../reference/postgresql/src/backend/storage/page/README) | One-page overview of the slotted-page model and how checksums interact with WAL. |

## The Postgres page header (24 bytes)

```text
struct PageHeaderData {
    PageXLogRecPtr  pd_lsn;        // 8 bytes — the LSN that last modified this page
    uint16          pd_checksum;   // 2 bytes — CRC32C over the page (set if data_checksums)
    uint16          pd_flags;      // 2 bytes — has-free-space, etc.
    LocationIndex   pd_lower;      // 2 bytes — offset to start of free space (line ptr end)
    LocationIndex   pd_upper;      // 2 bytes — offset to end of free space (tuple start)
    LocationIndex   pd_special;    // 2 bytes — offset to access-method specific area
    uint16          pd_pagesize_version;  // 2 bytes — page size + layout version
    TransactionId   pd_prune_xid;  // 4 bytes — oldest XID to prune (vacuum hint)
};
```

The body of the page after this header holds **line pointers** (`ItemIdData`, 4 bytes each) growing forward and **tuples** growing backward. The gap between `pd_lower` and `pd_upper` is the free space.

## What's worth porting (eventually)

1. **`pd_lsn` field at the head of every page.** When a page is read back, the LSN tells you "this page reflects WAL records up to LSN N." Recovery can skip records ≤ N for this page (they're already applied). Postgres uses this to avoid double-applying WAL during recovery; writeonce's phase 12+ would too if it goes page-based.
2. **CRC32C trailer/embedded checksum.** Postgres puts it in the header and zeroes the field while computing. Writeonce's phase 10 record framing puts a CRC32C **trailer** (last 4 bytes of the record) — same algorithm, different position. The trailer position is simpler when records are variable-length: the length field in the header tells you exactly where the CRC ends.
3. **Slotted-page line pointers (later).** When phase 12+ wants page-locality for range scans, the slotted-page model — line pointers near the page header, tuples backward from the end — gives O(1) tuple access by index without resizing copies. Postgres' implementation is well-trodden ground.

## What writeonce does instead (phase 10)

Variable-length records, length-prefixed. The framing is in [`docs/plan/10-storage-foundations.md`](../../10-storage-foundations.md):

```text
[u32 length LE][u8 flags][u8 record_kind][u64 LSN][payload bytes][u32 CRC32C]
```

Compared to a Postgres page:

| Concern | Postgres page | Writeonce record |
| --- | --- | --- |
| Granularity | 8 KiB fixed | variable, typical row size |
| Address | `(file, BLCKSZ × block)` | `(type, byte_offset)` |
| LSN | `pd_lsn` in header | embedded after flags |
| Checksum | `pd_checksum` in header | CRC32C trailer |
| Free-space tracking | `pd_lower` / `pd_upper` | none — append-only, segment growth via fallocate |
| Line pointers | yes — relocatable tuples | no — record offset is permanent until tombstoned |

Phase 10 trades range-scan locality for simplicity and append-only commit semantics. The trade is reversible: a future phase can introduce a page layer above the segment without breaking the WAL/recovery contract.

## Why writeonce starts without slotted pages

Postgres' page format earns its complexity:

- **MVCC tuple visibility** needs in-place updates of `xmin/xmax/ctid` on individual tuples — line pointers let one page hold versions across many transactions without rewriting tuples on every `UPDATE`.
- **Free-space recovery** within a page (after a tuple is dead and pruned) is essential when 99% of pages are partly empty.
- **Range queries on a B-tree leaf** want all the keys in one page, sorted, so a 4-KiB read returns dozens of matches.

Phase 10 hits none of these:

- No MVCC yet (`docs/plan/12-engine-disk-cutover.md` defers).
- Append-only segments — a tombstoned record is wasted bytes until compaction, which is fine for the workload.
- Reads go through the in-memory `BTreeMap<i64, SegmentOffset>` index — the segment file isn't scanned linearly; we know exactly where each row lives.

Slotted pages are the answer when those assumptions break. Until then, the framing above is enough.

## Used by

- [`docs/plan/10-storage-foundations.md`](../../10-storage-foundations.md) — record framing borrows the **header + checksum** pattern from `bufpage.h`.
- [`docs/plan/12-engine-disk-cutover.md`](../../12-engine-disk-cutover.md) — when reading rows back from disk, CRC verification is the silent-corruption safety net the page header gives Postgres.

Pair with [`wal.md`](./wal.md) for the LSN convention and [`buffer-and-checkpoint.md`](./buffer-and-checkpoint.md) for the dirty-page semantics that pages need.

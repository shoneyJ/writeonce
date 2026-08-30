# residency — per-table storage

What [databasev2 2](../../stories/databasev2/02-table-storage-modes.md) added:
`durable` and `resident`, declared per `@table` instead of one environment
variable for the whole process.

Before it, `WO_DATA` was the only switch. Set, and every table is WAL-logged;
unset, and none are. Applications are not uniform — a session table is
disposable, a product's stock level is not, and a catalogue large enough to
matter does not fit in RAM at all. One global switch forces "everything is precious" or "nothing
is", and you pay for whichever is wrong.

## Run it

```
just residency
```

The gate writes the example's own output to `/tmp/residency.log`, banner-
separated, so you can `tail -F` it while it runs.

Or by hand, which is the whole demonstration — the same program twice against
one data directory:

```
compiler/_build/default/bin/woc --emit docs/examples/residency/main.wo -o /tmp/residency.wob
mkdir -p /tmp/residency-data          # WO_DATA must exist; wovm will not create it
WO_DATA=/tmp/residency-data runtime/wovm /tmp/residency.wob seed
WO_DATA=/tmp/residency-data runtime/wovm /tmp/residency.wob order
```

```
seeded: products=2 carts=1 SKU-1 stock=10
after restart: products=2 carts=0
order: SKU-1 stock 10 -> 7
ok: first order placed; run `order` again to see it replay
```

The second run inserts nothing. Both products come back from the log; the cart
does not, because it was never written to it. Both tables were filled by the
same code — only the annotation differs, so the difference after the restart is
the annotation's doing and nothing else's.

**Run `order` a third time.** Stock goes 7 → 4, and the example says so: a
level below the seeded 10 can only mean an earlier order's *update* survived a
restart. That is the stronger claim — not just that inserts replay, but that a
field change does.

## The three modes

| Declaration | Meaning | State today |
| --- | --- | --- |
| `durable: true` (default) | WAL-logged, replayed at boot | ✅ works |
| `durable: false` | never written to the log; costs no disk and no fsync; empty after a restart | ✅ works |
| `resident: all` (default) | every row's payload lives in RAM | ✅ works |
| `resident: keys` | the id map stays resident, the payload lives in the WAL and is read back by offset | ✅ works, including update |

## `resident: keys`, and what it costs

`Product` above is declared `resident: keys` — the mode the track exists for. A
catalogue is the table that outgrows RAM first: only the `sku -> row` id map
stays in memory, and each row's payload is read back from the log. Storage, the
read paths, scans (including through the `sku` index, a Text column), `@unique`,
deletes, checkpoint survival, and update all work.

**A read costs one `pread` plus every delta since the row's last checkpoint.**
Updating a keys-resident row has no slab slot to mutate, so it is
read-modify-**append**: `place_order` moving `stock` appends a small delta
record (id, field, new value) chained off the row's previous record, rather
than rewriting `sku`, `name` and `price` to change one integer — the argument
for a delta at all, on the hottest write path a shop has. Reading the row back
folds that chain: the base row plus every delta not yet superseded or
checkpointed away. A row updated once costs a `pread` and one small decode on
top of the base read; a row updated many times between checkpoints costs one
decode per delta still in the chain.

Three limitations ship with this, on purpose documented rather than fixed:

1. **Mid-drain stale reads.** A request reading a row inside the same
   uncommitted drain, while an earlier request in that drain has an in-flight
   update to it, may see the last durable value, not that request's write.
   Read-your-writes holds within a request, not across requests sharing a
   drain. Closing it needs the fold to consult the WAL's staging buffer
   generally, which is materially bigger than this feature.
2. **Replay is O(N²) in a row's delta-chain length.** Each replayed delta
   re-folds the whole chain back to its base record, so boot cost for one long
   chain is quadratic in that chain's length.
3. **Compaction cannot see chain length.** The checkpoint that flattens delta
   chains triggers on the log's overall byte ratio, not on any one row's delta
   count — so a single hot row taking many small updates (a popular SKU,
   exactly this example's workload) can grow a long personal chain without
   moving the aggregate ratio enough to fire a checkpoint. This mode's design
   deliberately does not cap chain length, trusting compaction to bound it
   instead; for a hot-row workload, it may not.

The refusal that used to stand here was earned, not reflexive: an audit before
lifting it found that `delete` on a keys-resident table was reading a WAL byte
offset as a slab index and freeing whatever it landed on — memory corruption,
not a missing feature — fixed and pinned by a test that SEGVs against the old
code. The same audit, repeated before lifting the update refusal, found a
second bug of the same shape: three index functions (and `db.c`'s field-read
and probe paths) were reading a keys-resident row's Text column through the
wrong struct layout, reproduced as a genuine ASan heap-buffer-overflow. Fixed
at the root — a keys-resident row now holds the same engine-encoded values a
`resident: all` row always has — and pinned by a test that reproduces the
overflow against the pre-fix code.

## What this example does NOT show

The mode-mismatch startup refusal, the zero-WAL-bytes measurement, and the two
compile-time refusals are proven by `scripts/residency-accept.sh` against
purpose-built snippets, because each needs a deliberately broken program or a
byte-level assertion on the log file. This example is the readable half.

# residency — per-table storage

What [databasev2 2](../../stories/databasev2/02-table-storage-modes.md) added:
`durable` and `resident`, declared per `@table` instead of one environment
variable for the whole process.

Before it, `WO_DATA` was the only switch. Set, and every table is WAL-logged;
unset, and none are. Applications are not uniform — a session table is
disposable, an orders table is precious, and a 120 GB audit table does not fit
in RAM at all. One global switch forces "everything is precious" or "nothing
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
WO_DATA=/tmp/residency-data runtime/wovm /tmp/residency.wob
```

```
seeded: orders=3 sessions=3
after restart: orders=3 sessions=0
ok: durable replayed, volatile did not
```

The second run inserts nothing. Three orders come back from the log; zero
sessions do, because they were never written to it. Both tables were filled by
the same loop — only the annotation differs, so the difference after the
restart is the annotation's doing and nothing else's.

## The three modes

| Declaration | Meaning | State today |
| --- | --- | --- |
| `durable: true` (default) | WAL-logged, replayed at boot | ✅ works |
| `durable: false` | never written to the log; costs no disk and no fsync; empty after a restart | ✅ works |
| `resident: all` (default) | every row's payload lives in RAM | ✅ works |
| `resident: keys` | the id map stays resident, the payload lives in the WAL and is read back by offset | ⛔ **refused at load** |

## Why `resident: keys` is refused

It is the mode the track exists for — a table larger than RAM. Storage, the
read paths, deletes and checkpoint survival all work. **Updating such a row
does not:** the row has no slab slot to mutate, so a write would land in a
scratch buffer and be discarded *silently*. Doing it properly is
read-modify-append.

So the loader refuses the annotation rather than honouring it in name only.
Uncomment the `AuditEntry` block in `main.wo` and you get:

```
wovm: class 0 declares `resident: keys`, which is INCOMPLETE: rows are stored
and read keys-only, but UPDATING one is not implemented (it needs
read-modify-append). Remove it until databasev2 2 lands updates;
`resident: all` is what runs
```

Note **where** that comes from: `woc` compiles it happily and emits a `.wob`.
The annotation is a load-time property, so the compiler is green and `wovm`
exits 2.

Refusing at load rather than at the first update is deliberate. A developer who
declared a 120 GB table keys-resident, saw it compile, and shipped would find
the gap in production. That judgement earned its keep in a way nobody had
written down: an audit before relaxing the refusal found that `delete` on such
a table was reading a WAL byte offset as a slab index and freeing whatever it
landed on — memory corruption, not a missing feature. It is fixed and pinned by
a test that SEGVs against the old code, but the refusal is what stood in front
of it.

## What this example does NOT show

The mode-mismatch startup refusal, the zero-WAL-bytes measurement, and the two
compile-time refusals are proven by `scripts/residency-accept.sh` against
purpose-built snippets, because each needs a deliberately broken program or a
byte-level assertion on the log file. This example is the readable half.

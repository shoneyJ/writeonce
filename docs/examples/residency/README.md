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
| `resident: keys` | the id map stays resident, the payload lives in the WAL and is read back by offset | ⛔ **refused at load** |

## Why `resident: keys` is refused — and why this example is the argument

It is the mode the track exists for: a catalogue is the table that outgrows RAM
first, so `Product` is exactly what you would want to declare keys-resident.
Storage, the read paths, scans, `@unique`, deletes and checkpoint survival all
work. **Updating such a row does not**, and `place_order` is precisely why that
matters — the row has no slab slot to mutate, so the write would land in a
materialised scratch buffer and be discarded *silently*.

The shape of the fix follows from the same example. When an order is placed
only `stock` changes; `sku`, `name` and `price` do not. Appending the whole row
per sale would rewrite every field to move one integer, on the hottest write
path a shop has — which is the argument for appending a **delta** (id, field,
new value) and folding it on read, with the existing checkpoint doing the fold
that keeps delta chains short. That design is being settled now; the loader
refusal stands until it lands.

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

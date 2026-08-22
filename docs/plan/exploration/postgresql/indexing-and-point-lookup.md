# Indexing & point lookup — how Postgres never full-scans for `=`

The access-method algorithms, and the mechanism that turns an equality
predicate into a direct row address instead of a table walk. Tree:
19devel; paths into
[`reference/postgresql/`](../../../../.dev/reference/postgresql/).
Written for the read-path finding iteration 22 measured: writeonce
point lookups are O(table) (~1.5k reads/s at p50 600µs on 20k rows).

## The access-method roster (facts, with paths)

| AM | algorithm | serves | point lookup |
| --- | --- | --- | --- |
| nbtree | Lehman–Yao B-tree (`access/nbtree/README:6`) | `< <= = >= >`, IN, ordered scans, prefix LIKE | O(log N) page descents |
| hash | Seltzer/Yigit extendible hashing (`access/hash/README:6`) | `=` only | O(1) expected: metapage + bucket-page binary search |
| gin | inverted index: btree of keys → posting lists (`access/gin/README:17`) | containment, full-text | bitmap-only (no amgettuple) |
| gist | generalized balanced tree, opclass `consistent` (`access/gist/README:8`) | overlap, kNN | multi-subtree descent |
| spgist | space-partitioned tries/quadtrees, non-balanced (`access/spgist/README:3`) | points, prefixes | data-bounded depth |
| brin | per-block-range min/max summaries (`access/brin/README:4`) | huge clustered scans | none — lossy bitmap |

Algorithm notes worth keeping:

- **btree**: Lehman–Yao's right-link + high-key lets descents run
  lock-free past concurrent splits (`nbtree/README:17-29`); equality
  and range use the SAME descent — `_bt_first` positions, `_bt_next`
  walks siblings (`nbtsearch.c:883/:1586`); heap TID is a tiebreaker
  making every key unique per level.
- **hash**: bucket count doubles at split points; one bucket splits at
  a time (linear hashing, `hash/README:14-22,60-79`); bucket resolution
  is a MASK — `bucket = hash & highmask; if > maxbucket then & lowmask`
  (`hashutil.c:125`); entries sorted by hash within a page for binary
  search. Fully WAL-logged in this tree (the old caveat is gone);
  btree's remaining edge is capability, not durability: only btree does
  unique constraints, ordered scans, and range predicates.

## Why `=` never scans the table

1. The planner builds BOTH paths and costs them: seqscan cost is
   unconditionally whole-relation (`pages × seq_page_cost + tuples ×
   cpu_tuple_cost`, `costsize.c:270`); index cost scales by
   SELECTIVITY (`cost_index`, `:545`, delegating to the AM's
   `amcostestimate`). A selective equality wins by arithmetic, not by
   rule.
2. An index entry stores a **TID** — `(block, offset)`, 6 bytes
   (`storage/itemptr.h:36`): the ROW'S ADDRESS. The executor path is
   IndexScan → `btgettuple` → `index_fetch_heap` reads exactly ONE
   heap page and one line pointer (`indexam.c:698`). The index answers
   "where", the heap answers "what" — nothing walks.

## The writeonce translation — O(1) lookups are one wiring change

What exists (`database/src/table.c`):

- The id path is ALREADY the TID story: `hget` (open-addressing hash,
  :260) maps id → global slot + 1, and the slot IS the address
  (slab base + offset — addresses stable forever). O(1), proven by
  db-bench's 297k inserts/s.
- Secondary indexes ALREADY exist as a hash multimap —
  `db_index`/`db_ibucket` (`idx_hash`/`idx_bucket`, :302/:342), the
  same expected-O(1) shape as Postgres' hash AM (minus its paging,
  which a RAM-authoritative store does not need). Maintained inside
  the insert/remove/update choke points, exactly where they belong.

The measured gap: **the read path never asks the index.** Both
`WO_B_DB_PROBE` (`db.c:125`) and its RPC twin in `wo_db_exec_req` read
the index metadata only for the key column's KIND, then walk EVERY
slab comparing values — Postgres' seqscan, unconditionally, on a
column that has a live hash index. `wo_row_has_referrers` (FK
restrict) is the same story across all tables.

Direction the next slice takes (words only):

1. Probe = `idx_bucket(ix, hash(key))`, then verify equality against
   the bucket's ids via `wo_row_ptr` (a hash is a hint, never an
   answer — the engine's own doctrine, already enforced on the unique
   path). Expected O(1); the bucket wins by the same arithmetic that
   makes Postgres pick the index.
2. Multi-column indexes probe on the FULL column set today
   (`idx_hash` hashes all cols) — a single-column equality over a
   composite index needs either a leading-column bucket layout or a
   declared single-column index; the slice decides, the bench arbitrates.
3. FK restrict + backlink reads ride the same probe once the
   grammar card's rule lands (backlink implies the FK-column index).
4. Non-goals, recorded: no btree (no ordered-scan workload yet —
   `order by` sorts materialized results today), no planner (one AM,
   one rule: indexed equality probes, everything else scans), no
   paging (RAM-authoritative; the WAL is the disk story).

Acceptance shape for that slice: db-bench `read`/`query` move from
~1.5k ops/s to the same order as inserts; `bench/baseline.json`
refreshed with the delta recorded — the gate exists precisely so this
claim gets measured.

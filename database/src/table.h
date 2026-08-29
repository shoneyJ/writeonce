/* table.h — class-shaped row storage (iteration 9, Task 1).
 *
 * The engine and the VM heap are two memory worlds crossed only by copy
 * (the 9b design's section 6): a row stores NO VM pointer. Every field
 * lands in one 8-byte slot, kind-driven:
 *
 *   SCALAR  the 8 bytes themselves (WO_NIL_SCALAR spells a ?scalar's nil)
 *   TEXT    engine-owned db_text*  (0 = nil)
 *   OWNED   engine-owned db_rec*   — the object flattened by value,
 *           recursively, through these same rules (0 = nil)
 *   MULTI   engine-owned db_multi* — elements encoded element-wise
 *   MAP     engine-owned db_map*   — keys and values encoded pair-wise
 *   GCREF   never stored: the compiler rejects it (the GC bulkhead);
 *           the engine refuses it defensively as an encode error
 *
 * `ref T` is a SCALAR at this layer — the target row's id, an ordinary
 * number the compiler produced; the engine learns nothing about it until
 * the FK checks (9b plan, Task 3).
 *
 * Row layout: a 16-byte header (id, class, flags) then field_cnt 8-byte
 * slots — deliberately the VM object layout's shape, so encode/decode walk
 * the same class-table kinds the VM walks. Rows live in per-class SLABS
 * (fixed-count, malloc'd, never moved: a row's address is stable for its
 * lifetime, which is what lets 9b hand out loop-scoped row views). A
 * per-table bitmap tracks occupancy; removed slots go on a free list and
 * are reused before any slab grows. The id->row map is an open-addressing
 * hash owned by the table.
 *
 * Id discipline (the c-runtime plan's shipped behavior): per table, per
 * shard, ids interleave — shard S of N allocates S+1, S+1+N, S+1+2N, … —
 * so creation is coordination-free and a row's owner shard is (id-1) % N.
 * Milestone runs at N=1 (iteration 8 not yet landed); everything here is
 * N-parametric and degenerates cleanly.
 *
 * CHOKE POINT DOCTRINE: wo_row_insert / wo_row_remove are the only paths
 * that touch storage. Task 4's secondary indexes hook exactly these two
 * functions; anything else mutating a slab is a defect by definition.
 */
#ifndef WO_TABLE_H
#define WO_TABLE_H

#include "obj.h" /* wo_rt, wo_classdesc, kinds, wo_str, containers */

/* ---- engine-owned value shapes (all malloc'd, all reachable only from
 * row slots, all freed through db_val_free) ---- */

typedef struct db_text {
    uint32_t len;
    char bytes[]; /* len bytes, no NUL */
} db_text;

typedef struct db_rec { /* an owned object flattened by value */
    uint32_t class_id;  /* index into the SAME class table the VM uses */
    uint32_t _pad;
    uint64_t slots[]; /* field_cnt slots, encoded by these rules */
} db_rec;

typedef struct db_multi {
    uint8_t elem_kind;
    uint32_t len;
    uint64_t items[];
} db_multi;

typedef struct db_map {
    uint8_t key_kind, val_kind;
    uint32_t len;
    uint64_t kv[]; /* len pairs: k0 v0 k1 v1 … */
} db_map;

/* ---- rows and tables ---- */

typedef struct db_row {
    uint64_t id;
    uint32_t class_id;
    uint32_t flags; /* reserved (0) */
    uint64_t slots[];
} db_row;

#define DB_SLAB_ROWS 256u

/* Secondary index (iteration 9, Task 4): built from the class table's v3
 * metadata at first touch, maintained ONLY inside the row choke points.
 * Hash multimap: bucket per column-value hash, ids within; equality is
 * re-checked against the actual rows on the unique path (a hash is a hint,
 * never an answer). */
typedef struct db_ibucket {
    uint64_t hash;
    uint64_t *ids;
    uint32_t len, cap;
} db_ibucket;

typedef struct db_index {
    uint32_t flags; /* bit0 = unique */
    uint32_t col_cnt;
    const uint32_t *cols; /* into the loader's idx pool */
    db_ibucket *buckets;  /* open addressing by hash; hash==0 stored as 1 */
    size_t bcap, blen;
} db_index;

/* wo_row_insert failure classes — *msg carries the sentence, this carries
 * the machine-readable kind so db.c maps to the right trap. */
enum { DB_ERR_NONE = 0, DB_ERR_OOM = 1, DB_ERR_BADKIND = 2, DB_ERR_UNIQUE = 3, DB_ERR_MISC = 4 };

typedef struct db_table {
    uint32_t class_id;
    size_t row_size; /* 16 + field_cnt * 8 */
    /* slabs of DB_SLAB_ROWS rows each; addresses stable forever */
    uint8_t **slabs;
    uint32_t slab_cnt, slab_cap;
    uint64_t *bitmap; /* one bit per slot, slab-major */
    /* removed slots, reused LIFO before any slab grows */
    uint32_t *free_slots;
    uint32_t free_cnt, free_cap;
    uint64_t next_id; /* next id THIS shard hands out for this table */
    uint64_t count;   /* live rows */
    /* id -> (global slot + 1); 0 = empty. Open addressing, pow2. */
    uint64_t *hkeys;
    uint64_t *hvals;
    size_t hcap, hlen;
    /* secondary indexes, from the class table's v3 metadata */
    db_index *indexes;
    uint32_t index_cnt;
    /* databasev2 2: one reusable materialisation buffer per table, for
     * wo_row_borrow. Per-TABLE and not per-call because the unique shadow
     * check borrows once per candidate inside a bucket loop, and per-call
     * allocation would turn an O(1) probe into an allocation storm. Safe
     * because the store is single-writer (the owner shard) and a borrow is
     * never nested — `busy` exists to catch it if that ever stops being
     * true, rather than aliasing silently. */
    uint8_t *scratch;
    size_t scratch_cap;
    int scratch_busy;
} db_table;

typedef struct wo_db {
    /* databasev2 2 (5c): the runtime this store belongs to, so a borrow can
     * reach the WAL. wo_rt already carries `db` and `wal` as opaque handles,
     * so this closes the loop without threading a wal pointer through
     * wo_row_borrow's eleven call sites — which is the whole reason 5c is one
     * accessor rather than eleven rewrites. NULL in test binaries and with
     * durability off; a `resident: keys` table cannot exist in either case,
     * because it has no log to read rows back from. */
    wo_rt *rt;
    const wo_classdesc *classes;
    uint32_t class_cnt;
    uint32_t shard, nshards; /* S of N; ids interleave S+1, S+1+N, … */
    db_table *tables;        /* class_cnt entries, created lazily on first insert */
} wo_db;

/* 0 ok, -1 alloc failure. nshards >= 1, shard < nshards. */
int wo_db_init(wo_db *db, const wo_classdesc *classes, uint32_t class_cnt,
               uint32_t shard, uint32_t nshards);
void wo_db_destroy(wo_db *db);

/* Insert: encode field_cnt VM values (register words, kinds from the class
 * table) into a fresh row. Returns the new id, or 0 with *msg set (OOM, or
 * a GCREF field — which the compiler should have refused upstream). */
uint64_t wo_row_insert(wo_db *db, uint32_t class_id, const uint64_t *vals,
                       const char **msg, int *err_kind);

/* Read: decode the row's fields into VM values freshly allocated from
 * [rt] — always copies, never a pointer into the slab (the out-gate).
 * 0 ok, -1 no such row, -2 OOM (*msg set). */
int wo_row_read(wo_db *db, wo_rt *rt, uint32_t class_id, uint64_t id,
                uint64_t *out_vals, const char **msg);

/* Remove: free the row's engine-owned field values, clear the slot, recycle
 * it. 0 ok, -1 no such row. */
int wo_row_remove(wo_db *db, uint32_t class_id, uint64_t id);

/* databasev2 2 (5c): drop a row's PAYLOAD while keeping it live.
 *
 * The operation the plan recorded as missing. For a `resident: keys` table the
 * row's bytes live in the log, not in a slab: this frees the slot and its
 * engine-owned values, then re-points the id map at [wal_off] (stored as
 * off + 1, reusing the same 0-is-empty trick the slot encoding uses — a table
 * is wholly `all` or wholly `keys`, so the interpretation is per-table and
 * never ambiguous).
 *
 * What it deliberately does NOT do, and why:
 *   - it does not touch the secondary indexes. They store row IDS, not slots
 *     (see db_ibucket), so they are already indirect through the id map and
 *     stay correct across this.
 *   - it does not decrement `count`. The row is still LIVE; only its backing
 *     moved.
 *   - it does not remove the id. The id is how the row is found afterwards.
 *
 * [wal_off] must be the offset of a record whose commit succeeded. Since
 * databasev2 4 made a failed commit fatal, no execution can reach here with an
 * offset that never became durable — which is what wo_wal_next_offset's
 * contract asks for, now guaranteed by process death rather than by an inline
 * check the deferred barrier no longer allows.
 *
 * 0 ok, -1 unknown class/row. */
int wo_row_drop_payload(wo_db *db, uint32_t class_id, uint64_t id, uint64_t wal_off);
int wo_row_set_offset(wo_db *db, uint32_t class_id, uint64_t id, uint64_t wal_off);
uint64_t wo_row_offset1(const wo_db *db, uint32_t class_id, uint64_t id);

/* databasev2 2 (5d): iterate the live row IDS of a table, whichever backing it
 * has. [*cursor] starts at 0 and is opaque; returns 1 with *id_out set, or 0
 * when exhausted.
 *
 * A keys-resident table has an EMPTY bitmap by construction — its payloads live
 * in the log — so every bitmap walk in the engine would silently see no rows.
 * This is the one primitive those walks move onto.
 *
 * Resident tables keep walking the bitmap, deliberately: the id map holds the
 * same set, but in hash order, and switching would reorder the results of every
 * unordered query in the repo. Two backings, one interface, no behaviour change
 * where nothing needed to change. */
int wo_row_next_id(const wo_db *db, uint32_t class_id, size_t *cursor, uint64_t *id_out);

/* databasev2 2 (5c): is this table's row data in the log rather than in slabs? */
int wo_table_is_keys_resident(const wo_db *db, uint32_t class_id);

/* iteration 9b FK restrict: 1 if some row in some class holds a non-nullable
 * `ref` to [class_id] equal to [id] — i.e. deleting this row would dangle a
 * reference. The compiler records a ref field's target class in the class
 * table's field_class metadata; this scans those columns. Correctness-first
 * (a full scan of referencing tables); the backlink index is the later
 * optimization the spec records. */
int wo_row_has_referrers(wo_db *db, uint32_t class_id, uint64_t id);

/* Update one field in place (iteration 9 Task 5): encode the VM value,
 * swap it into the slot, keep every index containing that column honest —
 * remove-old/add-new with the unique re-check running BEFORE anything
 * mutates, so a violating update leaves the row untouched. 0 ok, -1 no
 * such row / bad field, DB_ERR_* codes via *err_kind like insert. */
int wo_row_update_field(wo_db *db, uint32_t class_id, uint64_t id, uint32_t field,
                        uint64_t vm_val, const char **msg, int *err_kind);

/* Borrowed row pointer for engine-internal callers (the WAL writes a row's
 * encoded bytes; indexes read key slots). NULL = no such row. NEVER handed
 * to the VM. */
db_row *wo_row_ptr(wo_db *db, uint32_t class_id, uint64_t id);

/* ---- databasev2 2: the shared row accessor -------------------------------
 *
 * Every reader that today does `wo_row_ptr` and then touches `r->slots[...]`
 * uses this pair instead, so ONE code path serves both residencies:
 *
 *   resident: all   borrow returns the slab pointer; release is a no-op
 *   resident: keys  borrow materialises the record from its log offset into
 *                   the table's scratch; release frees what it built
 *
 * Landed as a PURE REFACTOR: until the offset storage exists, borrow is
 * wo_row_ptr plus a branch and every release is a no-op. Deliberate — the
 * refactor is provable on its own, before the storage change it enables.
 *
 * A borrowed row is READ-ONLY when it is materialised: it is a copy, so
 * writing to it changes nothing durable. Mutation still goes through the row
 * choke points. Pair EVERY non-NULL borrow with a release, and never nest two
 * borrows on the same table — they would share one scratch. */
db_row *wo_row_borrow(wo_db *db, uint32_t class_id, uint64_t id, const char **msg);
void wo_row_release(wo_db *db, uint32_t class_id, db_row *r);

/* Engine-internal, for WAL replay only: create a row with a FIXED id,
 * slots zeroed — the caller (wal.c) fills them with engine-encoded values
 * it built while decoding. Advances the table's next_id past [id] when the
 * id belongs to this shard, so post-replay inserts never collide. NULL =
 * OOM or duplicate id (corruption beyond a torn tail). */
db_row *wo_row_create_raw(wo_db *db, uint32_t class_id, uint64_t id);

/* Engine-internal: free one engine-encoded slot value of [kind] (wal.c's
 * decode error paths). */
void wo_db_val_free(wo_db *db, uint8_t kind, uint64_t v);

/* Decode one engine slot value to a FRESH VM value in [rt] (the out-gate:
 * always a copy). The query builtins' field reads go through this. */
uint64_t wo_val_decode_vm(wo_db *db, wo_rt *rt, uint8_t kind, uint64_t engine_val,
                          int *ok, const char **msg);

/* Read-path index probe (the O(1) wiring): answer a SINGLE-COLUMN
 * equality from the index's hash buckets instead of walking slabs.
 * Key representation is caller-neutral so wo_str and db_text callers
 * both fit: a Text key passes its bytes+len (bytes == NULL means nil;
 * an empty text is a non-NULL pointer with len 0); any scalar/float
 * key passes the raw word in [key_scalar] (bytes ignored). The bucket
 * hash canonicalizes floats exactly as index maintenance does; the
 * VERIFY step then compares exactly as the slab walk compares (raw
 * words for scalars/floats, byte equality for text) — a hash is a
 * hint, never an answer, so results are identical to the scan.
 * Returns 1 = probed (*out_ids is a malloc'd id list of *out_cnt,
 * possibly NULL/0 — the caller frees), 0 = cannot probe (unknown
 * class/index, untouched table, or a multi-column index — the caller
 * keeps its scan fallback), -1 = OOM. */
int wo_idx_probe(wo_db *db, uint32_t class_id, uint32_t index, uint64_t key_scalar,
                 const void *key_bytes, uint32_t key_len, uint64_t **out_ids,
                 uint32_t *out_cnt);

/* Engine-internal, replay only: after wal.c fills a raw row's slots, this
 * runs the index maintenance the normal insert runs inline — including the
 * unique check, whose violation during replay is corruption, not data
 * (0 ok, -1). */
int wo_row_raw_commit(wo_db *db, uint32_t class_id, db_row *r);

/* ---- arc stage 3: the slot-level surface the transparent DB RPC uses ----
 * VM heaps are never read cross-shard (a worker's GC writes header mark
 * bits concurrently), so the REQUESTER shard encodes its VM values into
 * engine-owned slots on its own thread and ships those; the OWNER shard
 * executes from slots. Everything here is thread-agnostic: it touches only
 * the wo_db it is handed and engine-owned mallocs. */

/* Encode one VM value into an engine slot on the caller's thread (the
 * in-gate, split out of wo_row_insert for the RPC path). */
uint64_t wo_db_val_encode(const wo_classdesc *classes, uint8_t kind, uint64_t vm_val,
                          int *ok, const char **msg);

/* Deep-copy one engine value — a GET_FIELD reply must outlive the row it
 * was read from (a later statement may free the row's slot). */
uint64_t wo_db_val_clone(const wo_classdesc *classes, uint8_t kind, uint64_t v, int *ok);

/* Insert from PRE-ENCODED slots (field_cnt of them). Ownership of the slot
 * VALUES transfers: installed on success, freed on failure. New id, or 0
 * with *msg / *err_kind set exactly as wo_row_insert sets them. */
uint64_t wo_row_insert_slots(wo_db *db, uint32_t class_id, const uint64_t *slots,
                             const char **msg, int *err_kind);

/* Update one field from a PRE-ENCODED slot value (consumed either way:
 * installed on success, freed on failure). Same contract as
 * wo_row_update_field after its encode. */
int wo_row_update_field_slot(wo_db *db, uint32_t class_id, uint64_t id, uint32_t field,
                             uint64_t slot, const char **msg, int *err_kind);

#endif /* WO_TABLE_H */

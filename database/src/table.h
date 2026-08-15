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
} db_table;

typedef struct wo_db {
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
                       const char **msg);

/* Read: decode the row's fields into VM values freshly allocated from
 * [rt] — always copies, never a pointer into the slab (the out-gate).
 * 0 ok, -1 no such row, -2 OOM (*msg set). */
int wo_row_read(wo_db *db, wo_rt *rt, uint32_t class_id, uint64_t id,
                uint64_t *out_vals, const char **msg);

/* Remove: free the row's engine-owned field values, clear the slot, recycle
 * it. 0 ok, -1 no such row. */
int wo_row_remove(wo_db *db, uint32_t class_id, uint64_t id);

/* Borrowed row pointer for engine-internal callers (the WAL writes a row's
 * encoded bytes; indexes read key slots). NULL = no such row. NEVER handed
 * to the VM. */
db_row *wo_row_ptr(wo_db *db, uint32_t class_id, uint64_t id);

/* Engine-internal, for WAL replay only: create a row with a FIXED id,
 * slots zeroed — the caller (wal.c) fills them with engine-encoded values
 * it built while decoding. Advances the table's next_id past [id] when the
 * id belongs to this shard, so post-replay inserts never collide. NULL =
 * OOM or duplicate id (corruption beyond a torn tail). */
db_row *wo_row_create_raw(wo_db *db, uint32_t class_id, uint64_t id);

/* Engine-internal: free one engine-encoded slot value of [kind] (wal.c's
 * decode error paths). */
void wo_db_val_free(wo_db *db, uint8_t kind, uint64_t v);

#endif /* WO_TABLE_H */

#include "db.h"

#include <stdlib.h>
#include <string.h>

#include "cont.h"
#include "table.h"
#include "wal.h"

/* databasev2 2: is this table's storage durable? A `@table(durable: false)`
 * class carries WO_CLASSF_VOLATILE and is never staged to the WAL — no
 * record, no fsync, ack straight from RAM. One predicate for all three
 * mutation sites below: `database/src/CODE-LOGIC.md` names those as the only
 * places storage may be staged, and that invariant is worth more than the
 * convenience of inlining this. cid is always loader-validated by the time a
 * mutation has succeeded, so no bounds check is added here. */
static int table_is_durable(const wo_db *db, uint32_t cid) {
    return (db->classes[cid].flags & WO_CLASSF_VOLATILE) == 0u;
}

/* databasev2 3: the inline path's compaction check.
 *
 * The drain has its own (vm.c, after the barrier). This one exists because a
 * statement running ON the owner shard never enters that drain, so without it
 * a single-shard durable program's log grows FOREVER — measured: WO_SHARDS=1
 * reached 536 KB where the multi-shard run held 446 KB, because the check was
 * only wired into the drain.
 *
 * Safe here for the same reason it is safe there: the commit above just
 * emptied the staging buffer. The result is ignored because a failed
 * compaction is a missed optimisation, not a durability event. */
static void maybe_compact(wo_db *db, wo_wal *w) {
    if (wo_wal_should_compact(w->off, w->compacted_bytes, wo_wal_ckpt_floor,
                              wo_wal_ckpt_ratio))
        (void)wo_wal_compact(w, db);
}

int wo_builtin_db(wo_vm *vm, uint64_t *R, uint32_t ins, const char **msg) {
    uint32_t A = wo_ins_a(ins), B = wo_ins_b(ins), C = wo_ins_c(ins);
    wo_db *db = (wo_db *)vm->rt.db;
    if (!db) {
        *msg = "database engine not initialized";
        return WO_T_DB;
    }
    switch (C) {
    case WO_B_DB_INSERT: {
        uint32_t cid = (uint32_t)R[B];
        int ek = 0;
        uint64_t id = wo_row_insert(db, cid, &R[B + 1], msg, &ek);
        if (!id)
            return ek == DB_ERR_UNIQUE ? WO_T_UNIQUE
                   : ek == DB_ERR_OOM  ? WO_T_OOM
                                       : WO_T_DB;
        wo_wal *w = (wo_wal *)vm->rt.wal;
        if (w && table_is_durable(db, cid)) {
            /* THE INLINE PATH KEEPS ITS OWN BARRIER, AND THAT ASYMMETRY IS
             * DELIBERATE (databasev2 4 part A). The request path batches:
             * wo_vm_adopt holds each reply and commits once per drain. This
             * path cannot, because it has no reply to hold — it returns into
             * its OWN fiber rather than unparking a requester. Do not "fix"
             * this by dropping the commit: without it an inline statement
             * would never be durable at all.
             *
             * Committing here is safe because the drain commits
             * unconditionally whenever anything is staged, so the buffer is
             * empty when this runs.
             *
             * The `table_is_durable` guard is databasev2 2's: a
             * `@table(durable: false)` class is never staged, so it reaches
             * neither this barrier nor the compaction check below.
             *
             * Failure is fatal, not a trap: the row is already in RAM. */
            /* databasev2 2 (5c): the offset this record WILL occupy. Taken
             * BEFORE the append, recorded as pending, and acted on only after
             * the commit below — a keys-resident payload dropped any earlier
             * would leave an offset whose bytes are still in the staging
             * buffer. */
            uint64_t koff = wo_wal_next_offset(w);
            if (wo_wal_append_insert(w, db, cid, id) != 0) wo_wal_stage_fatal(w);
            if (wo_table_is_keys_resident(db, cid)) (void)wo_wal_pend_drop(w, cid, id, koff);
            wo_wal_commit_fatal(w, 1);
            wo_db_flush_drops(db, w);
            maybe_compact(db, w);
        }
        R[A] = id;
        return 0;
    }
    case WO_B_DB_UPDATE_FIELD: {
        uint32_t cid = (uint32_t)R[B];
        uint64_t id = R[B + 1];
        uint32_t field = (uint32_t)R[B + 2];
        int ek = 0;
        if (wo_row_update_field(db, cid, id, field, R[B + 3], msg, &ek) != 0)
            return ek == DB_ERR_UNIQUE ? WO_T_UNIQUE : ek == DB_ERR_OOM ? WO_T_OOM : WO_T_DB;
        wo_wal *w = (wo_wal *)vm->rt.wal;
        if (w && table_is_durable(db, cid)) {
            /* was: trap and leave RAM ahead of disk, which the old comment
             * admitted. Now fatal — see the insert arm. */
            if (wo_wal_append_update(w, db, cid, id) != 0) wo_wal_stage_fatal(w);
            wo_wal_commit_fatal(w, 1);
            maybe_compact(db, w);
        }
        R[A] = 0;
        return 0;
    }
    case WO_B_DB_DELETE: {
        uint32_t cid = (uint32_t)R[B];
        uint64_t id = R[B + 1];
        /* FK restrict: refuse if another row still references this one
           (iteration 9b) — nothing is removed, the statement traps */
        if (wo_row_has_referrers(db, cid, id)) {
            *msg = "row is still referenced (restrict)";
            return WO_T_FK;
        }
        if (wo_row_remove(db, cid, id) != 0) {
            *msg = "no such row";
            return WO_T_DB;
        }
        wo_wal *w = (wo_wal *)vm->rt.wal;
        if (w && table_is_durable(db, cid)) {
            if (wo_wal_append_remove(w, cid, id) != 0) wo_wal_stage_fatal(w);
            wo_wal_commit_fatal(w, 1);
            maybe_compact(db, w);
        }
        R[A] = 0;
        return 0;
    }
    case WO_B_DB_SCAN: {
        uint32_t cid = (uint32_t)R[B];
        if (cid >= db->class_cnt) {
            *msg = "no such class";
            return WO_T_DB;
        }
        wo_multi *ids = wo_multi_new(&vm->rt, WO_K_SCALAR);
        if (!ids) return WO_T_OOM;
        /* materialize the id list up front — the 9b cursor-stability rule:
         * the loop body then point-reads each id, so a row updated mid-loop
         * (even an indexed column) cannot disturb the iteration */
        db_table *t = &db->tables[cid];
        if (t->row_size) {
            uint32_t total = t->slab_cnt * DB_SLAB_ROWS;
            for (uint32_t g = 0; g < total; g++) {
                if (!(t->bitmap[g >> 6] & (1ull << (g & 63)))) continue;
                db_row *row =
                    (db_row *)(t->slabs[g / DB_SLAB_ROWS] + (size_t)(g % DB_SLAB_ROWS) * t->row_size);
                if (wo_multi_push(ids, row->id) != 0) return WO_T_OOM;
            }
        }
        R[A] = (uint64_t)(uintptr_t)ids;
        return 0;
    }
    case WO_B_DB_GET_FIELD: {
        uint32_t cid = (uint32_t)R[B];
        uint64_t id = R[B + 1];
        uint32_t field = (uint32_t)R[B + 2];
        if (cid >= db->class_cnt || field >= db->classes[cid].field_cnt) {
            *msg = "no such field";
            return WO_T_DB;
        }
        db_row *row = wo_row_ptr(db, cid, id);
        if (!row) {
            *msg = "no such row";
            return WO_T_DB;
        }
        int ok = 1;
        uint64_t v = wo_val_decode_vm(db, &vm->rt, db->classes[cid].kinds[field],
                                      row->slots[field], &ok, msg);
        if (!ok) return WO_T_OOM;
        R[A] = v;
        return 0;
    }
    case WO_B_DB_PROBE: {
        uint32_t cid = (uint32_t)R[B];
        uint32_t index = (uint32_t)R[B + 1];
        if (cid >= db->class_cnt) {
            *msg = "no such class";
            return WO_T_DB;
        }
        wo_multi *ids = wo_multi_new(&vm->rt, WO_K_SCALAR);
        if (!ids) return WO_T_OOM;
        db_table *t = &db->tables[cid];
        if (t->row_size && index < t->index_cnt) {
            db_index *ix = &t->indexes[index];
            uint32_t col = ix->cols[0];
            uint8_t kind = db->classes[cid].kinds[col];
            uint64_t key = R[B + 2];
            {
                /* the O(1) path: single-column equality answers from the
                 * index buckets; the slab walk below stays the composite
                 * fallback (wo_idx_probe verifies exactly as it compares) */
                const void *kb = NULL;
                uint32_t kl = 0;
                if (kind == WO_K_TEXT && key) {
                    const wo_str *s = (const wo_str *)(uintptr_t)key;
                    kb = s->data;
                    kl = s->len;
                }
                uint64_t *hit = NULL;
                uint32_t hn = 0;
                int prc = wo_idx_probe(db, cid, index, key, kb, kl, &hit, &hn);
                if (prc < 0) return WO_T_OOM;
                if (prc == 1) {
                    for (uint32_t i = 0; i < hn; i++)
                        if (wo_multi_push(ids, hit[i]) != 0) {
                            free(hit);
                            return WO_T_OOM;
                        }
                    free(hit);
                    R[A] = (uint64_t)(uintptr_t)ids;
                    return 0;
                }
            }
            uint32_t total = t->slab_cnt * DB_SLAB_ROWS;
            for (uint32_t g = 0; g < total; g++) {
                if (!(t->bitmap[g >> 6] & (1ull << (g & 63)))) continue;
                db_row *row =
                    (db_row *)(t->slabs[g / DB_SLAB_ROWS] + (size_t)(g % DB_SLAB_ROWS) * t->row_size);
                int eq;
                if (kind == WO_K_TEXT) {
                    const wo_str *want = (const wo_str *)(uintptr_t)key;
                    const db_text *have = (const db_text *)(uintptr_t)row->slots[col];
                    eq = (!want && !have) ||
                         (want && have && want->len == have->len &&
                          memcmp(want->data, have->bytes, have->len) == 0);
                } else
                    eq = row->slots[col] == key;
                if (eq && wo_multi_push(ids, row->id) != 0) return WO_T_OOM;
            }
        }
        R[A] = (uint64_t)(uintptr_t)ids;
        return 0;
    }
    default:
        *msg = "unknown db builtin";
        return WO_T_DB;
    }
}

/* ---- arc stage 3: the owner-shard executor ------------------------------
 * Mirrors the switch above case for case, with slot inputs and plain
 * outputs — every trap code and message a worker sees is byte-identical to
 * what the same statement would produce on the primary. */
void wo_db_exec_req(wo_vm *vm, wo_db_req *q) {
    wo_db *db = (wo_db *)vm->rt.db;
    wo_wal *w = (wo_wal *)vm->rt.wal;
    const char *m = "db failed";
    q->status = 0;
    q->msg = "";
    if (!db) {
        q->status = WO_T_DB;
        q->msg = "database engine not initialized";
        goto out;
    }
    switch (q->op) {
    case WO_B_DB_INSERT: {
        int ek = 0;
        uint64_t id = wo_row_insert_slots(db, q->cid, q->slots, &m, &ek);
        if (!id) {
            q->status = ek == DB_ERR_UNIQUE ? WO_T_UNIQUE
                        : ek == DB_ERR_OOM  ? WO_T_OOM
                                            : WO_T_DB;
            q->msg = m;
            break;
        }
        if (w && table_is_durable(db, q->cid)) {
            /* databasev2 4: staging failure is FATAL, not a trap. The row is
             * already in RAM; of the three verbs only insert could undo
             * itself, so continuing means RAM ahead of disk. One rule: once a
             * statement has mutated RAM, the outcomes are durable or death. */
            uint64_t koff = wo_wal_next_offset(w);
            if (wo_wal_append_insert(w, db, q->cid, id) != 0) wo_wal_stage_fatal(w);
            /* recorded, not performed: this batch's barrier runs in the drain
             * (vm.c), and only then are these offsets readable */
            if (wo_table_is_keys_resident(db, q->cid))
                (void)wo_wal_pend_drop(w, q->cid, id, koff);
        }
        q->result = id;
        break;
    }
    case WO_B_DB_UPDATE_FIELD: {
        int ek = 0;
        if (wo_row_update_field_slot(db, q->cid, q->id, q->field, q->slots[0], &m, &ek) != 0) {
            q->status = ek == DB_ERR_UNIQUE ? WO_T_UNIQUE : ek == DB_ERR_OOM ? WO_T_OOM : WO_T_DB;
            q->msg = m;
            break;
        }
        if (w && table_is_durable(db, q->cid)) {
            if (wo_wal_append_update(w, db, q->cid, q->id) != 0) wo_wal_stage_fatal(w);
        }
        break;
    }
    case WO_B_DB_DELETE: {
        if (wo_row_has_referrers(db, q->cid, q->id)) {
            q->status = WO_T_FK;
            q->msg = "row is still referenced (restrict)";
            break;
        }
        if (wo_row_remove(db, q->cid, q->id) != 0) {
            q->status = WO_T_DB;
            q->msg = "no such row";
            break;
        }
        if (w && table_is_durable(db, q->cid)) {
            if (wo_wal_append_remove(w, q->cid, q->id) != 0) wo_wal_stage_fatal(w);
        }
        break;
    }
    case WO_B_DB_SCAN:
    case WO_B_DB_PROBE: {
        if (q->cid >= db->class_cnt) {
            q->status = WO_T_DB;
            q->msg = "no such class";
            break;
        }
        db_table *t = &db->tables[q->cid];
        uint64_t *out = NULL;
        uint32_t n = 0, cap = 0;
        if (t->row_size && (q->op == WO_B_DB_SCAN || q->index < t->index_cnt)) {
            uint32_t col = 0;
            uint8_t kind = 0;
            if (q->op == WO_B_DB_PROBE) {
                col = t->indexes[q->index].cols[0];
                kind = db->classes[q->cid].kinds[col];
                /* the O(1) path, mirroring the local executor: the key is
                 * engine-encoded here (db_text for Text), same buckets,
                 * same verify — worker shards get the identical speedup */
                const void *kb = NULL;
                uint32_t kl = 0;
                if ((kind == WO_K_TEXT || kind == WO_K_BYTES) && q->slots[0]) {
                    const db_text *s = (const db_text *)(uintptr_t)q->slots[0];
                    kb = s->bytes;
                    kl = s->len;
                }
                int prc = wo_idx_probe(db, q->cid, q->index, q->slots[0], kb, kl,
                                       &q->ids, &q->id_cnt);
                if (prc < 0) {
                    q->status = WO_T_OOM;
                    q->msg = "out of memory";
                    break;
                }
                if (prc == 1) break; /* probed; reply fields already set */
            }
            uint32_t total = t->slab_cnt * DB_SLAB_ROWS;
            for (uint32_t g = 0; g < total; g++) {
                if (!(t->bitmap[g >> 6] & (1ull << (g & 63)))) continue;
                db_row *row =
                    (db_row *)(t->slabs[g / DB_SLAB_ROWS] + (size_t)(g % DB_SLAB_ROWS) * t->row_size);
                if (q->op == WO_B_DB_PROBE) {
                    int eq;
                    if (kind == WO_K_TEXT || kind == WO_K_BYTES) {
                        /* both sides engine-encoded: the key was encoded on
                         * the requester's thread, the slot lives here */
                        const db_text *want = (const db_text *)(uintptr_t)q->slots[0];
                        const db_text *have = (const db_text *)(uintptr_t)row->slots[col];
                        eq = (!want && !have) ||
                             (want && have && want->len == have->len &&
                              memcmp(want->bytes, have->bytes, have->len) == 0);
                    } else
                        eq = row->slots[col] == q->slots[0];
                    if (!eq) continue;
                }
                if (n == cap) {
                    uint32_t ncap = cap ? cap * 2 : 16;
                    uint64_t *no = realloc(out, (size_t)ncap * 8u);
                    if (!no) {
                        free(out);
                        out = NULL;
                        q->status = WO_T_OOM;
                        q->msg = "out of memory";
                        break;
                    }
                    out = no;
                    cap = ncap;
                }
                out[n++] = row->id;
            }
        }
        if (!q->status) {
            q->ids = out;
            q->id_cnt = n;
        }
        break;
    }
    case WO_B_DB_GET_FIELD: {
        if (q->cid >= db->class_cnt || q->field >= db->classes[q->cid].field_cnt) {
            q->status = WO_T_DB;
            q->msg = "no such field";
            break;
        }
        db_row *row = wo_row_ptr(db, q->cid, q->id);
        if (!row) {
            q->status = WO_T_DB;
            q->msg = "no such row";
            break;
        }
        int ok = 1;
        q->val_kind = db->classes[q->cid].kinds[q->field];
        q->val = wo_db_val_clone(db->classes, q->val_kind, row->slots[q->field], &ok);
        if (!ok) {
            q->status = WO_T_OOM;
            q->msg = "out of memory";
        }
        break;
    }
    default:
        q->status = WO_T_DB;
        q->msg = "unknown db builtin";
        break;
    }
out:
    /* slot VALUES were consumed by the ops above (insert/update install or
     * free them); the PROBE key is ours to free, the array always is. (The
     * requester only encodes a key for an index its identical class table
     * declares, so a keyed request always finds its kind here.) */
    if (db && q->op == WO_B_DB_PROBE && q->slots && q->cid < db->class_cnt) {
        db_table *t = &db->tables[q->cid];
        if (t->row_size && q->index < t->index_cnt)
            wo_db_val_free(db, db->classes[q->cid].kinds[t->indexes[q->index].cols[0]],
                           q->slots[0]);
    }
    free(q->slots);
    q->slots = NULL;
    q->slot_cnt = 0;
}

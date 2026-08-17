#include "db.h"

#include <string.h>

#include "cont.h"
#include "table.h"
#include "wal.h"

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
        if (w) {
            /* RAM applied, record staged, ONE commit before the ack (the
             * builtin's return). A failed commit is a failed write: the
             * row is removed again so RAM never claims what disk never
             * acknowledged, and the statement traps. */
            if (wo_wal_append_insert(w, db, cid, id) != 0 || wo_wal_commit(w) != 0) {
                wo_row_remove(db, cid, id);
                *msg = "wal commit failed";
                return WO_T_IO;
            }
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
        if (w) {
            if (wo_wal_append_update(w, db, cid, id) != 0 || wo_wal_commit(w) != 0) {
                *msg = "wal commit failed"; /* RAM ahead of disk: trap, do not ack */
                return WO_T_IO;
            }
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
        if (w) {
            if (wo_wal_append_remove(w, cid, id) != 0 || wo_wal_commit(w) != 0) {
                *msg = "wal commit failed";
                return WO_T_IO;
            }
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

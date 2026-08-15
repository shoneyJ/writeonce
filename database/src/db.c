#include "db.h"

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
        uint64_t id = wo_row_insert(db, cid, &R[B + 1], msg);
        if (!id) return WO_T_DB; /* *msg already set (OOM / bad kind) */
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
    default:
        *msg = "unknown db builtin";
        return WO_T_DB;
    }
}

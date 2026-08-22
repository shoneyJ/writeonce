/* db.h — DB statement executors (iteration 9, Task 3+).
 *
 * The VM reaches the engine through one dispatcher with the same contract
 * as every builtin family: 0 = ok, else a WO_T_* code with *msg set. The
 * engine and WAL handles ride the runtime context as opaque pointers
 * (obj.h's rt.db / rt.wal) — set by main.c at boot, NULL in test binaries
 * that never touch DB statements (a DB builtin with rt.db == NULL traps
 * WO_T_DB "engine not initialized").
 *
 * Commit contract per statement (until iteration 8 brings ticks): the
 * insert applies to RAM, stages its WAL record, and COMMITS before the
 * builtin returns — the builtin returning IS the acknowledgment, so the
 * ack-after-fsync doctrine holds at statement granularity. No WAL
 * (rt.wal == NULL, no WO_DATA) means RAM-only: every test and every
 * corpus fixture runs that way; durability is opt-in by pointing WO_DATA
 * at a directory. */
#ifndef WO_DB_H
#define WO_DB_H

#include "vm.h"

int wo_builtin_db(wo_vm *vm, uint64_t *R, uint32_t ins, const char **msg);

/* ---- arc stage 3: one marshaled DB statement (the transparent DB actor).
 * A worker shard fills the request on ITS thread — args pre-encoded into
 * engine slots, since VM heaps are never read cross-shard — and ships it
 * to shard 0 in an envelope; the owner executes it via wo_db_exec_req and
 * ships it back. Ownership: `slots` VALUES pass to the owner (consumed by
 * the op), the array and every reply buffer pass back to the requester.
 * The envelope handoff (mutex + eventfd) orders `done` on both sides. */
typedef struct wo_db_req {
    /* request */
    uint32_t op; /* WO_B_DB_INSERT..WO_B_DB_PROBE */
    uint32_t cid, field, index;
    uint64_t id;
    uint64_t *slots; /* INSERT: field_cnt; UPDATE: 1; PROBE: 1 (the key) */
    uint32_t slot_cnt;
    /* routing */
    uint32_t from_shard;
    void *fiber; /* the parked wo_fiber*, opaque to the engine */
    int done;
    /* reply */
    int status;      /* 0 ok, else the WO_T_* the local path would trap */
    const char *msg; /* static literal, safe cross-thread */
    uint64_t result; /* INSERT: the new id */
    uint64_t *ids;   /* SCAN/PROBE: malloc'd id list */
    uint32_t id_cnt;
    uint8_t val_kind; /* GET_FIELD: a cloned engine value */
    uint64_t val;
} wo_db_req;

/* Execute one marshaled statement on the OWNER shard (vm = shard 0's; its
 * rt.db/rt.wal are the engine). Fills the reply fields; never traps. */
void wo_db_exec_req(wo_vm *vm, wo_db_req *q);

#endif /* WO_DB_H */

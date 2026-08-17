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

#endif /* WO_DB_H */

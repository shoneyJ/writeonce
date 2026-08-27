/* wal.h — typed-row write-ahead log + boot replay (iteration 9, Task 2).
 *
 * The c-runtime plan's shipped pattern (phases D/E), generalized to typed
 * rows. The commit order is doctrine, verbatim:
 *
 *   RAM apply  →  wal_append (staged)  →  wal_commit (write + fdatasync)
 *   → only then is the write ACKNOWLEDGED
 *
 * Record framing — replay-whole-or-not-at-all:
 *
 *   record  := len u32 | crc u32 | payload | mark u32
 *   len      = payload byte count (never 0; 0 = preallocated tail, stop)
 *   crc      = CRC32 of payload
 *   mark     = 0x574F4C31 "WOL1" — written LAST, so a record without its
 *              mark is torn by definition
 *   payload := kind u8 | class_id u32 | row_id u64 | body
 *   kind     : 1 insert (body = the row's fields, engine encoding below)
 *              2 remove (no body)
 *              3 update (reserved for Task 5)
 *
 * Field encoding in a body walks the class table's kinds:
 *   SCALAR  8 bytes
 *   TEXT    u32 len | bytes           (0xFFFFFFFF = nil)
 *   OWNED   u8 0 = nil, or u8 1 | u32 class_id | fields recursively
 *   MULTI   u8 0 = nil, or u8 1 | u8 elem_kind | u32 len | elements
 *   MAP     u8 0 = nil, or u8 1 | u8 kk | u8 vk | u32 len | k v pairs
 *
 * Replay decodes payloads STRAIGHT into engine-owned values — the VM heap
 * is never involved (boot must not depend on a VM existing yet), and rows
 * re-enter through the same choke-point row API, so Task 4's indexes are
 * rebuilt for free. A torn tail (short record, bad CRC, missing mark) drops
 * everything from the tear onward — never a partial record, never a record
 * after a tear. Little-endian on-disk, matching the .wob loader's platform
 * note.
 *
 * wo_wal_check is the offline oracle the crash battery verifies with: it
 * walks a WAL file with no engine at all and reports how many records are
 * intact and where the intact prefix ends. */
#ifndef WO_WAL_H
#define WO_WAL_H

#include "table.h"

#define WO_WAL_MARK 0x574F4C31u /* "WOL1" LE */

enum { WO_WAL_INSERT = 1, WO_WAL_REMOVE = 2, WO_WAL_UPDATE = 3 };

typedef struct wo_wal {
    int fd;
    uint64_t off; /* next write offset (the intact tail) */
    /* staged batch: appended by wal_append_*, flushed by wal_commit */
    uint8_t *buf;
    size_t len, cap;
} wo_wal;

/* Open (create if missing) and preallocate [prealloc] bytes (best-effort;
 * a filesystem without fallocate still works). Positions the write offset
 * at the end of the INTACT record prefix — an existing file is scanned the
 * same way replay scans it, so a torn tail is overwritten, not appended
 * after. 0 ok, -1 errno-style failure. */
int wo_wal_open(wo_wal *w, const char *path, uint64_t prealloc);
void wo_wal_close(wo_wal *w);

/* Stage a record for the row that MUST already be applied to RAM (the
 * commit-order doctrine). Insert/update read the row via wo_row_ptr.
 * 0 ok, -1 OOM / no such row. */
int wo_wal_append_insert(wo_wal *w, wo_db *db, uint32_t class_id, uint64_t id);
int wo_wal_append_remove(wo_wal *w, uint32_t class_id, uint64_t id);
/* UPDATE re-logs the whole row (KISS: replay replaces — remove + re-create
 * with the same id; the prefix/suffix delta trick from the survey is a
 * later optimization, recorded). Call AFTER the RAM update. */
int wo_wal_append_update(wo_wal *w, wo_db *db, uint32_t class_id, uint64_t id);

/* Write the staged batch and fdatasync — the ack line. Empty batch = ok,
 * no syscall. 0 ok, -1 write/sync failure (the batch stays staged). */
int wo_wal_commit(wo_wal *w);

/* Boot replay: apply every intact record to [db] in order. Ids re-enter
 * exactly as logged; each table's next_id advances past the replayed ids
 * that belong to this shard. Returns the number of records applied, or -1
 * on open failure / a record naming an unknown class (corruption beyond
 * what a torn tail explains). A torn tail is NOT an error: replay applies
 * the intact prefix and reports it. */
int64_t wo_wal_replay(const char *path, wo_db *db);

/* databasev2 2: as wo_wal_replay, but distinguishes the two failure kinds.
 * Returns the applied count on success; -1 on corruption beyond a torn tail;
 * -2 when the log holds records for a class the loaded image declares
 * `durable: false`, writing that class id through [volatile_cid] if non-NULL.
 * The plain wo_wal_replay above is this with NULL, kept so the existing
 * callers and the 156 WAL unit checks are untouched. */
int64_t wo_wal_replay_ex(const char *path, wo_db *db, uint32_t *volatile_cid);

/* Offline verification (no engine): scan [path], count intact records.
 * *intact_bytes (optional) = where the intact prefix ends. -1 = open
 * failure. */
int64_t wo_wal_check(const char *path, uint64_t *intact_bytes);

#endif /* WO_WAL_H */

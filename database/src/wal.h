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
    /* databasev2 4: where this WAL lives, so a durability failure can name
     * the file it could not write. An abort diagnostic without the path
     * sends an operator hunting. Owned here, freed by wo_wal_close. */
    char *path;
    uint64_t off; /* next write offset (the intact tail) */
    /* staged batch: appended by wal_append_*, flushed by wal_commit */
    uint8_t *buf;
    size_t len, cap;
    /* databasev2 4: group-commit diagnostics. Batching is worthless if
     * batches are always one, and a throughput change would then have come
     * from somewhere else — so the mechanism is measured, not assumed.
     * peak_staged also settles whether the batch needs a cap with a number
     * instead of a guess. Reported at exit under WO_WAL_STATS. */
    uint64_t stat_batches;     /* non-empty commits */
    uint64_t stat_records;     /* records those commits carried */
    uint64_t stat_peak_batch;  /* most records in one barrier */
    uint64_t stat_peak_staged; /* most bytes staged behind one barrier */
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

/* databasev2 4: which half of the barrier failed. A pwrite failure and an
 * fdatasync failure are different operational problems (a short write vs a
 * device refusing the flush), so the diagnostic must name the right one. */
#define WO_WAL_ERR_WRITE (-1)
#define WO_WAL_ERR_SYNC  (-2)

/* The process exit status for a durability failure.
 *
 * 74 is sysexits' EX_IOERR, chosen deliberately over a small number: 1 is a
 * trap and 2 is a loader refusal, but 3 and 4 are already used by SAMPLES for
 * their own meanings — db-bench's own `verify` exits 3 on a checksum mismatch,
 * and it is the gate that exercises durability, so a durability abort exiting 3
 * would have been indistinguishable from the mismatch it is supposed to help
 * diagnose. The low range belongs to programs; the runtime takes a high one. */
#define WO_EXIT_DURABILITY 74

/* Write the staged batch and fdatasync — the ack line. Empty batch = ok,
 * no syscall. 0 ok, WO_WAL_ERR_WRITE / WO_WAL_ERR_SYNC on failure (the
 * batch stays staged: a failed commit consumes nothing). */
int wo_wal_commit(wo_wal *w);

/* databasev2 4: a record could not even be STAGED (the row is already in
 * RAM, so this is the same unrecoverable position as a failed barrier — see
 * wo_wal_commit_fatal). Never returns. */
void wo_wal_stage_fatal(const wo_wal *w);

/* databasev2 4: commit, or END THE PROCESS.
 *
 * The one rule this iteration introduces: once a statement has mutated RAM,
 * the only outcomes are durable or process death. Retrying is not an
 * alternative — on Linux a failed fsync may already have discarded the dirty
 * pages, so a second call can report success having written nothing. The
 * recovery that works is replay, which returns the last durable state.
 *
 * [nrec] is the number of records in the batch, for the diagnostic only.
 * Returns on success; never returns on failure. */
void wo_wal_commit_fatal(wo_wal *w, uint32_t nrec);

/* Boot replay: apply every intact record to [db] in order. Ids re-enter
 * exactly as logged; each table's next_id advances past the replayed ids
 * that belong to this shard. Returns the number of records applied, or -1
 * on open failure / a record naming an unknown class (corruption beyond
 * what a torn tail explains). A torn tail is NOT an error: replay applies
 * the intact prefix and reports it. */
int64_t wo_wal_replay(const char *path, wo_db *db);

/* Offline verification (no engine): scan [path], count intact records.
 * *intact_bytes (optional) = where the intact prefix ends. -1 = open
 * failure. */
int64_t wo_wal_check(const char *path, uint64_t *intact_bytes);

#endif /* WO_WAL_H */

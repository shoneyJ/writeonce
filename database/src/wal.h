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
    /* databasev2 3: bytes the last compaction wrote. The trigger compares the
     * log against THIS rather than an estimate of the live set — estimating
     * would mean estimating Text, and the compactor knows the true number. */
    uint64_t compacted_bytes;
    /* databasev2 3: the preallocation this log was opened with. Compaction
     * MUST give the replacement the same one: the WAL is preallocated so that
     * appends never extend the file, which is what lets fdatasync alone be the
     * ack barrier. A replacement without it silently weakens durability. */
    uint64_t prealloc;
    /* databasev2 3: what compaction actually did, reported under WO_WAL_STATS.
     * The PAUSE is the number the spec refused to assume — compaction is
     * stop-the-world, so its duration is the cost being weighed. */
    uint64_t stat_compactions;
    uint64_t stat_compact_us_max;
    uint64_t stat_compact_us_total;
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

/* databasev2 3: the checkpoint trigger, as a PURE decision so it can be tested
 * without a store — which is the only way a policy like this gets tested at all.
 *
 * [used] the log's used bytes; [last] what the LAST compaction wrote (0 if it
 * has never run); [floor] the size below which compacting is not worth it;
 * [ratio] the multiple of [last] that counts as too much history.
 *
 * The denominator is the last compaction's MEASURED output rather than an
 * estimate of the live set: estimating would mean estimating Text, and the
 * compactor already knows the true number.
 *
 * There is deliberately NO TIME component. Postgres' CheckPointTimeout exists
 * to bound data loss from unflushed buffers; our records are durable at commit,
 * so a checkpoint only reclaims space and shortens boot. An idle log does not
 * grow, so a timer would fire with nothing to do.
 *
 * 1 = compact now, 0 = leave it. */
int wo_wal_should_compact(uint64_t used, uint64_t last, uint64_t floor, uint32_t ratio);

/* Defaults, overridable at boot by WO_CHECKPOINT_BYTES / WO_CHECKPOINT_RATIO.
 * The knobs are what make the policy testable: a test sets a tiny floor and
 * forces compaction in a few writes instead of waiting for megabytes. */
extern uint64_t wo_wal_ckpt_floor;
extern uint32_t wo_wal_ckpt_ratio;

/* databasev2 3: the temporary file compaction writes before the swap. Named
 * next to the log so it lands on the same filesystem — rename(2) is only
 * atomic within one. Boot removes a stale one (a crash before the rename). */
#define WO_WAL_TMP_SUFFIX ".compact"

/* databasev2 3: rewrite the log as one INSERT record per LIVE row, then swap
 * it in with rename(2).
 *
 * Recovery is deliberately untouched: the result is an ordinary log in the
 * ordinary grammar, replayed from byte 0. Crash safety comes from rename being
 * atomic — before it the live log is intact and the temp file is not
 * authoritative; after it the new log is complete. There is no window in which
 * a reader sees a mixture, so this needs no recovery logic of its own.
 *
 * REFUSES if anything is staged (returns -1 without touching the log): those
 * records would be written into a file about to be replaced. Callers must
 * invoke this only where the staging buffer is empty — right after a barrier.
 *
 * A failure is a MISSED OPTIMISATION, not a durability event: the original log
 * is left usable and the process keeps running. It must not take the fatal
 * path wo_wal_commit_fatal takes.
 *
 * 0 ok, -1 on any failure. */
int wo_wal_compact(wo_wal *w, wo_db *db);

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

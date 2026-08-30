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
 *              4 delta (single-field update; body = field_idx u32 |
 *                back-pointer offset u64 | the one field's value, engine
 *                encoding below — keys-resident tables only)
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

enum { WO_WAL_INSERT = 1, WO_WAL_REMOVE = 2, WO_WAL_UPDATE = 3, WO_WAL_DELTA = 4 };

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
    /* databasev2 2 (5c): rows whose payload may be dropped ONCE the barrier
     * they are staged behind succeeds. A keys-resident row cannot be dropped
     * at append time: with group commit the record is still in the staging
     * buffer, so its offset would pread zeros. Recorded here and performed by
     * wo_db_flush_drops after the commit — the same shape as the drain's held
     * replies, and for the same reason. If the process dies first the list
     * dies with it, which is correct: nothing was dropped and nothing lost. */
    struct wo_wal_pend { uint32_t cid; uint64_t id; uint64_t off; } *pend;
    size_t pend_len, pend_cap;
    /* Task 4 (keys-resident delta updates): rows whose id-map entry must
     * move to a NEW offset once the delta staged there is durable. Same
     * three fields as `pend` above, deliberately its OWN list: a drop
     * discards a payload and a re-point moves a live row's chain head — two
     * different meanings a shared list would force a future reader to guess
     * between. Same lifetime discipline as `pend`: recorded before the
     * barrier, applied after it, and lost with the process if it dies
     * first — which is correct, since nothing was re-pointed either. */
    struct wo_wal_pend *repoint;
    size_t repoint_len, repoint_cap;
    uint64_t stat_compactions;
    uint64_t stat_compact_us_max;
    uint64_t stat_compact_us_total;
} wo_wal;

/* databasev2 2: the file offset the NEXT staged record will occupy.
 *
 * Exact, and knowable at append time — no deferral to flush is needed, which
 * is what the design spec feared. `off` is the durable tail and `len` the
 * bytes staged but not yet written, and wo_wal_commit pwrites the whole batch
 * AT `off` before advancing it, so a record staged now lands at off+len.
 *
 * Correct across the two awkward cases:
 *   - a failed commit leaves `off` unadvanced and `len` intact, so the batch
 *     is rewritten from the same place and previously-reported offsets stay
 *     valid;
 *   - a torn tail is handled by wo_wal_open, which positions `off` at the end
 *     of the INTACT prefix, so offsets are always relative to validated data.
 *
 * Call it BEFORE the append whose offset you want, and only trust the value
 * after the matching wo_wal_commit returns 0 — a record whose commit failed
 * was never durable and its offset must not be recorded anywhere. */
static inline uint64_t wo_wal_next_offset(const wo_wal *w) { return w->off + w->len; }

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
/* DELTA logs one field change, for a keys-resident row whose payload may
 * already be gone from RAM (so there is no whole row to re-log). back_off
 * is the row's PREVIOUS record's offset (insert or an earlier delta) — a
 * caller parameter, not looked up here, so the encoder stays ignorant of
 * table/map state. */
int wo_wal_append_delta(wo_wal *w, wo_db *db, uint32_t class_id, uint64_t id,
                         uint32_t field_idx, uint64_t back_off, uint64_t value);

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

/* databasev2 2 (5c): note a payload that may be dropped after the next commit.
 * 0 ok, -1 out of memory (the row simply stays resident, which is safe). */
int wo_wal_pend_drop(wo_wal *w, uint32_t cid, uint64_t id, uint64_t off);

/* Task 4 (keys-resident delta updates): note a keys-resident row's id-map
 * entry that must move to [off] once the delta staged there is durable —
 * the update-arm counterpart of wo_wal_pend_drop, on its own list (see the
 * `repoint` field). 0 ok, -1 out of memory.
 *
 * IMPORTANT 1 (review finding): unlike wo_wal_pend_drop, a failure here is
 * NOT safe to ignore. Replay does NOT reconcile a lost re-point: if a
 * second update to this row lands in the same drain, it finds no pending
 * entry, falls back to the stale durable offset, and its delta chains PAST
 * the one this call was meant to record — every reader, replay and
 * compaction included, then agrees on the wrong value, permanently.
 * Callers must treat a nonzero return as fatal (wo_wal_repoint_fatal),
 * exactly like a failed wo_wal_stage_fatal. */
int wo_wal_pend_repoint(wo_wal *w, uint32_t cid, uint64_t id, uint64_t off);

/* Task 4: the most recent PENDING re-point recorded for (cid, id), not yet
 * flushed to the id map — needed so a second update to the same row, staged
 * behind the SAME barrier as the first, computes its back-pointer against
 * the first's delta instead of the row's last DURABLE offset (which would
 * skip it). Off + 1, 0 = none pending (the caller falls back to
 * wo_row_offset1). Does NOT consult the durable map itself. */
uint64_t wo_wal_repoint_offset1(const wo_wal *w, uint32_t cid, uint64_t id);

/* databasev2 2 (5c): perform every pending drop, THEN every pending
 * re-point (Task 4). Call ONLY after a commit has succeeded — that is what
 * makes the recorded offsets readable. */
void wo_db_flush_drops(wo_db *db, wo_wal *w);

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
/* databasev2 11: the two terms a size-based policy needs beside its ratio.
 *
 * WO_CKPT_ABS_BYTES is the TRIGGERING threshold — PostgreSQL's
 * `autovacuum_vacuum_threshold`, not our `floor`, which suppresses instead.
 * Past this much reclaimable garbage, compact regardless of proportion, so
 * garbage that is large absolutely but small against a big live set still gets
 * reclaimed.
 *
 * WO_CKPT_MAX_GARBAGE caps the proportional term, mirroring
 * `autovacuum_vacuum_max_threshold`, so a very large live set cannot defer
 * compaction indefinitely. */
#define WO_CKPT_ABS_BYTES    (64u * 1024u * 1024u)
#define WO_CKPT_MAX_GARBAGE  (256u * 1024u * 1024u)

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

/* IMPORTANT 1 (review finding): a pending re-point could not even be
 * RECORDED — same unrecoverable position as wo_wal_stage_fatal, see
 * wo_wal_pend_repoint's own doc. Never returns. */
void wo_wal_repoint_fatal(const wo_wal *w);

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

/* databasev2 2: as wo_wal_replay, but distinguishes the two failure kinds.
 * Returns the applied count on success; -1 on corruption beyond a torn tail;
 * -2 when the log holds records for a class the loaded image declares
 * `durable: false`, writing that class id through [volatile_cid] if non-NULL.
 * The plain wo_wal_replay above is this with NULL, kept so the existing
 * callers and the 156 WAL unit checks are untouched. */
int64_t wo_wal_replay_ex(const char *path, wo_db *db, uint32_t *volatile_cid);

/* databasev2 2: read one row straight from a log offset — the offset twin of
 * wo_row_read. [out_vals] must have room for the class's field_cnt values and
 * receives FRESH VM allocations (the out-gate: always copies). [class_out] and
 * [id_out] are optional. Offsets come from wo_wal_next_offset, recorded at
 * append time.
 *
 *   0  ok
 *  -1  no intact record at that offset, a malformed header, a record that
 *      does not decode, trailing bytes, or a REMOVE tombstone (which carries
 *      no fields — refused rather than decoded, since returning a deleted row
 *      as live is the worst outcome available here)
 *  -2  out of memory (*msg set)
 *
 * Nothing in the engine calls this yet: it is the read half of `resident:
 * keys`, landed ahead of the storage change so it can be tested alone. */
int wo_wal_read_row_at(wo_wal *w, wo_db *db, wo_rt *rt, uint64_t off,
                       uint32_t *class_out, uint64_t *id_out, uint64_t *out_vals,
                       const char **msg);

/* keys-resident delta updates, Task 2: fold a delta chain into a row's
 * CURRENT field values, walking BACKWARD from [off] until a full row
 * (INSERT/UPDATE) is reached.
 *
 * [off] is the row's most recent record, exactly what wo_wal_read_row_at
 * takes. Each delta names its predecessor's offset (the append-time
 * back-pointer); the walk keeps hopping backward, remembering the FIRST
 * value seen for each field index — the newest delta touching it, since
 * newest is seen first — and skipping a delta whose field is already
 * resolved. Reaching the base row decodes every field, then overlays
 * whatever the walk resolved.
 *
 * out_vals[0..field_cnt) receive ENGINE-owned values (dec_val's
 * representation, exactly what a slab row's own slots hold) — NOT VM
 * values — so this one function serves every caller: a read decodes the
 * result onward through wo_val_decode_vm, replay installs it straight into
 * a freshly created row's slots, and compaction re-encodes it with enc_val
 * into a fresh full-row record. The caller frees every slot with
 * wo_db_val_free once done, on every path. This is the fold: written once,
 * called by all three — a fold that disagreed between them would be a
 * database that changes its mind at boot.
 *
 * [class_out] / [id_out] (optional) receive the row's identity, checked
 * against EVERY record touched — a chain that disagrees about whose row it
 * is is corruption, not a new row.
 *
 * A back-pointer must name something STRICTLY EARLIER in the log than the
 * record holding it — the row's PREVIOUS record, by construction, always
 * is. Anything else (a self-pointer, a forward pointer, corruption or
 * forgery of any shape) is refused on the very hop that violates it, which
 * also rules out a cycle: a walk that only ever moves to a lower offset
 * cannot revisit one.
 *
 * 0 ok, -1 no intact/malformed/corrupt record anywhere in the chain (or a
 * REMOVE tombstone reached mid-chain), -2 out of memory (*msg set). */
/* databasev2 11: `hops_out` (may be NULL) reports how many DELTA records the
 * walk crossed before reaching the full-row record that terminates the chain —
 * 0 for a row that has never been updated. The walk already visits each hop, so
 * this costs nothing, and it is the signal the update path uses to decide when
 * to flatten. It is this design's equivalent of PostgreSQL's `pd_prune_xid`: a
 * cheap "is work worth doing" hint obtained from something already being done. */
int wo_wal_fold_row_at(wo_wal *w, wo_db *db, uint64_t off, uint32_t *class_out,
                       uint64_t *id_out, uint64_t *out_vals, uint32_t *hops_out,
                       const char **msg);

/* databasev2 11: append a FULL-ROW image taken from a caller-supplied row,
 * rather than one looked up by id. wo_wal_append_insert sources its values via
 * wo_row_ptr, which is NULL for a keys-resident row whose payload has been
 * dropped; the update path holds a materialised row and needs to log it as a
 * chain-terminating record. Written as WO_WAL_INSERT because that is what a
 * chain's base must be: it has to replay into a database where nothing
 * precedes it. */
int wo_wal_append_row_image(wo_wal *w, wo_db *db, uint32_t class_id, uint64_t id,
                            const db_row *r);

/* Offline verification (no engine): scan [path], count intact records.
 * *intact_bytes (optional) = where the intact prefix ends. -1 = open
 * failure. */
int64_t wo_wal_check(const char *path, uint64_t *intact_bytes);

#endif /* WO_WAL_H */

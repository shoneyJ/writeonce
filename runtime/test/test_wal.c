/* test_wal — iteration 9 Task 2: typed WAL + boot replay.
 * Round-trip through a replay, torn-tail drop, reopen-overwrites-tear,
 * and the commit-then-kill crash battery: a forked child inserts rows and
 * acks each COMMITTED id over a pipe; SIGKILL lands mid-stream; the parent
 * verifies with the offline oracle and a replay that every acked id is
 * present with the right contents. */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "gc.h"
#include "obj.h"
#include "t.h"
#include "table.h"
#include "wal.h"

/* class 0: Row { n: scalar, label: Text } */
static const uint8_t row_kinds[] = {WO_K_SCALAR, WO_K_TEXT};
static const wo_classdesc CLASSES[] = {
    {.name = 0, .flags = 0, .field_cnt = 2, .kinds = row_kinds},
};

static char g_dir[64];

static void test_roundtrip_replay(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/basic.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    const char *msg = "";

    /* three inserts and one remove, RAM first, WAL second, one commit */
    uint64_t ids[3];
    for (int i = 0; i < 3; i++) {
        wo_str *s = wo_str_new(&rt, "abcXYZ" + i, 3); /* "abc","bcX","cXY" */
        uint64_t vals[2] = {(uint64_t)(i * 10), (uint64_t)(uintptr_t)s};
        ids[i] = wo_row_insert(&db, 0, vals, &msg, NULL);
        T_CHECK(ids[i] != 0);
        T_EQ(wo_wal_append_insert(&w, &db, 0, ids[i]), 0);
        wo_str_free(&rt, s);
    }
    T_EQ(wo_row_remove(&db, 0, ids[1]), 0);
    T_EQ(wo_wal_append_remove(&w, 0, ids[1]), 0);
    T_EQ(wo_wal_commit(&w), 0);
    wo_wal_close(&w);
    wo_db_destroy(&db);

    /* boot: fresh engine, replay, deep-compare */
    wo_db db2;
    T_EQ(wo_db_init(&db2, CLASSES, 1, 0, 1), 0);
    T_EQ(wo_wal_replay(path, &db2), 4);
    uint64_t out[2];
    T_EQ(wo_row_read(&db2, &rt, 0, ids[0], out, &msg), 0);
    T_EQ(out[0], 0);
    wo_str *s0 = (wo_str *)(uintptr_t)out[1];
    T_CHECK(s0->len == 3 && memcmp(s0->data, "abc", 3) == 0);
    wo_str_free(&rt, s0);
    T_EQ(wo_row_read(&db2, &rt, 0, ids[1], out, &msg), -1); /* removed */
    T_EQ(wo_row_read(&db2, &rt, 0, ids[2], out, &msg), 0);
    T_EQ(out[0], 20);
    wo_str_free(&rt, (wo_str *)(uintptr_t)out[1]);
    /* next_id advanced past the replayed ids: a fresh insert never collides */
    uint64_t vals[2] = {99, 0};
    uint64_t fresh = wo_row_insert(&db2, 0, vals, &msg, NULL);
    T_CHECK(fresh > ids[2]);
    wo_db_destroy(&db2);

    /* update record: re-log, replay replaces */
    {
        char upath[128];
        snprintf(upath, sizeof upath, "%s/upd.wal", g_dir);
        wo_db du;
        T_EQ(wo_db_init(&du, CLASSES, 1, 0, 1), 0);
        wo_wal wu;
        T_EQ(wo_wal_open(&wu, upath, 0), 0);
        wo_str *s1 = wo_str_new(&rt, "old", 3);
        uint64_t uv[2] = {7, (uint64_t)(uintptr_t)s1};
        uint64_t uid = wo_row_insert(&du, 0, uv, &msg, NULL);
        T_EQ(wo_wal_append_insert(&wu, &du, 0, uid), 0);
        int ek = 0;
        wo_str *s2 = wo_str_new(&rt, "new!", 4);
        T_EQ(wo_row_update_field(&du, 0, uid, 1, (uint64_t)(uintptr_t)s2, &msg, &ek), 0);
        T_EQ(wo_row_update_field(&du, 0, uid, 0, 8, &msg, &ek), 0);
        T_EQ(wo_wal_append_update(&wu, &du, 0, uid), 0);
        T_EQ(wo_wal_commit(&wu), 0);
        wo_wal_close(&wu);
        wo_db_destroy(&du);
        wo_db db4;
        T_EQ(wo_db_init(&db4, CLASSES, 1, 0, 1), 0);
        T_EQ(wo_wal_replay(upath, &db4), 2);
        uint64_t uo[2];
        T_EQ(wo_row_read(&db4, &rt, 0, uid, uo, &msg), 0);
        T_EQ(uo[0], 8);
        wo_str *us = (wo_str *)(uintptr_t)uo[1];
        T_CHECK(us->len == 4 && memcmp(us->data, "new!", 4) == 0);
        wo_str_free(&rt, us);
        wo_drop_obj(&rt, (wo_hdr *)s1);
        wo_drop_obj(&rt, (wo_hdr *)s2);
        wo_db_destroy(&db4);
    }

    /* replay of a missing file is a fresh boot, not an error */
    wo_db db3;
    T_EQ(wo_db_init(&db3, CLASSES, 1, 0, 1), 0);
    T_EQ(wo_wal_replay("/nonexistent/nope.wal", &db3), 0);
    wo_db_destroy(&db3);
    wo_rt_destroy(&rt);
}

/* databasev2 4 part A, Task 1: a failed barrier must be DETECTED, and the
 * caller must be able to tell WHICH operation failed — a pwrite failure and
 * an fdatasync failure are different operational problems and the diagnostic
 * has to name the right one. This proves detection only; the fatal exit that
 * follows it cannot be exercised in-process. */
static void test_commit_failure_detected(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/commitfail.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    const char *msg = "";

    /* the WAL remembers where it lives — the abort diagnostic is worthless
     * without it */
    T_CHECK(w.path != NULL && strstr(w.path, "commitfail.wal") != NULL);

    wo_str *s = wo_str_new(&rt, "abc", 3);
    uint64_t vals[2] = {7, (uint64_t)(uintptr_t)s};
    uint64_t id = wo_row_insert(&db, 0, vals, &msg, NULL);
    T_CHECK(id != 0);
    T_EQ(wo_wal_append_insert(&w, &db, 0, id), 0);
    T_CHECK(w.len > 0); /* something really is staged */

    /* an unusable descriptor: pwrite reports EBADF. -1 is used rather than
     * closing the real fd so the close below cannot double-free it. */
    int real = w.fd;
    w.fd = -1;
    T_EQ(wo_wal_commit(&w), WO_WAL_ERR_WRITE);
    T_CHECK(w.len > 0); /* a failed commit consumes nothing */
    w.fd = real;

    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

/* databasev2 3 Task 1: compaction rewrites the log as one record per LIVE row.
 * Asserts BOTH halves on purpose: "the file got shorter" is also true of a
 * truncating bug, so the replay comparison is what actually proves it. */
static void test_compact_shortens_and_replays_equal(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/compact.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    const char *msg = "";

    uint64_t ids[3];
    for (int i = 0; i < 3; i++) {
        wo_str *s = wo_str_new(&rt, "abc", 3);
        uint64_t vals[2] = {(uint64_t)(i * 10), (uint64_t)(uintptr_t)s};
        ids[i] = wo_row_insert(&db, 0, vals, &msg, NULL);
        T_CHECK(ids[i] != 0);
        T_EQ(wo_wal_append_insert(&w, &db, 0, ids[i]), 0);
        T_EQ(wo_wal_commit(&w), 0);
    }
    /* age it: the SAME row updated repeatedly, so HISTORY grows while the live
     * set does not — the exact case checkpoint exists for */
    for (int k = 0; k < 40; k++) {
        int ek = 0;
        T_EQ(wo_row_update_field(&db, 0, ids[0], 0, (uint64_t)(500 + k), &msg, &ek), 0);
        T_EQ(wo_wal_append_update(&w, &db, 0, ids[0]), 0);
        T_EQ(wo_wal_commit(&w), 0);
    }
    uint64_t before_bytes = 0;
    int64_t before_recs = wo_wal_check(path, &before_bytes);
    T_CHECK(before_recs == 43); /* 3 inserts + 40 updates, all history */

    T_EQ(wo_wal_compact(&w, &db), 0);

    uint64_t after_bytes = 0;
    int64_t after_recs = wo_wal_check(path, &after_bytes);
    T_CHECK(after_recs == 3);              /* one record per LIVE row */
    T_CHECK(after_bytes < before_bytes);   /* and the file really shrank */

    /* the WAL stays usable: the descriptor was reopened and the offset reset,
     * so a further write must land AFTER the compacted records, not over them */
    wo_str *s4 = wo_str_new(&rt, "xyz", 3);
    uint64_t v4[2] = {99, (uint64_t)(uintptr_t)s4};
    uint64_t id4 = wo_row_insert(&db, 0, v4, &msg, NULL);
    T_CHECK(id4 != 0);
    T_EQ(wo_wal_append_insert(&w, &db, 0, id4), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_CHECK(wo_wal_check(path, NULL) == 4);
    wo_wal_close(&w);

    /* the proof: a FRESH store replayed from the compacted log must hold the
     * same rows, the same ids, and the LAST value each row had */
    wo_db db2;
    T_EQ(wo_db_init(&db2, CLASSES, 1, 0, 1), 0);
    T_EQ(wo_wal_replay(path, &db2), 4);
    uint64_t out[2];
    T_EQ(wo_row_read(&db2, &rt, 0, ids[0], out, &msg), 0);
    T_CHECK(out[0] == 539); /* the 40th update won, not the original 0 */
    wo_str_free(&rt, (wo_str *)(uintptr_t)out[1]);
    T_EQ(wo_row_read(&db2, &rt, 0, ids[1], out, &msg), 0);
    T_CHECK(out[0] == 10);
    wo_str_free(&rt, (wo_str *)(uintptr_t)out[1]);
    T_EQ(wo_row_read(&db2, &rt, 0, ids[2], out, &msg), 0);
    T_CHECK(out[0] == 20);
    wo_str_free(&rt, (wo_str *)(uintptr_t)out[1]);
    T_EQ(wo_row_read(&db2, &rt, 0, id4, out, &msg), 0);
    T_CHECK(out[0] == 99);
    wo_str_free(&rt, (wo_str *)(uintptr_t)out[1]);

    wo_db_destroy(&db2);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

/* databasev2 3 Task 2: a stale temp file is the one input that could be
 * mistaken for data — a crash before the rename leaves one behind, full of
 * well-formed records that are NOT yet authoritative. So the fixture uses
 * plausible records (a byte copy of a real log), not garbage: garbage would be
 * rejected by the CRC anyway and would prove nothing. */
static void test_stale_compact_temp_is_removed(void) {
    char path[128], tmp[160];
    snprintf(path, sizeof path, "%s/stale.wal", g_dir);
    snprintf(tmp, sizeof tmp, "%s%s", path, WO_WAL_TMP_SUFFIX);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    const char *msg = "";

    /* two live rows in the REAL log */
    uint64_t ids[2];
    for (int i = 0; i < 2; i++) {
        wo_str *s = wo_str_new(&rt, "abc", 3);
        uint64_t vals[2] = {(uint64_t)(i + 1), (uint64_t)(uintptr_t)s};
        ids[i] = wo_row_insert(&db, 0, vals, &msg, NULL);
        T_EQ(wo_wal_append_insert(&w, &db, 0, ids[i]), 0);
        T_EQ(wo_wal_commit(&w), 0);
    }
    wo_wal_close(&w);

    /* forge a plausible stale temp: a byte copy of the real log */
    {
        int src = open(path, O_RDONLY);
        int dst = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        T_CHECK(src >= 0 && dst >= 0);
        char buf[8192];
        ssize_t n;
        while ((n = read(src, buf, sizeof buf)) > 0) T_CHECK(write(dst, buf, (size_t)n) == n);
        close(src);
        close(dst);
        T_EQ(access(tmp, F_OK), 0); /* it really is there before we open */
    }

    wo_wal w2;
    T_EQ(wo_wal_open(&w2, path, 1 << 16), 0);
    T_CHECK(access(tmp, F_OK) != 0); /* gone, and never consulted */
    wo_wal_close(&w2);

    /* and the live log still says exactly what it said */
    wo_db db2;
    T_EQ(wo_db_init(&db2, CLASSES, 1, 0, 1), 0);
    T_EQ(wo_wal_replay(path, &db2), 2);
    uint64_t out[2];
    T_EQ(wo_row_read(&db2, &rt, 0, ids[0], out, &msg), 0);
    T_CHECK(out[0] == 1);
    wo_str_free(&rt, (wo_str *)(uintptr_t)out[1]);
    T_EQ(wo_row_read(&db2, &rt, 0, ids[1], out, &msg), 0);
    T_CHECK(out[0] == 2);
    wo_str_free(&rt, (wo_str *)(uintptr_t)out[1]);

    wo_db_destroy(&db2);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

/* databasev2 7: WO_DATA names EITHER a directory (today's form, <dir>/shard-0.wal
 * byte for byte) or THE log file. Resolution is a pure function so main.c's
 * only branch is "print the refusal, exit 2" — and so every arm of the rule is
 * checkable here rather than by booting wovm against the filesystem. */
static void test_resolve_data_path(void) {
    char in[192], out[256], want[256];

    /* an existing directory: exactly the bytes main.c always produced */
    T_EQ(wo_wal_resolve_data_path(g_dir, out, sizeof out), 0);
    snprintf(want, sizeof want, "%s/shard-0.wal", g_dir);
    T_STREQ(out, want);

    /* a trailing slash keeps the directory form even when nothing exists
       there — including the doubled slash today's snprintf produced */
    snprintf(in, sizeof in, "%s/nodir/", g_dir);
    T_EQ(wo_wal_resolve_data_path(in, out, sizeof out), 0);
    snprintf(want, sizeof want, "%s/nodir//shard-0.wal", g_dir);
    T_STREQ(out, want);

    /* absent file under an existing parent: the path IS the log; resolution
       itself creates nothing (wo_wal_open's O_CREAT does, later) */
    snprintf(in, sizeof in, "%s/app.db", g_dir);
    T_EQ(wo_wal_resolve_data_path(in, out, sizeof out), 0);
    T_STREQ(out, in);
    T_CHECK(access(in, F_OK) != 0);

    /* an existing regular file: opened as the log */
    {
        int fd = open(in, O_WRONLY | O_CREAT, 0644);
        T_CHECK(fd >= 0);
        close(fd);
    }
    T_EQ(wo_wal_resolve_data_path(in, out, sizeof out), 0);
    T_STREQ(out, in);

    /* a bare relative name: its parent is ".", which always exists */
    T_EQ(wo_wal_resolve_data_path("app.db", out, sizeof out), 0);
    T_STREQ(out, "app.db");

    /* missing parent: refused, the PARENT is handed back for the message,
       and nothing was mkdir'd on the way */
    snprintf(in, sizeof in, "%s/nodir/app.db", g_dir);
    T_EQ(wo_wal_resolve_data_path(in, out, sizeof out), WO_WAL_PATH_NO_PARENT);
    snprintf(want, sizeof want, "%s/nodir", g_dir);
    T_STREQ(out, want);
    T_CHECK(access(want, F_OK) != 0);

    /* the parent exists but is a FILE (ENOTDIR): same refusal, same subject */
    snprintf(in, sizeof in, "%s/app.db/x.db", g_dir);
    T_EQ(wo_wal_resolve_data_path(in, out, sizeof out), WO_WAL_PATH_NO_PARENT);
    snprintf(want, sizeof want, "%s/app.db", g_dir);
    T_STREQ(out, want);

    /* exists, but neither a regular file nor a directory */
    snprintf(in, sizeof in, "%s/fifo.db", g_dir);
    T_EQ(mkfifo(in, 0600), 0);
    T_EQ(wo_wal_resolve_data_path(in, out, sizeof out), WO_WAL_PATH_NOT_A_FILE);

    /* a result that would not fit is refused, never truncated (today's
       snprintf into main.c's 512-byte buffer truncated silently) */
    T_EQ(wo_wal_resolve_data_path(g_dir, out, 8), WO_WAL_PATH_TOO_LONG);
    snprintf(in, sizeof in, "%s/app.db", g_dir);
    T_EQ(wo_wal_resolve_data_path(in, out, strlen(in)), WO_WAL_PATH_TOO_LONG);
}

/* databasev2 3 Task 3: the trigger, tested as a pure decision. Kept pure
 * precisely so it CAN be tested — a policy only observable by writing megabytes
 * and waiting is a policy nobody checks. */
static void test_should_compact_policy(void) {
    /* below the floor, nothing fires however bad the ratio looks */
    T_EQ(wo_wal_should_compact(1000, 10, 4096, 3), 0);
    T_EQ(wo_wal_should_compact(4095, 1, 4096, 3), 0);
    /* past the floor with no prior compaction: run once to learn the size */
    T_EQ(wo_wal_should_compact(4096, 0, 4096, 3), 1);
    /* with a known denominator it is a straight ratio test */
    T_EQ(wo_wal_should_compact(30000, 10000, 4096, 3), 0); /* exactly 3x is not MORE than 3x */
    T_EQ(wo_wal_should_compact(30001, 10000, 4096, 3), 1);
    T_EQ(wo_wal_should_compact(19999, 10000, 4096, 2), 0);
    T_EQ(wo_wal_should_compact(20001, 10000, 4096, 2), 1);
    /* a zero ratio disables the policy rather than dividing by nothing */
    T_EQ(wo_wal_should_compact(1u << 30, 10, 4096, 0), 0);
}

/* databasev2 3 Task 3: the ordering rule, asserted rather than trusted.
 * Compaction with records staged would write them into a file about to be
 * replaced, so it must be REFUSED — and refused without touching the log. */
static void test_compact_refuses_with_staged_records(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/staged.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    const char *msg = "";

    wo_str *s1 = wo_str_new(&rt, "abc", 3);
    uint64_t v1[2] = {7, (uint64_t)(uintptr_t)s1};
    uint64_t id1 = wo_row_insert(&db, 0, v1, &msg, NULL);
    T_EQ(wo_wal_append_insert(&w, &db, 0, id1), 0);
    T_EQ(wo_wal_commit(&w), 0); /* durable, buffer empty */

    /* now stage WITHOUT committing */
    wo_str *s2 = wo_str_new(&rt, "xyz", 3);
    uint64_t v2[2] = {8, (uint64_t)(uintptr_t)s2};
    uint64_t id2 = wo_row_insert(&db, 0, v2, &msg, NULL);
    T_EQ(wo_wal_append_insert(&w, &db, 0, id2), 0);
    T_CHECK(w.len > 0);

    uint64_t before = 0;
    int64_t recs = wo_wal_check(path, &before);
    T_EQ(wo_wal_compact(&w, &db), -1);  /* refused */
    T_CHECK(w.len > 0);                 /* and the staged record is still there */
    uint64_t after = 0;
    T_CHECK(wo_wal_check(path, &after) == recs && after == before); /* log untouched */

    /* the staged record still commits normally afterwards */
    T_EQ(wo_wal_commit(&w), 0);
    T_CHECK(wo_wal_check(path, NULL) == recs + 1);

    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

/* databasev2 3 Task 4: kill -9 DURING compaction.
 *
 * The existing battery is insert-only, so its "records >= acks" oracle is
 * exactly what compaction is allowed to break: collapsing history is the point.
 * The invariant that survives is the ACKED LIVE SET — every id acked as
 * inserted and not later acked as deleted must be present with its acked value,
 * and every id acked as deleted must be absent. Both the pre-compaction and the
 * post-compaction log satisfy that identically, which is precisely the
 * "never a mixture" property the design is shaped around.
 *
 * The child deletes as it goes so HISTORY accumulates while the live set stays
 * small — without that, compaction would have nothing to collapse and the test
 * would prove nothing. */
#define CK_DELETED UINT64_MAX

static void ck_ack(int fd, uint64_t id, uint64_t val) {
    uint64_t rec[2] = {id, val};
    if (write(fd, rec, sizeof rec) != (ssize_t)sizeof rec) _exit(0); /* parent gone */
}

static void compact_battery_child(const char *path, int ack_fd) {
    wo_rt rt;
    wo_db db;
    wo_wal w;
    if (wo_rt_init(&rt, 1 << 20, CLASSES, 1) != 0) _exit(9);
    if (wo_db_init(&db, CLASSES, 1, 0, 1) != 0) _exit(9);
    if (wo_wal_open(&w, path, 1 << 20) != 0) _exit(9);
    const char *msg = "";
    uint64_t live[512];
    size_t nlive = 0;
    for (uint64_t i = 0;; i++) {
        uint64_t val = i * 7 + 3;
        wo_str *s = wo_str_new(&rt, "r", 1);
        uint64_t vals[2] = {val, (uint64_t)(uintptr_t)s};
        uint64_t id = wo_row_insert(&db, 0, vals, &msg, NULL);
        wo_str_free(&rt, s);
        if (!id) _exit(9);
        if (wo_wal_append_insert(&w, &db, 0, id) != 0) _exit(9);
        if (wo_wal_commit(&w) != 0) _exit(9); /* durable BEFORE the ack */
        ck_ack(ack_fd, id, val);
        if (nlive < 512) live[nlive++] = id;

        /* drop the oldest so history grows while the live set does not */
        if (nlive > 16) {
            uint64_t victim = live[0];
            memmove(live, live + 1, (nlive - 1) * sizeof live[0]);
            nlive--;
            /* INTENT FIRST, deliberately. An ack after the commit would race:
             * a kill between them leaves the row legitimately gone on disk
             * while the last ack still says "inserted", and the parent would
             * demand a row the engine was right to remove. Announcing intent
             * makes the row's fate simply UNKNOWN to the parent, which is the
             * honest thing to assert about it. */
            ck_ack(ack_fd, victim, CK_DELETED);
            if (wo_row_remove(&db, 0, victim) != 0) _exit(9);
            if (wo_wal_append_remove(&w, 0, victim) != 0) _exit(9);
            if (wo_wal_commit(&w) != 0) _exit(9);
        }
        /* compact often, so a kill has a real chance of landing inside one */
        if (i % 24 == 23) (void)wo_wal_compact(&w, &db);
    }
}

static void test_compact_crash_battery(void) {
    int rounds = 40; /* it is a RACE: one green run proves very little */
    for (int round = 0; round < rounds; round++) {
        char path[128], tmp[160];
        snprintf(path, sizeof path, "%s/ckcrash-%d.wal", g_dir, round);
        snprintf(tmp, sizeof tmp, "%s%s", path, WO_WAL_TMP_SUFFIX);
        int pipefd[2];
        T_EQ(pipe(pipefd), 0);
        pid_t pid = fork();
        T_CHECK(pid >= 0);
        if (pid == 0) {
            close(pipefd[0]);
            compact_battery_child(path, pipefd[1]);
            _exit(0);
        }
        close(pipefd[1]);
        /* vary the instant so kills land before, inside and after rewrites */
        struct timespec ts = {0, (7 + round * 3) * 1000000L};
        while (nanosleep(&ts, &ts) != 0) {}
        kill(pid, SIGKILL);
        int status;
        waitpid(pid, &status, 0);

        /* replay the acks into the expected live set, in order */
        uint64_t ids[65536], vals[65536];
        size_t n = 0;
        for (;;) {
            uint64_t rec[2];
            ssize_t r = read(pipefd[0], rec, sizeof rec);
            if (r != (ssize_t)sizeof rec) break;
            if (n < 65536) { ids[n] = rec[0]; vals[n] = rec[1]; n++; }
        }
        close(pipefd[0]);
        T_CHECK(n > 0); /* the child got at least one commit out */

        wo_rt rt;
        T_EQ(wo_rt_init(&rt, 1 << 22, CLASSES, 1), 0);
        wo_db db;
        T_EQ(wo_db_init(&db, CLASSES, 1, 0, 1), 0);
        int64_t ck_recs = wo_wal_check(path, NULL);
        int64_t ck_applied = wo_wal_replay(path, &db);
        T_CHECK(ck_applied >= 0); /* never reported as corruption */

        /* A stale temp may well EXIST after a kill inside compaction — that is
         * the expected debris. The guarantee is that the next OPEN removes it
         * and never reads it, so that is what gets asserted here; checking
         * merely for its absence after a replay would be asserting something
         * the design never promised (wo_wal_replay does not open the WAL). */
        {
            wo_wal probe;
            T_EQ(wo_wal_open(&probe, path, 1 << 20), 0);
            T_CHECK(access(tmp, F_OK) != 0);
            wo_wal_close(&probe);
        }

        const char *msg = "";
        int bad = 0, checked = 0;
        for (size_t k = 0; k < n && !bad; k++) {
            if (vals[k] == CK_DELETED) continue; /* intent: fate is unknown */
            /* an id ever announced for deletion may legally be gone */
            int doomed = 0;
            for (size_t j = 0; j < n; j++)
                if (ids[j] == ids[k] && vals[j] == CK_DELETED) { doomed = 1; break; }
            if (doomed) continue;
            uint64_t out[2];
            int rc = wo_row_read(&db, &rt, 0, ids[k], out, &msg);
            if (0) {
            } else if (rc != 0 || out[0] != vals[k]) {
                bad = 1;              /* an acked insert is missing or wrong */
                fprintf(stderr, "CKDIAG round=%d id=%llu rc=%d got=%llu want=%llu ack#%zu/%zu "
                        "log_records=%lld replay_applied=%lld\n",
                        round, (unsigned long long)ids[k], rc,
                        rc == 0 ? (unsigned long long)out[0] : 0ull,
                        (unsigned long long)vals[k], k, n,
                        (long long)ck_recs, (long long)ck_applied);
            } else {
                wo_str_free(&rt, (wo_str *)(uintptr_t)out[1]);
            }
            checked++;
        }
        T_CHECK(checked > 0);
        T_CHECK(!bad);
        wo_db_destroy(&db);
        wo_rt_destroy(&rt);
    }
}

/* databasev2 2 (5c): the keys-resident round trip. A row is inserted, its
 * record committed, its PAYLOAD DROPPED from the slab, and then read back out
 * of the log by offset — including its heap-valued column, which is the case
 * that would silently return garbage if the materialisation were wrong. */
static const uint8_t keys_kinds[] = {WO_K_SCALAR, WO_K_TEXT};
static const wo_classdesc KEYS_CLASSES[] = {
    {.name = 0, .flags = WO_CLASSF_RESIDENT_KEYS, .field_cnt = 2, .kinds = keys_kinds},
};

static void test_keys_resident_round_trip(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/keysres.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, KEYS_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, KEYS_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db.rt = &rt;        /* the loop a borrow reads the WAL through */
    rt.wal = &w;
    rt.db = &db;
    const char *msg = "";

    T_CHECK(wo_table_is_keys_resident(&db, 0) == 1);

    wo_str *s = wo_str_new(&rt, "hello", 5);
    uint64_t vals[2] = {4242, (uint64_t)(uintptr_t)s};
    uint64_t id = wo_row_insert(&db, 0, vals, &msg, NULL);
    T_CHECK(id != 0);

    /* the offset this record WILL occupy — valid because the commit below
     * succeeds; a failed commit is fatal since databasev2 4 */
    uint64_t off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db, 0, id), 0);
    T_EQ(wo_wal_commit(&w), 0);

    /* while still resident, the row reads out of the slab */
    db_row *res = wo_row_borrow(&db, 0, id, &msg);
    T_CHECK(res != NULL && res->slots[0] == 4242);
    wo_row_release(&db, 0, res);

    /* drop the payload: slot freed, id kept, indexes untouched, still live */
    uint64_t before = db.tables[0].count;
    T_EQ(wo_row_drop_payload(&db, 0, id, off), 0);
    T_CHECK(db.tables[0].count == before); /* still LIVE, only unbacked */

    /* and now it comes back out of the LOG */
    db_row *r = wo_row_borrow(&db, 0, id, &msg);
    T_CHECK(r != NULL);
    T_CHECK(r->id == id);
    T_CHECK(r->slots[0] == 4242);
    /* engine-encoded, matching wo_row_ptr's contract (table.h's "a row
       stores NO VM pointer" doctrine) — db_text, not wo_str */
    db_text *back = (db_text *)(uintptr_t)r->slots[1];
    T_CHECK(back != NULL && back->len == 5 && memcmp(back->bytes, "hello", 5) == 0);
    wo_row_release(&db, 0, r);

    /* the scratch is reusable: a second borrow must succeed, which it cannot
     * if release failed to clear the busy flag */
    db_row *again = wo_row_borrow(&db, 0, id, &msg);
    T_CHECK(again != NULL && again->slots[0] == 4242);
    wo_row_release(&db, 0, again);

    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

/* databasev2 (task 1 of keys-resident delta updates): the delta record kind.
 * A keys-resident row cannot be rewritten whole to update one field (its
 * payload may already be gone from RAM), so a delta logs just the changed
 * field plus a back-pointer to the row's previous record. Nothing reads
 * deltas back yet — this only proves the encoder's bytes are what the format
 * says: kind, class, id, field index, back-pointer, value.
 *
 * All-scalar 3-field class, dedicated to this test (not the shared
 * KEYS_CLASSES): field_idx and back_off must each be a distinguishable
 * nonzero value or a transposition between the u32 field_idx and the u64
 * back_off is invisible (both would print as zero bytes either way). A
 * scalar-only row keeps every field a fixed 8 bytes, so a third field gives
 * a nonzero field_idx without a Text value's variable-length encoding
 * complicating the fixed body-size assertion below. class_id stays 0: this
 * fixture registers exactly one class, so there is no other value to give it
 * without fabricating an unused second class purely to shift an index. */
static const uint8_t delta_kinds[] = {WO_K_SCALAR, WO_K_SCALAR, WO_K_SCALAR};
static const wo_classdesc DELTA_CLASSES[] = {
    {.name = 0, .flags = WO_CLASSF_RESIDENT_KEYS, .field_cnt = 3, .kinds = delta_kinds},
};

static void test_delta_record(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/delta.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, DELTA_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, DELTA_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db.rt = &rt; rt.wal = &w; rt.db = &db;
    const char *msg = "";

    /* filler row+record so the TARGET row's insert lands at a nonzero
     * offset — a fresh WAL's first record is at offset 0, which would make
     * back_off indistinguishable from a zeroed field either way */
    uint64_t filler_vals[3] = {1, 2, 3};
    uint64_t filler_id = wo_row_insert(&db, 0, filler_vals, &msg, NULL);
    T_CHECK(filler_id != 0);
    T_EQ(wo_wal_append_insert(&w, &db, 0, filler_id), 0);
    T_EQ(wo_wal_commit(&w), 0);

    uint64_t vals[3] = {111, 222, 555};
    uint64_t id = wo_row_insert(&db, 0, vals, &msg, NULL);
    T_CHECK(id != 0);
    uint64_t base_off = wo_wal_next_offset(&w);
    T_CHECK(base_off != 0); /* the filler pushed this past offset 0 */
    T_EQ(wo_wal_append_insert(&w, &db, 0, id), 0);
    T_EQ(wo_wal_commit(&w), 0);

    /* field 2 (scalar) changes from 555 to 999; back-pointer is the insert
     * record this delta supersedes. field_idx=2 and back_off=base_off are
     * both nonzero and distinct from each other and from class_id=0, so a
     * field_idx/back_off transposition changes the read-back bytes. */
    uint64_t delta_off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_delta(&w, &db, 0, id, 2, base_off, 999), 0);
    T_EQ(wo_wal_commit(&w), 0);

    /* payload: kind u8 | class u32 | id u64 | field_idx u32 | back_off u64 |
     * value u64 (scalar) — 33 bytes, after the 8-byte len+crc head */
    uint8_t head[8], body[33];
    T_EQ((int)pread(w.fd, head, 8, (off_t)delta_off), 8);
    uint32_t len;
    memcpy(&len, head, 4);
    T_EQ(len, 33u);
    T_EQ((int)pread(w.fd, body, 33, (off_t)(delta_off + 8)), 33);
    T_EQ(body[0], WO_WAL_DELTA);
    uint32_t cid;
    uint64_t rid, back, val;
    uint32_t fidx;
    memcpy(&cid, body + 1, 4);
    memcpy(&rid, body + 5, 8);
    memcpy(&fidx, body + 13, 4);
    memcpy(&back, body + 17, 8);
    memcpy(&val, body + 25, 8);
    T_EQ(cid, 0u);
    T_EQ(rid, id);
    T_EQ(fidx, 2u);
    T_EQ(back, base_off);
    T_EQ(val, 999u);

    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

/* keys-resident delta updates, Task 2: the fold. A row's current record may
 * be a chain of deltas, not a base row — wo_row_borrow must walk back
 * through them, remembering one value per touched field, and overlay them
 * onto the base row it eventually reaches. Reuses DELTA_CLASSES (3 scalar
 * fields) so field 1 can stay untouched by any delta and prove the fold
 * does not clobber fields nobody changed. */
static void test_delta_fold_two_fields(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/foldtwo.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, DELTA_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, DELTA_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db.rt = &rt;
    rt.wal = &w;
    rt.db = &db;
    const char *msg = "";

    uint64_t vals[3] = {10, 20, 30};
    uint64_t id = wo_row_insert(&db, 0, vals, &msg, NULL);
    T_CHECK(id != 0);
    uint64_t base_off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db, 0, id), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_drop_payload(&db, 0, id, base_off), 0);

    /* field 0: 10 -> 111 */
    uint64_t d1_off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_delta(&w, &db, 0, id, 0, base_off, 111), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_set_offset(&db, 0, id, d1_off), 0);

    /* field 2: 30 -> 333, chained off the first delta */
    uint64_t d2_off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_delta(&w, &db, 0, id, 2, d1_off, 333), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_set_offset(&db, 0, id, d2_off), 0);

    db_row *r = wo_row_borrow(&db, 0, id, &msg);
    T_CHECK(r != NULL);
    T_CHECK(r->slots[0] == 111); /* changed */
    T_CHECK(r->slots[1] == 20);  /* untouched: original survives */
    T_CHECK(r->slots[2] == 333); /* changed */
    wo_row_release(&db, 0, r);

    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

/* The ordering rule: two deltas to the SAME field. A fold walking the chain
 * in the wrong direction sees the OLDER delta first and stops there — a
 * plausible-looking but stale value, invisible unless a test pins the
 * direction explicitly. */
static void test_delta_fold_same_field_newest_wins(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/foldsame.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, DELTA_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, DELTA_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db.rt = &rt;
    rt.wal = &w;
    rt.db = &db;
    const char *msg = "";

    uint64_t vals[3] = {1, 2, 3};
    uint64_t id = wo_row_insert(&db, 0, vals, &msg, NULL);
    T_CHECK(id != 0);
    uint64_t base_off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db, 0, id), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_drop_payload(&db, 0, id, base_off), 0);

    /* field 1: 2 -> 20 (older) -> 200 (newer) */
    uint64_t d1_off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_delta(&w, &db, 0, id, 1, base_off, 20), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_set_offset(&db, 0, id, d1_off), 0);

    uint64_t d2_off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_delta(&w, &db, 0, id, 1, d1_off, 200), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_set_offset(&db, 0, id, d2_off), 0);

    db_row *r = wo_row_borrow(&db, 0, id, &msg);
    T_CHECK(r != NULL);
    T_CHECK(r->slots[0] == 1);
    T_CHECK(r->slots[1] == 200); /* the NEWER delta wins, not the older */
    T_CHECK(r->slots[2] == 3);
    wo_row_release(&db, 0, r);

    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

/* keys-resident delta updates, Task 2 (review follow-up): a back-pointer
 * naming ITS OWN offset is the boundary case of the fold's invariant —
 * every hop must land on a STRICTLY earlier offset than the record naming
 * it. back_off == cur violates that on the very first hop and must be
 * refused immediately, not walked. */
static void test_fold_refuses_self_pointing_delta(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/foldself.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, DELTA_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, DELTA_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db.rt = &rt;
    rt.wal = &w;
    rt.db = &db;
    const char *msg = "";

    uint64_t vals[3] = {10, 20, 30};
    uint64_t id = wo_row_insert(&db, 0, vals, &msg, NULL);
    T_CHECK(id != 0);

    /* a delta whose back-pointer names ITS OWN offset */
    uint64_t delta_off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_delta(&w, &db, 0, id, 0, delta_off, 999), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_drop_payload(&db, 0, id, delta_off), 0);

    T_CHECK(wo_row_borrow(&db, 0, id, &msg) == NULL);

    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

/* The case a mere chain-length bound cannot rule out: a back-pointer that
 * points FORWARD to a real, valid record for the SAME row. Nothing about
 * this loops, so a cap on chain length would let it straight through in
 * one hop and return a plausible-but-wrong answer. Only checking that
 * every hop moves to a STRICTLY earlier offset catches it, immediately. */
static void test_fold_refuses_forward_pointing_delta(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/foldfwd.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, DELTA_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, DELTA_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db.rt = &rt;
    rt.wal = &w;
    rt.db = &db;
    const char *msg = "";

    /* filler row/record, same reason as test_delta_record's: a fresh WAL's
       first record sits at offset 0, which would make the forged delta's
       own offset indistinguishable from a zeroed field either way. */
    uint64_t filler_vals[3] = {1, 2, 3};
    uint64_t filler_id = wo_row_insert(&db, 0, filler_vals, &msg, NULL);
    T_CHECK(filler_id != 0);
    T_EQ(wo_wal_append_insert(&w, &db, 0, filler_id), 0);
    T_EQ(wo_wal_commit(&w), 0);

    uint64_t vals[3] = {10, 20, 30};
    uint64_t id = wo_row_insert(&db, 0, vals, &msg, NULL);
    T_CHECK(id != 0);

    /* forge a delta BEFORE the row's real base record exists, naming the
       offset the base record WILL occupy right after it. A scalar-field
       delta payload is kind|class|id|field_idx|back_off|value(u64) = 33
       bytes (established by test_delta_record); the frame is 8+33+4 = 45. */
    uint64_t delta_off = wo_wal_next_offset(&w);
    uint64_t insert_off = delta_off + 45u;
    T_EQ(wo_wal_append_delta(&w, &db, 0, id, 0, insert_off, 999), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_wal_next_offset(&w), insert_off); /* the hand-computed frame size held */

    /* the row's TRUE base record, landing exactly where the forged delta
       claimed — the row is still in RAM, so this is an ordinary insert-log */
    T_EQ(wo_wal_append_insert(&w, &db, 0, id), 0);
    T_EQ(wo_wal_commit(&w), 0);

    /* point the row at the forged, forward-pointing delta */
    T_EQ(wo_row_drop_payload(&db, 0, id, delta_off), 0);

    /* the fold must refuse — not silently return {999, 20, 30} by walking
       forward into the base record the forged back-pointer named */
    T_CHECK(wo_row_borrow(&db, 0, id, &msg) == NULL);

    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

/* Task 3 (keys-resident delta updates): the plain case, through the real
 * API — wo_row_update_field, not a hand-rolled append+commit+set_offset like
 * the fold tests above. Before this task it refused outright with "update on
 * a `resident: keys` table is not implemented".
 *
 * Task 4 ruling: wo_row_update_field now only STAGES the delta and applies
 * the index swap — it does not commit and does not move the id map (that
 * mirrors insert, whose koff/commit/pend_drop live in the CALLER). So this
 * test now does the caller's half itself, exactly as db.c's inline arm
 * does: capture the offset before calling in, commit, then re-point. */
static void test_keys_resident_update_field(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/keysupd.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, KEYS_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, KEYS_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db.rt = &rt; rt.wal = &w; rt.db = &db;
    const char *msg = "";

    wo_str *s = wo_str_new(&rt, "hello", 5);
    uint64_t vals[2] = {111, (uint64_t)(uintptr_t)s};
    uint64_t id = wo_row_insert(&db, 0, vals, &msg, NULL);
    T_CHECK(id != 0);
    uint64_t off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db, 0, id), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_drop_payload(&db, 0, id, off), 0);

    int ek = 0;
    uint64_t roff = wo_wal_next_offset(&w);
    T_EQ(wo_row_update_field(&db, 0, id, 0, 999, &msg, &ek), 0);
    T_EQ(ek, DB_ERR_NONE);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_set_offset(&db, 0, id, roff), 0);

    db_row *r = wo_row_borrow(&db, 0, id, &msg);
    T_CHECK(r != NULL);
    T_CHECK(r->slots[0] == 999);
    db_text *back = (db_text *)(uintptr_t)r->slots[1];
    T_CHECK(back != NULL && back->len == 5 && memcmp(back->bytes, "hello", 5) == 0);
    wo_row_release(&db, 0, r);

    /* the scratch must be free again — a release that skipped clearing
       scratch_busy would wedge this second borrow */
    db_row *again = wo_row_borrow(&db, 0, id, &msg);
    T_CHECK(again != NULL && again->slots[0] == 999);
    wo_row_release(&db, 0, again);

    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

/* class 0: Row { n: scalar @index(non-unique), label: Text } — a keys-resident
 * table with a secondary index on the scalar column, dedicated to the test
 * below. The design deliberately allows a delta to change an indexed column
 * (a catalogue indexes exactly the columns that change, like `price`), so
 * this is the realistic case. */
static const uint8_t keys_idx_kinds[] = {WO_K_SCALAR, WO_K_TEXT};
static const uint32_t keys_idx_meta[] = {0 /*non-unique*/, 1, 0 /*col: n*/};
static const wo_classdesc KEYS_IDX_CLASSES[] = {
    {.name = 0, .flags = WO_CLASSF_RESIDENT_KEYS, .field_cnt = 2, .kinds = keys_idx_kinds,
     .idx_cnt = 1, .idx_meta = keys_idx_meta},
};

/* 2026-09-10 defect: the FIRST keys-resident row of a FRESH log. Boot adopts
 * the compiled schema (main.c: wo_wal_set_schema, never wo_wal_ensure_schema)
 * and the head record is staged lazily ahead of the first real record.
 * db.c's insert arm captures the row's offset with wo_wal_next_offset BEFORE
 * the append, so the head must already be staged by then — otherwise `koff`
 * names the schema record and the row's first read folds "record header is
 * malformed"; through wo_idx_probe (borrow with msg == NULL) that was a
 * zero-page write — the residency example's `seed` died rc 139. Call for
 * call the db.c:78 sequence, then the two reads `seed` performs. */
static void test_keys_resident_fresh_log_first_row(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/keysfresh.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, KEYS_IDX_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, KEYS_IDX_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db.rt = &rt; rt.wal = &w; rt.db = &db;
    const char *msg = "";

    static wo_schema_field f[] = {
        {(const uint8_t *)"n", 1, WO_K_SCALAR, WO_SCHEMA_NONE, WO_SCHEMA_NONE},
        {(const uint8_t *)"label", 5, WO_K_TEXT, WO_SCHEMA_NONE, WO_SCHEMA_NONE},
    };
    static wo_schema_class cls[] = {{(const uint8_t *)"row", 3, WO_CLASSF_RESIDENT_KEYS, 2, f}};
    wo_schema sc = {1, cls, NULL};
    T_EQ(wo_wal_set_schema(&w, &sc), 0);
    T_EQ(wo_wal_read_schema(path, NULL, NULL), 1); /* still zero bytes: lazy */

    /* db.c's insert arm, call for call */
    wo_str *s = wo_str_new(&rt, "sku", 3);
    uint64_t vals[2] = {10, (uint64_t)(uintptr_t)s};
    uint64_t id = wo_row_insert(&db, 0, vals, &msg, NULL);
    T_CHECK(id != 0);
    uint64_t koff = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db, 0, id), 0);
    T_EQ(wo_wal_pend_drop(&w, 0, id, koff), 0);
    T_EQ(wo_wal_commit(&w), 0);
    wo_db_flush_drops(&db, &w);
    wo_str_free(&rt, s);

    /* the log describes itself AND koff names the row, not the head */
    T_EQ(wo_wal_read_schema(path, NULL, NULL), 0);
    T_CHECK(koff != 0);
    uint32_t at_cid = 99;
    uint64_t at_id = 0, at[2];
    T_EQ(wo_wal_read_row_at(&w, &db, &rt, koff, &at_cid, &at_id, at, &msg), 0);
    T_CHECK(at_cid == 0 && at_id == id);
    if (at_cid == 0 && at_id == id) wo_str_free(&rt, (wo_str *)(uintptr_t)at[1]);

    /* seed's first read: through the id map */
    uint64_t out[2];
    msg = "";
    int rrc = wo_row_read(&db, &rt, 0, id, out, &msg);
    T_EQ(rrc, 0);
    if (rrc != 0) fprintf(stderr, "  wo_row_read: %s\n", msg);
    else {
        T_EQ(out[0], 10);
        wo_str_free(&rt, (wo_str *)(uintptr_t)out[1]);
    }

    /* seed's second read: the index probe borrows with msg == NULL */
    uint64_t *ids;
    uint32_t cnt;
    T_EQ(wo_idx_probe(&db, 0, 0, 10, NULL, 0, &ids, &cnt), 1);
    T_CHECK(cnt == 1 && ids[0] == id);
    free(ids);

    /* and a restart sees one row behind the head, readable by offset again */
    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_db db2;
    T_EQ(wo_db_init(&db2, KEYS_IDX_CLASSES, 1, 0, 1), 0);
    db2.rt = &rt; rt.wal = NULL; rt.db = &db2;
    T_EQ(wo_wal_replay(path, &db2), 1);
    wo_wal w2;
    T_EQ(wo_wal_open(&w2, path, 1 << 16), 0);
    rt.wal = &w2;
    db_row *r = wo_row_borrow(&db2, 0, id, &msg);
    T_CHECK(r != NULL && r->slots[0] == 10);
    wo_row_release(&db2, 0, r);
    wo_wal_close(&w2);
    wo_db_destroy(&db2);
    wo_rt_destroy(&rt);
}

/* Task 3, the test that matters: updating an INDEXED column on a
 * keys-resident row must move the row in the index too, not just in the
 * log — queried through wo_idx_probe, the row is found by its NEW value and
 * gone from its OLD one.
 *
 * Task 4 ruling: wo_idx_probe's bucket hit is verified by folding the row
 * from the log (table.c's idx_cols_equal path), so the probes below must
 * run AFTER the caller's commit + re-point — mirroring db.c's inline arm —
 * not straight after wo_row_update_field, which now only stages. */
/* databasev2 11: helper — drive one update through the full commit/re-point
 * dance the request path performs, so a chain can be built in a loop. */
static void chain_update(wo_db *db, wo_wal *w, uint32_t cid, uint64_t id,
                         uint32_t field, uint64_t v) {
    const char *msg = "";
    int ek = 0;
    uint64_t roff = wo_wal_next_offset(w);
    T_EQ(wo_row_update_field(db, cid, id, field, v, &msg, &ek), 0);
    T_EQ(ek, DB_ERR_NONE);
    T_EQ(wo_wal_commit(w), 0);
    T_EQ(wo_row_set_offset(db, cid, id, roff), 0);
}

/* databasev2 11: the branch this iteration exists for. Past WO_DELTA_MAX_HOPS
 * an update must TERMINATE the chain with a full-row record rather than
 * lengthening it — otherwise read cost and replay cost grow without bound,
 * because compaction's trigger is a whole-log byte ratio and cannot see one
 * row's chain.
 *
 * The assertion is on the DEPTH the fold reports, not on timing: a test that
 * measured speed would pass on a slow box with an unbounded chain. */
/* databasev2 11 tier 2: the compaction policy's two new terms. A pure
 * function, so this is cheap and exact — no log, no timing.
 *
 * The trap being guarded: our `floor` SUPPRESSES compaction on a small log,
 * the opposite of PostgreSQL's vac_base_thresh, which TRIGGERS on a small
 * absolute problem the proportion would hide. Before this iteration we had the
 * proportion and the suppressor and neither real guard. */
static void test_should_compact_absolute_and_ceiling(void) {
    const uint64_t floor_b = 1024;

    /* unchanged behaviour: below the floor, never */
    T_EQ(wo_wal_should_compact(512, 256, floor_b, 2), 0);
    /* unchanged: never compacted yet, past the floor -> once, to set a denominator */
    T_EQ(wo_wal_should_compact(4096, 0, floor_b, 2), 1);
    /* unchanged: ratio 0 disables the policy rather than dividing by nothing */
    T_EQ(wo_wal_should_compact(1u << 30, 1024, floor_b, 0), 0);

    /* THE ABSOLUTE TERM. A live set so large that the ratio will not trip for
     * a very long time, but with more than WO_CKPT_ABS_BYTES of garbage
     * already reclaimable. The old policy said no; the point of the term is
     * that garbage large in BYTES is worth reclaiming even when it is small in
     * PROPORTION. */
    {
        uint64_t live = 4ull * 1024 * 1024 * 1024;      /* 4 GiB live */
        uint64_t used = live + WO_CKPT_ABS_BYTES + 1;   /* just over the term */
        T_CHECK(used < live * 2);                       /* ratio 2 would NOT fire */
        T_EQ(wo_wal_should_compact(used, live, floor_b, 2), 1);
    }
    /* and just under it, the ratio still governs */
    {
        uint64_t live = 4ull * 1024 * 1024 * 1024;
        uint64_t used = live + (WO_CKPT_ABS_BYTES / 2);
        T_EQ(wo_wal_should_compact(used, live, floor_b, 2), 0);
    }

    /* DEFERRAL IS CAPPED by the same term — no separate ceiling exists, and
     * one was removed as unreachable. With an 8 GiB live set, ratio 2 would
     * wait for the log to double; the absolute term fires long before that. */
    {
        uint64_t live = 8ull * 1024 * 1024 * 1024;      /* 8 GiB live */
        uint64_t used = live + WO_CKPT_ABS_BYTES + 1;
        T_CHECK(used < live * 2);                       /* the ratio alone would defer */
        T_EQ(wo_wal_should_compact(used, live, floor_b, 2), 1);
    }

    /* the ratio still governs BELOW the absolute term, which is what keeps the
     * two complementary rather than one subsuming the other: a small live set
     * trips the ratio with far less garbage than 64 MiB */
    T_EQ(wo_wal_should_compact(3072, 1024, floor_b, 2), 1);   /* 3 KiB > 1 KiB * 2 */
    T_EQ(wo_wal_should_compact(2048, 1024, floor_b, 2), 0);   /* not yet */
}

/* ---- databasev2 12: the migration transcode ----------------------------- */

#define SF(nm, k) {(const uint8_t *)nm, (uint32_t)(sizeof nm - 1), k, WO_SCHEMA_NONE, WO_SCHEMA_NONE}
#define SFC(nm, k, fc) {(const uint8_t *)nm, (uint32_t)(sizeof nm - 1), k, fc, WO_SCHEMA_NONE}
#define SC(nm, fl, arr) {(const uint8_t *)nm, (uint32_t)(sizeof nm - 1), fl, \
                         (uint32_t)(sizeof arr / sizeof arr[0]), arr}


/* every migrate test speaks both sides: a classdesc array for the engine and
 * a wo_schema for the diff, built from the same literals */
static const uint8_t mig_nt_kinds[] = {WO_K_SCALAR, WO_K_TEXT};
static const wo_classdesc MIG_NT[] = {
    {.name = 0, .flags = 0, .field_cnt = 2, .kinds = mig_nt_kinds},
};
static const uint8_t mig_nte_kinds[] = {WO_K_SCALAR, WO_K_TEXT, WO_K_SCALAR};
static const wo_classdesc MIG_NTE[] = {
    {.name = 0, .flags = 0, .field_cnt = 3, .kinds = mig_nte_kinds},
};
static const uint8_t mig_n_kinds[] = {WO_K_SCALAR};
static const wo_classdesc MIG_N[] = {
    {.name = 0, .flags = 0, .field_cnt = 1, .kinds = mig_n_kinds},
};

static wo_schema_field mig_sf_n[] = {SF("n", WO_K_SCALAR)};
static wo_schema_field mig_sf_nt[] = {SF("n", WO_K_SCALAR), SF("t", WO_K_TEXT)};
static wo_schema_field mig_sf_nte[] = {SF("n", WO_K_SCALAR), SF("t", WO_K_TEXT),
                                       SF("extra", WO_K_SCALAR)};

static db_text *mig_text(const char *sz) {
    size_t n = strlen(sz);
    db_text *t = malloc(sizeof(db_text) + n);
    t->len = (uint32_t)n;
    memcpy(t->bytes, sz, n);
    return t;
}

/* REORDER + OWNED FIXUP: the classes swap declaration order and one of them
 * embeds the other by value. The record's outer cid AND the cid inside the
 * stored owned value must both be renumbered — the outer one alone would
 * decode the embedded value against the wrong class. */
static void test_migrate_reorder_owned(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/migreorder.wal", g_dir);
    static const uint8_t x_kinds[] = {WO_K_SCALAR};
    static const uint8_t c_kinds[] = {WO_K_OWNED, WO_K_SCALAR};
    static const wo_classdesc OLD_XC[] = {
        {.name = 0, .flags = 0, .field_cnt = 1, .kinds = x_kinds},
        {.name = 0, .flags = 0, .field_cnt = 2, .kinds = c_kinds},
    };
    static const wo_classdesc NEW_CX[] = {
        {.name = 0, .flags = 0, .field_cnt = 2, .kinds = c_kinds},
        {.name = 0, .flags = 0, .field_cnt = 1, .kinds = x_kinds},
    };
    static wo_schema_field sx[] = {SF("v", WO_K_SCALAR)};
    static wo_schema_field sc_old[] = {SFC("part", WO_K_OWNED, 0), SF("m", WO_K_SCALAR)};
    static wo_schema_field sc_new[] = {SFC("part", WO_K_OWNED, 1), SF("m", WO_K_SCALAR)};
    wo_schema_class oc[] = {SC("X", 0, sx), SC("C", 0, sc_old)};
    wo_schema oldsc = {2, oc, NULL};
    wo_schema_class nc[] = {SC("C", 0, sc_new), SC("X", 0, sx)};
    wo_schema newsc = {2, nc, NULL};

    {
        wo_db db;
        T_EQ(wo_db_init(&db, OLD_XC, 2, 0, 1), 0);
        wo_wal w;
        T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
        db_row *rx = wo_row_create_raw(&db, 0, 3); /* an X row, old cid 0 */
        rx->slots[0] = 7;
        T_EQ(wo_row_raw_commit(&db, 0, rx), 0);
        T_EQ(wo_wal_append_insert(&w, &db, 0, 3), 0);
        db_rec *part = malloc(sizeof(db_rec) + 8); /* embedded X, old cid 0 */
        part->class_id = 0;
        part->_pad = 0;
        part->slots[0] = 42;
        db_row *rc = wo_row_create_raw(&db, 1, 5); /* a C row, old cid 1 */
        rc->slots[0] = (uint64_t)(uintptr_t)part;
        rc->slots[1] = 9;
        T_EQ(wo_row_raw_commit(&db, 1, rc), 0);
        T_EQ(wo_wal_append_insert(&w, &db, 1, 5), 0);
        T_EQ(wo_wal_commit(&w), 0);
        wo_wal_close(&w);
        wo_db_destroy(&db);
    }
    wo_db db2;
    T_EQ(wo_db_init(&db2, NEW_CX, 2, 0, 1), 0);
    wo_mig_plan pl;
    T_EQ(wo_schema_diff(&oldsc, &newsc, &pl), 0);
    T_EQ(pl.identity, 0);
    T_EQ(pl.classes[0].new_cid, 1u);
    T_EQ(pl.classes[1].new_cid, 0u);
    T_CHECK(pl.classes[0].poison == NULL && pl.classes[1].poison == NULL);
    T_EQ(wo_wal_migrate(path, &db2, &oldsc, &pl, &newsc, 1 << 16, NULL), 0);
    wo_mig_plan_free(&pl);
    T_EQ(wo_wal_replay(path, &db2), 2);
    db_row *rx = wo_row_ptr(&db2, 1, 3); /* X lives at cid 1 now */
    T_CHECK(rx != NULL && rx->slots[0] == 7);
    db_row *rc = wo_row_ptr(&db2, 0, 5); /* C lives at cid 0 now */
    T_CHECK(rc != NULL && rc->slots[1] == 9);
    db_rec *part = (db_rec *)(uintptr_t)rc->slots[0];
    T_CHECK(part != NULL && part->class_id == 1 && part->slots[0] == 42);
    wo_db_destroy(&db2);
}

/* DELTA SPLICE: a keys-resident row's chain carries deltas on a field that is
 * being DELETED. The spliced chain must still fold — later deltas re-point
 * around the dropped ones — and the surviving field's latest value wins. */
static void test_migrate_delta_splice(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/migsplice.wal", g_dir);
    static wo_schema_field sk_old[] = {SF("n", WO_K_SCALAR), SF("label", WO_K_TEXT)};
    static wo_schema_field sk_new[] = {SF("n", WO_K_SCALAR)};
    wo_schema_class oc[] = {SC("K", WO_CLASSF_RESIDENT_KEYS, sk_old)};
    wo_schema oldsc = {1, oc, NULL};
    wo_schema_class nc[] = {SC("K", WO_CLASSF_RESIDENT_KEYS, sk_new)};
    wo_schema newsc = {1, nc, NULL};
    static const uint8_t knew_kinds[] = {WO_K_SCALAR};
    static const wo_classdesc KNEW[] = {
        {.name = 0,
         .flags = WO_CLASSF_RESIDENT_KEYS,
         .field_cnt = 1,
         .kinds = knew_kinds},
    };

    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, KEYS_CLASSES, 1), 0);
    uint64_t id;
    {
        wo_db db;
        T_EQ(wo_db_init(&db, KEYS_CLASSES, 1, 0, 1), 0);
        wo_wal w;
        T_EQ(wo_wal_open(&w, path, 1 << 18), 0);
        db.rt = &rt;
        rt.wal = &w;
        rt.db = &db;
        const char *msg = "";
        wo_str *sa = wo_str_new(&rt, "a", 1);
        uint64_t vals[2] = {1, (uint64_t)(uintptr_t)sa};
        id = wo_row_insert(&db, 0, vals, &msg, NULL);
        T_CHECK(id != 0);
        uint64_t off = wo_wal_next_offset(&w);
        T_EQ(wo_wal_append_insert(&w, &db, 0, id), 0);
        T_EQ(wo_wal_commit(&w), 0);
        T_EQ(wo_row_drop_payload(&db, 0, id, off), 0);
        /* the chain: n=2, label="x" (doomed), n=3 — the last delta's back
           pointer crosses the doomed one */
        chain_update(&db, &w, 0, id, 0, 2);
        wo_str *sx = wo_str_new(&rt, "x", 1);
        chain_update(&db, &w, 0, id, 1, (uint64_t)(uintptr_t)sx);
        chain_update(&db, &w, 0, id, 0, 3);
        wo_wal_close(&w);
        wo_db_destroy(&db);
    }

    wo_db db2;
    T_EQ(wo_db_init(&db2, KNEW, 1, 0, 1), 0);
    wo_mig_plan pl;
    T_EQ(wo_schema_diff(&oldsc, &newsc, &pl), 0);
    T_CHECK(pl.classes[0].poison == NULL && pl.classes[0].fmap[1] == -1);
    T_EQ(wo_wal_migrate(path, &db2, &oldsc, &pl, &newsc, 1 << 18, NULL), 0);
    wo_mig_plan_free(&pl);

    /* replay the migrated log the way boot does for a keys table, then read
       the row back through the fold: the chain must resolve to n=3 */
    db2.rt = &rt;
    rt.wal = NULL;
    rt.db = &db2;
    T_CHECK(wo_wal_replay(path, &db2) >= 0);
    wo_wal w2;
    T_EQ(wo_wal_open(&w2, path, 1 << 18), 0);
    rt.wal = &w2;
    const char *msg = "";
    db_row *r = wo_row_borrow(&db2, 0, id, &msg);
    T_CHECK(r != NULL && r->slots[0] == 3);
    wo_row_release(&db2, 0, r);
    wo_wal_close(&w2);
    wo_db_destroy(&db2);
    wo_rt_destroy(&rt);
}

/* ADD: a two-field log boots a three-field binary — rows survive, the new
 * field reads the kind's zero, the head record states the NEW shape, and a
 * stale compaction temp lying beside the log is discarded, not appended to */
static void test_migrate_add_field(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/migadd.wal", g_dir);
    wo_schema_class oc[] = {SC("row", 0, mig_sf_nt)};
    wo_schema oldsc = {1, oc, NULL};
    wo_schema_class nc[] = {SC("row", 0, mig_sf_nte)};
    wo_schema newsc = {1, nc, NULL};

    /* the OLD program writes its log, schema record at the head */
    {
        wo_db db;
        T_EQ(wo_db_init(&db, MIG_NT, 1, 0, 1), 0);
        wo_wal w;
        T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
        T_EQ(wo_wal_set_schema(&w, &oldsc), 0);
        T_EQ(wo_wal_ensure_schema(&w), 0);
        db_row *r1 = wo_row_create_raw(&db, 0, 1);
        r1->slots[0] = 7;
        r1->slots[1] = (uint64_t)(uintptr_t)mig_text("abc");
        T_EQ(wo_row_raw_commit(&db, 0, r1), 0);
        T_EQ(wo_wal_append_insert(&w, &db, 0, 1), 0);
        db_row *r2 = wo_row_create_raw(&db, 0, 2);
        r2->slots[0] = 9;
        r2->slots[1] = 0;
        T_EQ(wo_row_raw_commit(&db, 0, r2), 0);
        T_EQ(wo_wal_append_insert(&w, &db, 0, 2), 0);
        T_EQ(wo_wal_commit(&w), 0);
        T_EQ(wo_row_remove(&db, 0, 2), 0);
        T_EQ(wo_wal_append_remove(&w, 0, 2), 0);
        T_EQ(wo_wal_commit(&w), 0);
        wo_wal_close(&w);
        wo_db_destroy(&db);
    }
    /* a stale temp beside the log: never authoritative, must be discarded */
    {
        char tmp[160];
        snprintf(tmp, sizeof tmp, "%s.compact", path);
        FILE *f = fopen(tmp, "w");
        T_CHECK(f != NULL);
        fputs("stale-not-a-record", f);
        fclose(f);
    }

    /* the NEW binary migrates it at boot */
    wo_db db3;
    T_EQ(wo_db_init(&db3, MIG_NTE, 1, 0, 1), 0);
    wo_mig_plan pl;
    T_EQ(wo_schema_diff(&oldsc, &newsc, &pl), 0);
    T_EQ(pl.identity, 0);
    T_CHECK(pl.classes[0].poison == NULL);
    char *err = NULL;
    T_EQ(wo_wal_migrate(path, &db3, &oldsc, &pl, &newsc, 1 << 16, &err), 0);
    T_CHECK(err == NULL);
    wo_mig_plan_free(&pl);

    /* head record: the new three-field shape */
    uint8_t *sp;
    uint32_t slen;
    T_EQ(wo_wal_read_schema(path, &sp, &slen), 0);
    wo_schema *head = wo_schema_decode(sp, slen);
    T_CHECK(head != NULL && head->classes[0].field_cnt == 3);
    wo_schema_free(head);
    free(sp);

    /* replay: row 1 intact with a zero-valued third field, row 2 gone */
    T_EQ(wo_wal_replay(path, &db3), 3); /* insert, insert, remove */
    db_row *r = wo_row_ptr(&db3, 0, 1);
    T_CHECK(r != NULL && r->slots[0] == 7);
    db_text *t = (db_text *)(uintptr_t)r->slots[1];
    T_CHECK(t != NULL && t->len == 3 && memcmp(t->bytes, "abc", 3) == 0);
    T_EQ(r->slots[2], 0u);
    T_CHECK(wo_row_ptr(&db3, 0, 2) == NULL);
    wo_db_destroy(&db3);
}

/* DELETE: the Text column's stored values are freed (ASan holds the leash)
 * and the surviving field lands in its new slot */
static void test_migrate_delete_field(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/migdel.wal", g_dir);
    wo_schema_class oc[] = {SC("row", 0, mig_sf_nt)};
    wo_schema oldsc = {1, oc, NULL};
    wo_schema_class nc[] = {SC("row", 0, mig_sf_n)};
    wo_schema newsc = {1, nc, NULL};
    {
        wo_db db;
        T_EQ(wo_db_init(&db, MIG_NT, 1, 0, 1), 0);
        wo_wal w;
        T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
        for (uint64_t id = 1; id <= 20; id++) {
            db_row *r = wo_row_create_raw(&db, 0, id);
            r->slots[0] = id * 10;
            r->slots[1] = (uint64_t)(uintptr_t)mig_text("payload-to-drop");
            T_EQ(wo_row_raw_commit(&db, 0, r), 0);
            T_EQ(wo_wal_append_insert(&w, &db, 0, id), 0);
        }
        T_EQ(wo_wal_commit(&w), 0);
        wo_wal_close(&w);
        wo_db_destroy(&db);
    }
    wo_db db2;
    T_EQ(wo_db_init(&db2, MIG_N, 1, 0, 1), 0);
    wo_mig_plan pl;
    T_EQ(wo_schema_diff(&oldsc, &newsc, &pl), 0);
    T_CHECK(pl.classes[0].poison == NULL && pl.classes[0].fmap[1] == -1);
    T_EQ(wo_wal_migrate(path, &db2, &oldsc, &pl, &newsc, 1 << 16, NULL), 0);
    wo_mig_plan_free(&pl);
    T_EQ(wo_wal_replay(path, &db2), 20);
    for (uint64_t id = 1; id <= 20; id++) {
        db_row *r = wo_row_ptr(&db2, 0, id);
        T_CHECK(r != NULL && r->slots[0] == id * 10);
    }
    wo_db_destroy(&db2);
}

/* CRASH BETWEEN WRITE AND RENAME: the sharpest point on the timeline — a
 * COMPLETE, VALID migrated log sits beside the original as the temp, the
 * rename never happened. The next boot must treat the temp as the nothing it
 * is (its records were never authoritative) and re-migrate from the intact
 * original. A garbage temp tests the unlink; a valid one tests the doctrine. */
static void test_migrate_crash_before_rename(void) {
    char pa[128], pb[160], tmp[160];
    snprintf(pa, sizeof pa, "%s/migcrash.wal", g_dir);
    snprintf(pb, sizeof pb, "%s/migcrash-copy.wal", g_dir);
    snprintf(tmp, sizeof tmp, "%s.compact", pa);
    wo_schema_class oc[] = {SC("row", 0, mig_sf_nt)};
    wo_schema oldsc = {1, oc, NULL};
    wo_schema_class nc[] = {SC("row", 0, mig_sf_nte)};
    wo_schema newsc = {1, nc, NULL};
    {
        wo_db db;
        T_EQ(wo_db_init(&db, MIG_NT, 1, 0, 1), 0);
        wo_wal w;
        T_EQ(wo_wal_open(&w, pa, 0), 0);
        db_row *r = wo_row_create_raw(&db, 0, 1);
        r->slots[0] = 5;
        r->slots[1] = (uint64_t)(uintptr_t)mig_text("keep");
        T_EQ(wo_row_raw_commit(&db, 0, r), 0);
        T_EQ(wo_wal_append_insert(&w, &db, 0, 1), 0);
        T_EQ(wo_wal_commit(&w), 0);
        wo_wal_close(&w);
        wo_db_destroy(&db);
    }
    wo_mig_plan pl;
    T_EQ(wo_schema_diff(&oldsc, &newsc, &pl), 0);
    /* produce the "crashed" state: migrate a COPY, then plant its result as
       the original's temp — exactly what a kill after fsync, before rename,
       leaves on disk */
    {
        FILE *a = fopen(pa, "rb"), *b = fopen(pb, "wb");
        T_CHECK(a && b);
        int ch;
        while ((ch = fgetc(a)) != EOF) fputc(ch, b);
        fclose(a);
        fclose(b);
        wo_db dbn;
        T_EQ(wo_db_init(&dbn, MIG_NTE, 1, 0, 1), 0);
        T_EQ(wo_wal_migrate(pb, &dbn, &oldsc, &pl, &newsc, 0, NULL), 0);
        wo_db_destroy(&dbn);
        T_EQ(rename(pb, tmp), 0);
    }
    /* the next boot: re-migrates from the intact original, result correct */
    wo_db db2;
    T_EQ(wo_db_init(&db2, MIG_NTE, 1, 0, 1), 0);
    T_EQ(wo_wal_migrate(pa, &db2, &oldsc, &pl, &newsc, 0, NULL), 0);
    wo_mig_plan_free(&pl);
    T_EQ(wo_wal_replay(pa, &db2), 1);
    db_row *r = wo_row_ptr(&db2, 0, 1);
    T_CHECK(r != NULL && r->slots[0] == 5 && r->slots[2] == 0);
    db_text *t = (db_text *)(uintptr_t)r->slots[1];
    T_CHECK(t != NULL && t->len == 4 && memcmp(t->bytes, "keep", 4) == 0);
    wo_db_destroy(&db2);
}

/* databasev2 7 Task 2: with WO_DATA naming a FILE, compaction (databasev2 3)
 * and migration (databasev2 12) must build their temp as `<file>.compact`
 * beside it and take the parent they fsync from the FILE's path — both derive
 * everything from the log path today, and this pins that against a future
 * "derive it from WO_DATA". The proof is a blocker, not a listing: a
 * DIRECTORY planted at exactly `<file>.compact` makes each rewrite refuse (-1)
 * with the log untouched, which a temp anywhere else could not produce; with
 * the blocker gone both succeed and the operator's file is the only artifact
 * in its directory (a sibling directory stands in as the decoy nothing may
 * land in). The parent derivation is one helper shared with the resolver
 * (parent_dir_of), so test_resolve_data_path's missing-parent arm already
 * pins what "the parent" of such a path is; fsync itself is not observable. */
static int dir_entries(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) n++;
    closedir(d);
    return n;
}

static void test_file_form_temps_beside_log(void) {
    char dir[160], decoy[192], path[192], tmp[224], out[256];
    snprintf(dir, sizeof dir, "%s/fileform", g_dir);
    snprintf(decoy, sizeof decoy, "%s/app.db.d", dir); /* sibling directory */
    snprintf(path, sizeof path, "%s/app.db", dir);      /* the operator's name */
    snprintf(tmp, sizeof tmp, "%s%s", path, WO_WAL_TMP_SUFFIX);
    T_EQ(mkdir(dir, 0700), 0);
    T_EQ(mkdir(decoy, 0700), 0);
    T_EQ(wo_wal_resolve_data_path(path, out, sizeof out), 0);
    T_STREQ(out, path); /* the file form hands the path straight to the engine */

    /* ---- compaction ---- */
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    const char *msg = "";
    uint64_t ids[2];
    for (int i = 0; i < 2; i++) {
        wo_str *s = wo_str_new(&rt, "abc", 3);
        uint64_t vals[2] = {(uint64_t)(i + 1), (uint64_t)(uintptr_t)s};
        ids[i] = wo_row_insert(&db, 0, vals, &msg, NULL);
        T_EQ(wo_wal_append_insert(&w, &db, 0, ids[i]), 0);
        T_EQ(wo_wal_commit(&w), 0);
    }
    for (int k = 0; k < 8; k++) {
        int ek = 0;
        T_EQ(wo_row_update_field(&db, 0, ids[0], 0, (uint64_t)(100 + k), &msg, &ek), 0);
        T_EQ(wo_wal_append_update(&w, &db, 0, ids[0]), 0);
        T_EQ(wo_wal_commit(&w), 0);
    }
    T_CHECK(wo_wal_check(path, NULL) == 10);

    T_EQ(mkdir(tmp, 0700), 0);               /* the blocker */
    T_EQ(wo_wal_compact(&w, &db), -1);       /* the temp has exactly one home */
    T_CHECK(wo_wal_check(path, NULL) == 10); /* refused = untouched */
    T_EQ(rmdir(tmp), 0);                     /* still an empty dir: nothing went in */
    T_EQ(wo_wal_compact(&w, &db), 0);
    T_CHECK(wo_wal_check(path, NULL) == 2);
    T_CHECK(access(tmp, F_OK) != 0);
    T_EQ(dir_entries(dir), 2);   /* app.db + the decoy, nothing else */
    T_EQ(dir_entries(decoy), 0); /* and the decoy saw nothing */
    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);

    /* ---- migration: same temp, same parent. The compacted log is two legacy
       INSERT records of (scalar, text) — MIG_NT's shape — so it migrates
       n,t → n,t,extra in place. ---- */
    wo_schema_class oc[] = {SC("row", 0, mig_sf_nt)};
    wo_schema oldsc = {1, oc, NULL};
    wo_schema_class nc[] = {SC("row", 0, mig_sf_nte)};
    wo_schema newsc = {1, nc, NULL};
    wo_mig_plan pl;
    T_EQ(wo_schema_diff(&oldsc, &newsc, &pl), 0);
    wo_db dbn;
    T_EQ(wo_db_init(&dbn, MIG_NTE, 1, 0, 1), 0);
    T_EQ(mkdir(tmp, 0700), 0);
    T_EQ(wo_wal_migrate(path, &dbn, &oldsc, &pl, &newsc, 0, NULL), -1);
    T_CHECK(wo_wal_check(path, NULL) == 2); /* refused = untouched */
    T_EQ(rmdir(tmp), 0);
    T_EQ(wo_wal_migrate(path, &dbn, &oldsc, &pl, &newsc, 0, NULL), 0);
    wo_mig_plan_free(&pl);
    T_CHECK(access(tmp, F_OK) != 0);
    T_EQ(dir_entries(dir), 2);
    T_EQ(dir_entries(decoy), 0);
    /* and the file, under the operator's name, replays into the new shape */
    T_EQ(wo_wal_replay(path, &dbn), 2);
    db_row *r = wo_row_ptr(&dbn, 0, ids[0]);
    T_CHECK(r != NULL && r->slots[0] == 107 && r->slots[2] == 0);
    wo_db_destroy(&dbn);
}

/* POISON BITES ONLY WITH RECORDS: a retyped class with no stored rows never
 * blocks the boot; the same retype WITH a row refuses and names the field */
static void test_migrate_poison_needs_records(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/migpoison.wal", g_dir);
    static const uint8_t two_kinds0[] = {WO_K_SCALAR, WO_K_TEXT};
    static const uint8_t two_kinds1[] = {WO_K_SCALAR};
    static const wo_classdesc TWO[] = {
        {.name = 0, .flags = 0, .field_cnt = 2, .kinds = two_kinds0},
        {.name = 0, .flags = 0, .field_cnt = 1, .kinds = two_kinds1},
    };
    static const uint8_t two_kinds1f[] = {WO_K_FLOAT};
    static const wo_classdesc TWO_NEW[] = {
        {.name = 0, .flags = 0, .field_cnt = 2, .kinds = two_kinds0},
        {.name = 0, .flags = 0, .field_cnt = 1, .kinds = two_kinds1f},
    };
    wo_schema_field b_old[] = {SF("x", WO_K_SCALAR)};
    wo_schema_field b_new[] = {SF("x", WO_K_FLOAT)};
    wo_schema_class oc[] = {SC("A", 0, mig_sf_nt), SC("B", 0, b_old)};
    wo_schema oldsc = {2, oc, NULL};
    /* the new side also ADDS a field to A, so the plan is not identity and
       the transcode genuinely runs */
    wo_schema_class nc[] = {SC("A", 0, mig_sf_nte), SC("B", 0, b_new)};
    wo_schema newsc = {2, nc, NULL};

    /* log 1: rows of A only */
    {
        wo_db db;
        T_EQ(wo_db_init(&db, TWO, 2, 0, 1), 0);
        wo_wal w;
        T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
        db_row *r = wo_row_create_raw(&db, 0, 1);
        r->slots[0] = 1;
        r->slots[1] = 0;
        T_EQ(wo_row_raw_commit(&db, 0, r), 0);
        T_EQ(wo_wal_append_insert(&w, &db, 0, 1), 0);
        T_EQ(wo_wal_commit(&w), 0);
        wo_wal_close(&w);
        wo_db_destroy(&db);
    }
    wo_mig_plan pl;
    T_EQ(wo_schema_diff(&oldsc, &newsc, &pl), 0);
    T_CHECK(pl.classes[1].poison != NULL); /* B is poisoned... */
    {
        wo_db dbn;
        T_EQ(wo_db_init(&dbn, TWO_NEW, 2, 0, 1), 0);
        char *err = NULL;
        T_EQ(wo_wal_migrate(path, &dbn, &oldsc, &pl, &newsc, 1 << 16, &err), 0);
        T_CHECK(err == NULL); /* ...but nothing of B is stored: boots fine */
        wo_db_destroy(&dbn);
    }
    /* log 2: now with a B record — the poison bites and names the field */
    {
        wo_db db;
        T_EQ(wo_db_init(&db, TWO, 2, 0, 1), 0);
        wo_wal w;
        T_EQ(wo_wal_open(&w, path, 1 << 16), 0); /* migrated log: reopen fresh */
        char p2[144];
        snprintf(p2, sizeof p2, "%s2", path);
        wo_wal w2;
        T_EQ(wo_wal_open(&w2, p2, 1 << 16), 0);
        db_row *rb = wo_row_create_raw(&db, 1, 4);
        rb->slots[0] = 11;
        T_EQ(wo_row_raw_commit(&db, 1, rb), 0);
        T_EQ(wo_wal_append_insert(&w2, &db, 1, 4), 0);
        T_EQ(wo_wal_commit(&w2), 0);
        wo_wal_close(&w2);
        wo_wal_close(&w);
        wo_db_destroy(&db);
        wo_db dbn;
        T_EQ(wo_db_init(&dbn, TWO_NEW, 2, 0, 1), 0);
        char *err = NULL;
        T_EQ(wo_wal_migrate(p2, &dbn, &oldsc, &pl, &newsc, 1 << 16, &err), -2);
        T_CHECK(err != NULL && strstr(err, "`x`") != NULL);
        free(err);
        wo_db_destroy(&dbn);
    }
    wo_mig_plan_free(&pl);
}

/* CORRUPT INPUT: a torn tail ends the intact prefix — the transcode takes
 * the prefix (same rule as replay), never the tear */
static void test_migrate_corrupt_input(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/migcorrupt.wal", g_dir);
    wo_schema_class oc[] = {SC("row", 0, mig_sf_nt)};
    wo_schema oldsc = {1, oc, NULL};
    wo_schema_class nc[] = {SC("row", 0, mig_sf_nte)};
    wo_schema newsc = {1, nc, NULL};
    {
        wo_db db;
        T_EQ(wo_db_init(&db, MIG_NT, 1, 0, 1), 0);
        wo_wal w;
        T_EQ(wo_wal_open(&w, path, 0), 0);
        for (uint64_t id = 1; id <= 3; id++) {
            db_row *r = wo_row_create_raw(&db, 0, id);
            r->slots[0] = id;
            r->slots[1] = 0;
            T_EQ(wo_row_raw_commit(&db, 0, r), 0);
            T_EQ(wo_wal_append_insert(&w, &db, 0, id), 0);
        }
        T_EQ(wo_wal_commit(&w), 0);
        /* tear the LAST record's tail byte */
        off_t end = lseek(w.fd, 0, SEEK_END);
        T_CHECK(end > 4);
        uint8_t junk = 0xFF;
        T_EQ((int)pwrite(w.fd, &junk, 1, end - 1), 1);
        wo_wal_close(&w);
        wo_db_destroy(&db);
    }
    wo_db db2;
    T_EQ(wo_db_init(&db2, MIG_NTE, 1, 0, 1), 0);
    wo_mig_plan pl;
    T_EQ(wo_schema_diff(&oldsc, &newsc, &pl), 0);
    T_EQ(wo_wal_migrate(path, &db2, &oldsc, &pl, &newsc, 0, NULL), 0);
    wo_mig_plan_free(&pl);
    T_EQ(wo_wal_replay(path, &db2), 2); /* rows 1 and 2; the torn third is gone */
    T_CHECK(wo_row_ptr(&db2, 0, 1) != NULL && wo_row_ptr(&db2, 0, 3) == NULL);
    wo_db_destroy(&db2);
}

/* ---- databasev2 12: the boot diff --------------------------------------- */

static void test_schema_diff_verdicts(void) {
    /* base: A { n: scalar, t: text }, B { part: owned->A } */
    wo_schema_field a_f[] = {SF("n", WO_K_SCALAR), SF("t", WO_K_TEXT)};
    wo_schema_field b_f[] = {SFC("part", WO_K_OWNED, 0)};
    wo_schema_class base_c[] = {SC("A", 0, a_f), SC("B", 0, b_f)};
    wo_schema base = {2, base_c, NULL};
    wo_mig_plan pl;

    /* 1. identical -> identity */
    T_EQ(wo_schema_diff(&base, &base, &pl), 0);
    T_EQ(pl.identity, 1);
    T_CHECK(pl.classes[0].poison == NULL && pl.classes[1].poison == NULL);
    wo_mig_plan_free(&pl);

    /* 2. classes reordered -> not identity, cids remapped BY NAME, and the
       owned reference (a different NUMBER now) is recognised by name too */
    {
        wo_schema_field b2_f[] = {SFC("part", WO_K_OWNED, 1)}; /* A is cid 1 now */
        wo_schema_class swap_c[] = {SC("B", 0, b2_f), SC("A", 0, a_f)};
        wo_schema swp = {2, swap_c, NULL};
        T_EQ(wo_schema_diff(&base, &swp, &pl), 0);
        T_EQ(pl.identity, 0);
        T_EQ(pl.classes[0].new_cid, 1u); /* old A -> new cid 1 */
        T_EQ(pl.classes[1].new_cid, 0u); /* old B -> new cid 0 */
        T_CHECK(pl.classes[0].poison == NULL && pl.classes[1].poison == NULL);
        T_CHECK(pl.classes[0].changed == 0 && pl.classes[1].changed == 0);
        wo_mig_plan_free(&pl);
    }

    /* 3. added field -> changed, surviving map intact, B poisoned (embeds A) */
    {
        wo_schema_field a3_f[] = {SF("n", WO_K_SCALAR), SF("t", WO_K_TEXT),
                                  SF("extra", WO_K_SCALAR)};
        wo_schema_class c3[] = {SC("A", 0, a3_f), SC("B", 0, b_f)};
        wo_schema n3 = {2, c3, NULL};
        T_EQ(wo_schema_diff(&base, &n3, &pl), 0);
        T_EQ(pl.identity, 0);
        T_CHECK(pl.classes[0].poison == NULL && pl.classes[0].changed == 1);
        T_EQ(pl.classes[0].fmap[0], 0);
        T_EQ(pl.classes[0].fmap[1], 1);
        T_CHECK(pl.classes[1].poison != NULL); /* embeds a changed class */
        wo_mig_plan_free(&pl);
    }

    /* 4. deleted field -> fmap -1; different-shape delete+add migrates */
    {
        wo_schema_field a4_f[] = {SF("n", WO_K_SCALAR), SF("blob", WO_K_BYTES)};
        wo_schema_class c4[] = {SC("A", 0, a4_f), SC("B", 0, b_f)};
        wo_schema n4 = {2, c4, NULL}; /* t: Text deleted, blob: Bytes added */
        T_EQ(wo_schema_diff(&base, &n4, &pl), 0);
        T_CHECK(pl.classes[0].poison == NULL && pl.classes[0].changed == 1);
        T_EQ(pl.classes[0].fmap[0], 0);
        T_EQ(pl.classes[0].fmap[1], -1);
        wo_mig_plan_free(&pl);
    }

    /* 5. SAME-shape delete+add -> the rename ambiguity poison */
    {
        wo_schema_field a5_f[] = {SF("n", WO_K_SCALAR), SF("headline", WO_K_TEXT)};
        wo_schema_class c5[] = {SC("A", 0, a5_f), SC("B", 0, b_f)};
        wo_schema n5 = {2, c5, NULL};
        T_EQ(wo_schema_diff(&base, &n5, &pl), 0);
        T_CHECK(pl.classes[0].poison != NULL);
        T_CHECK(strstr(pl.classes[0].poison, "two separate steps") != NULL);
        wo_mig_plan_free(&pl);
    }

    /* 6. retype -> poison naming the field */
    {
        wo_schema_field a6_f[] = {SF("n", WO_K_FLOAT), SF("t", WO_K_TEXT)};
        wo_schema_class c6[] = {SC("A", 0, a6_f), SC("B", 0, b_f)};
        wo_schema n6 = {2, c6, NULL};
        T_EQ(wo_schema_diff(&base, &n6, &pl), 0);
        T_CHECK(pl.classes[0].poison != NULL &&
                strstr(pl.classes[0].poison, "`n`") != NULL);
        wo_mig_plan_free(&pl);
    }

    /* 7. vanished class -> poison; the other class (embedding nothing that
       changed shape) is untouched */
    {
        wo_schema_class c7[] = {SC("A", 0, a_f)};
        wo_schema n7 = {1, c7, NULL};
        T_EQ(wo_schema_diff(&base, &n7, &pl), 0);
        T_CHECK(pl.classes[1].new_cid == WO_SCHEMA_NONE &&
                pl.classes[1].poison != NULL);
        T_CHECK(pl.classes[0].poison == NULL);
        wo_mig_plan_free(&pl);
    }

    /* 8. flags change -> poison (v1 migrates fields, not storage modes) */
    {
        wo_schema_class c8[] = {SC("A", WO_CLASSF_RESIDENT_KEYS, a_f), SC("B", 0, b_f)};
        wo_schema n8 = {2, c8, NULL};
        T_EQ(wo_schema_diff(&base, &n8, &pl), 0);
        T_CHECK(pl.classes[0].poison != NULL &&
                strstr(pl.classes[0].poison, "storage") != NULL);
        wo_mig_plan_free(&pl);
    }

    /* 9. a NEW class in the binary does not break identity: it has no records */
    {
        wo_schema_field n_f[] = {SF("x", WO_K_SCALAR)};
        wo_schema_class c9[] = {SC("A", 0, a_f), SC("B", 0, b_f), SC("C", 0, n_f)};
        wo_schema n9 = {3, c9, NULL};
        T_EQ(wo_schema_diff(&base, &n9, &pl), 0);
        T_EQ(pl.identity, 1);
        wo_mig_plan_free(&pl);
    }

    /* 10. the embed closure is transitive: C owns B, B owns A, A changed ->
       both B and C poisoned */
    {
        wo_schema_field cB[] = {SFC("a", WO_K_OWNED, 0)};
        wo_schema_field cC[] = {SFC("b", WO_K_OWNED, 1)};
        wo_schema_class oc[] = {SC("A", 0, a_f), SC("B", 0, cB), SC("C", 0, cC)};
        wo_schema oldsc = {3, oc, NULL};
        wo_schema_field a10[] = {SF("n", WO_K_SCALAR)}; /* t deleted */
        wo_schema_class nc[] = {SC("A", 0, a10), SC("B", 0, cB), SC("C", 0, cC)};
        wo_schema newsc = {3, nc, NULL};
        T_EQ(wo_schema_diff(&oldsc, &newsc, &pl), 0);
        T_CHECK(pl.classes[0].poison == NULL && pl.classes[0].changed == 1);
        T_CHECK(pl.classes[1].poison != NULL);
        T_CHECK(pl.classes[2].poison != NULL);
        wo_mig_plan_free(&pl);
    }
}

/* ---- databasev2 12: the schema record ---------------------------------- */

/* a hand-built two-class schema exercising every payload field */
static wo_schema mig_schema_sample(void) {
    static wo_schema_field f0[] = {
        {(const uint8_t *)"n", 1, WO_K_SCALAR, WO_SCHEMA_NONE, WO_SCHEMA_NONE},
        {(const uint8_t *)"label", 5, WO_K_TEXT, WO_SCHEMA_NONE, WO_SCHEMA_NONE},
    };
    static wo_schema_field f1[] = {
        {(const uint8_t *)"part", 4, WO_K_OWNED, 0, WO_SCHEMA_NONE},
        {(const uint8_t *)"tags", 4, WO_K_MULTI, WO_SCHEMA_NONE, WO_K_TEXT},
    };
    static wo_schema_class cls[] = {
        {(const uint8_t *)"row", 3, 0, 2, f0},
        {(const uint8_t *)"box", 3, WO_CLASSF_RESIDENT_KEYS, 2, f1},
    };
    wo_schema sc = {2, cls, NULL};
    return sc;
}

static void test_schema_roundtrip(void) {
    wo_schema sc = mig_schema_sample();
    uint8_t *p;
    uint32_t len;
    T_EQ(wo_schema_encode(&sc, &p, &len), 0);
    T_CHECK(len > 5 && p[0] == WO_WAL_SCHEMA);
    wo_schema *back = wo_schema_decode(p, len);
    T_CHECK(back != NULL);
    T_EQ(back->class_cnt, 2u);
    T_CHECK(back->classes[0].name_len == 3 && memcmp(back->classes[0].name, "row", 3) == 0);
    T_EQ(back->classes[0].field_cnt, 2u);
    T_CHECK(back->classes[0].fields[1].kind == WO_K_TEXT &&
            back->classes[0].fields[1].name_len == 5 &&
            memcmp(back->classes[0].fields[1].name, "label", 5) == 0);
    T_EQ(back->classes[1].flags, (uint32_t)WO_CLASSF_RESIDENT_KEYS);
    T_CHECK(back->classes[1].fields[0].fclass == 0 &&
            back->classes[1].fields[1].felem == WO_K_TEXT);
    /* the decode owns its bytes: the encode buffer can die first */
    free(p);
    T_CHECK(memcmp(back->classes[1].name, "box", 3) == 0);
    /* a truncated payload is malformed, not a crash */
    uint8_t *p2;
    uint32_t len2;
    T_EQ(wo_schema_encode(&sc, &p2, &len2), 0);
    T_CHECK(wo_schema_decode(p2, len2 - 3) == NULL);
    free(p2);
    wo_schema_free(back);
}

/* a fresh log opened with a schema carries it as its FIRST record; replay
 * skips it without counting it, and rows behind it land intact */
static void test_schema_fresh_log(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/schemafresh.wal", g_dir);
    wo_db db;
    T_EQ(wo_db_init(&db, CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    wo_schema sc = mig_schema_sample();
    T_EQ(wo_wal_set_schema(&w, &sc), 0);
    T_EQ(wo_wal_ensure_schema(&w), 0);
    /* head record is the schema */
    uint8_t *p;
    uint32_t len;
    T_EQ(wo_wal_read_schema(path, &p, &len), 0);
    wo_schema *back = wo_schema_decode(p, len);
    T_CHECK(back != NULL && back->class_cnt == 2);
    wo_schema_free(back);
    free(p);
    /* a second ensure is a no-op: records exist now */
    uint64_t before = wo_wal_next_offset(&w);
    T_EQ(wo_wal_ensure_schema(&w), 0);
    T_EQ(wo_wal_next_offset(&w), before);
    /* a row behind it replays; the schema record is not counted */
    const char *msg = "";
    uint64_t vals[2] = {7, 0};
    uint64_t id = wo_row_insert(&db, 0, vals, &msg, NULL);
    T_CHECK(id != 0);
    T_EQ(wo_wal_append_insert(&w, &db, 0, id), 0);
    T_EQ(wo_wal_commit(&w), 0);
    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_db db2;
    T_EQ(wo_db_init(&db2, CLASSES, 1, 0, 1), 0);
    T_EQ(wo_wal_replay(path, &db2), 1); /* one row, not two records */
    db_row *r = wo_row_ptr(&db2, 0, id);
    T_CHECK(r != NULL && r->slots[0] == 7);
    wo_db_destroy(&db2);
}

/* 2026-09-10 defect, second half: every `*msg = …` in the fold was
 * unguarded, and wo_idx_probe borrows with msg == NULL (a candidate that does
 * not fold is simply not a hit), so a malformed record under an index probe
 * was a zero-page write instead of a refused row. [msg] is optional; a
 * caller that asks still gets the name. */
static void test_fold_row_at_tolerates_null_msg(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/foldnull.wal", g_dir);
    wo_db db;
    T_EQ(wo_db_init(&db, CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    wo_schema sc = mig_schema_sample();
    T_EQ(wo_wal_set_schema(&w, &sc), 0);
    T_EQ(wo_wal_ensure_schema(&w), 0); /* offset 0 holds the head, not a row */
    uint32_t cid = 99, hops = 0;
    uint64_t id = 0, vals[2] = {0, 0};
    /* the two refusals a probe can meet: a non-row record, and no record */
    T_EQ(wo_wal_fold_row_at(&w, &db, 0, &cid, &id, vals, &hops, NULL), -1);
    T_EQ(wo_wal_fold_row_at(&w, &db, 1u << 20, &cid, &id, vals, &hops, NULL), -1);
    const char *msg = NULL;
    T_EQ(wo_wal_fold_row_at(&w, &db, 0, &cid, &id, vals, &hops, &msg), -1);
    T_STREQ(msg, "record header is malformed");
    msg = NULL;
    T_EQ(wo_wal_fold_row_at(&w, &db, 1u << 20, &cid, &id, vals, &hops, &msg), -1);
    T_STREQ(msg, "no intact record at that offset");
    wo_wal_close(&w);
    wo_db_destroy(&db);
}

/* a LEGACY log (rows, no schema record) reports 1 from read_schema, and its
 * first compaction with a schema set writes the record at the head */
static void test_schema_compaction_adopts_legacy(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/schemalegacy.wal", g_dir);
    wo_db db;
    T_EQ(wo_db_init(&db, CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    const char *msg = "";
    uint64_t vals[2] = {1, 0};
    uint64_t id = wo_row_insert(&db, 0, vals, &msg, NULL);
    T_CHECK(id != 0);
    T_EQ(wo_wal_append_insert(&w, &db, 0, id), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_wal_read_schema(path, NULL, NULL), 1); /* legacy: head is a row */
    wo_schema sc = mig_schema_sample();
    T_EQ(wo_wal_set_schema(&w, &sc), 0);
    T_EQ(wo_wal_ensure_schema(&w), 0); /* no-op: not empty */
    T_EQ(wo_wal_read_schema(path, NULL, NULL), 1);
    T_EQ(wo_wal_compact(&w, &db), 0);
    uint8_t *p;
    uint32_t len;
    T_EQ(wo_wal_read_schema(path, &p, &len), 0); /* adopted at the head */
    free(p);
    /* and the compacted log still replays its row */
    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_db db2;
    T_EQ(wo_db_init(&db2, CLASSES, 1, 0, 1), 0);
    T_EQ(wo_wal_replay(path, &db2), 1);
    db_row *r = wo_row_ptr(&db2, 0, id);
    T_CHECK(r != NULL && r->slots[0] == 1);
    wo_db_destroy(&db2);
}

/* missing and empty files are legacy, not errors */
static void test_schema_read_absent(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/schemanone.wal", g_dir);
    T_EQ(wo_wal_read_schema(path, NULL, NULL), 1); /* no file */
    FILE *f = fopen(path, "w");
    T_CHECK(f != NULL);
    fclose(f);
    T_EQ(wo_wal_read_schema(path, NULL, NULL), 1); /* empty file */
}

static void test_delta_chain_flattens_at_k(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/chainflat.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, KEYS_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, KEYS_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 18), 0);
    db.rt = &rt; rt.wal = &w; rt.db = &db;
    const char *msg = "";

    wo_str *sv = wo_str_new(&rt, "flat", 4);
    uint64_t vals[2] = {0, (uint64_t)(uintptr_t)sv};
    uint64_t id = wo_row_insert(&db, 0, vals, &msg, NULL);
    T_CHECK(id != 0);
    uint64_t off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db, 0, id), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_drop_payload(&db, 0, id, off), 0);

    /* Walk the depth up one update at a time and watch it reset. Without the
     * flatten branch this climbs forever; with it, it must never exceed K. */
    uint32_t peak = 0;
    int saw_reset = 0;
    for (uint32_t n = 1; n <= WO_DELTA_MAX_HOPS * 2u + 2u; n++) {
        chain_update(&db, &w, 0, id, 0, (uint64_t)n);

        uint32_t hops = 0;
        uint32_t got_cid = 0;
        uint64_t got_id = 0;
        uint64_t out[2] = {0, 0};
        const char *fm = "";
        uint64_t o1 = wo_row_offset1(&db, 0, id);
        T_CHECK(o1 != 0);
        T_EQ(wo_wal_fold_row_at(&w, &db, o1 - 1, &got_cid, &got_id, out, &hops, &fm), 0);
        T_CHECK(got_cid == 0 && got_id == id);
        T_CHECK(out[0] == (uint64_t)n);          /* the value is still right */
        for (uint32_t i = 0; i < 2; i++) wo_db_val_free(&db, KEYS_CLASSES[0].kinds[i], out[i]);

        if (hops > peak) peak = hops;
        if (n > 1 && hops == 0) saw_reset = 1;   /* a chain was terminated */
    }
    T_CHECK(peak <= WO_DELTA_MAX_HOPS);          /* the bound holds */
    T_CHECK(saw_reset);                          /* and it was actually reached */

    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

/* databasev2 11: a flattened row must survive a restart identically. Replay
 * meets a WO_WAL_INSERT where a chain used to be; if flattening wrote a shape
 * replay mishandled, this is where it shows. */
static void test_delta_chain_flatten_replays(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/chainflatreplay.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, KEYS_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, KEYS_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 18), 0);
    db.rt = &rt; rt.wal = &w; rt.db = &db;
    const char *msg = "";

    wo_str *sv = wo_str_new(&rt, "rep", 3);
    uint64_t vals[2] = {0, (uint64_t)(uintptr_t)sv};
    uint64_t id = wo_row_insert(&db, 0, vals, &msg, NULL);
    uint64_t off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db, 0, id), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_drop_payload(&db, 0, id, off), 0);

    /* enough updates to guarantee at least one flatten */
    uint64_t last = 0;
    for (uint32_t n = 1; n <= WO_DELTA_MAX_HOPS + 3u; n++) {
        chain_update(&db, &w, 0, id, 0, (uint64_t)n);
        last = n;
    }
    wo_wal_close(&w);
    wo_db_destroy(&db);

    wo_db db2;
    T_EQ(wo_db_init(&db2, KEYS_CLASSES, 1, 0, 1), 0);
    db2.rt = &rt; rt.wal = NULL; rt.db = &db2;
    T_CHECK(wo_wal_replay(path, &db2) >= 0);
    wo_wal w2;
    T_EQ(wo_wal_open(&w2, path, 1 << 18), 0);
    rt.wal = &w2;

    db_row *r = wo_row_borrow(&db2, 0, id, &msg);
    T_CHECK(r != NULL);
    T_CHECK(r->slots[0] == last);                /* the newest value survived */
    db_text *back = (db_text *)(uintptr_t)r->slots[1];
    T_CHECK(back != NULL && back->len == 3 && memcmp(back->bytes, "rep", 3) == 0);
    wo_row_release(&db2, 0, r);

    wo_wal_close(&w2);
    wo_db_destroy(&db2);
    wo_rt_destroy(&rt);
}

static void test_keys_resident_update_indexed(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/keysidx.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, KEYS_IDX_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, KEYS_IDX_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db.rt = &rt; rt.wal = &w; rt.db = &db;
    const char *msg = "";

    wo_str *sa = wo_str_new(&rt, "a", 1);
    wo_str *sb = wo_str_new(&rt, "b", 1);
    uint64_t va[2] = {100, (uint64_t)(uintptr_t)sa};
    uint64_t vb[2] = {200, (uint64_t)(uintptr_t)sb};
    uint64_t a = wo_row_insert(&db, 0, va, &msg, NULL);
    uint64_t b = wo_row_insert(&db, 0, vb, &msg, NULL);
    T_CHECK(a != 0 && b != 0);
    uint64_t off_a = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db, 0, a), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_drop_payload(&db, 0, a, off_a), 0);
    uint64_t off_b = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db, 0, b), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_drop_payload(&db, 0, b, off_b), 0);

    /* before the update: probing 100 finds a */
    uint64_t *ids;
    uint32_t cnt;
    T_EQ(wo_idx_probe(&db, 0, 0, 100, NULL, 0, &ids, &cnt), 1);
    T_CHECK(cnt == 1 && ids[0] == a);
    free(ids);

    int ek = 0;
    uint64_t roff = wo_wal_next_offset(&w);
    T_EQ(wo_row_update_field(&db, 0, a, 0, 150, &msg, &ek), 0);
    T_EQ(ek, DB_ERR_NONE);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_set_offset(&db, 0, a, roff), 0);

    /* found by the NEW value */
    T_EQ(wo_idx_probe(&db, 0, 0, 150, NULL, 0, &ids, &cnt), 1);
    T_CHECK(cnt == 1 && ids[0] == a);
    free(ids);

    /* gone from the OLD one */
    T_EQ(wo_idx_probe(&db, 0, 0, 100, NULL, 0, &ids, &cnt), 1);
    T_CHECK(cnt == 0 && ids == NULL);

    /* b, untouched, still finds by its own value */
    T_EQ(wo_idx_probe(&db, 0, 0, 200, NULL, 0, &ids, &cnt), 1);
    T_CHECK(cnt == 1 && ids[0] == b);
    free(ids);

    db_row *r = wo_row_borrow(&db, 0, a, &msg);
    T_CHECK(r != NULL && r->slots[0] == 150);
    wo_row_release(&db, 0, r);

    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

/* databasev2 11: the third leg the story called out — a delta on an INDEXED
 * column, with flattening in play. The two are independent features that meet
 * on the same write path, and the meeting is where a bug would live:
 * `row_apply_field_keys` picks DELTA or full-row image AFTER the index has
 * already been re-pointed, so a flattened image that captured the wrong value
 * would leave the index pointing at a row the fold disagrees with.
 *
 * Drives enough updates on the indexed column to cross WO_DELTA_MAX_HOPS
 * several times, so at least one update lands on each branch, then checks the
 * index and the fold agree at the end and after replay. */
static void test_keys_resident_indexed_across_flatten(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/keysidxflat.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, KEYS_IDX_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, KEYS_IDX_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db.rt = &rt; rt.wal = &w; rt.db = &db;
    const char *msg = "";

    wo_str *sa = wo_str_new(&rt, "a", 1);
    uint64_t va[2] = {100, (uint64_t)(uintptr_t)sa};
    uint64_t a = wo_row_insert(&db, 0, va, &msg, NULL);
    T_CHECK(a != 0);
    uint64_t off_a = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db, 0, a), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_drop_payload(&db, 0, a, off_a), 0);

    /* cross the bound several times over; ROUNDS is deliberately not a
     * multiple of the bound, so the run does not end on a reset */
    const uint64_t ROUNDS = WO_DELTA_MAX_HOPS * 3 + 5;
    uint64_t val = 100;
    int saw_reset = 0;
    db_table *t = &db.tables[0];
    for (uint64_t i = 0; i < ROUNDS; i++) {
        uint64_t prev = val;
        val = 200 + i;
        int ek = 0;
        uint64_t roff = wo_wal_next_offset(&w);
        T_EQ(wo_row_update_field(&db, 0, a, 0, val, &msg, &ek), 0);
        T_EQ(ek, DB_ERR_NONE);
        T_EQ(wo_wal_commit(&w), 0);
        T_EQ(wo_row_set_offset(&db, 0, a, roff), 0);

        /* every intermediate step, not only the last: the row is findable by
         * the value just written and absent from the one it replaced */
        uint64_t *ids; uint32_t cnt;
        T_EQ(wo_idx_probe(&db, 0, 0, val, NULL, 0, &ids, &cnt), 1);
        T_CHECK(cnt == 1 && ids[0] == a);
        free(ids);
        T_EQ(wo_idx_probe(&db, 0, 0, prev, NULL, 0, &ids, &cnt), 1);
        T_CHECK(cnt == 0 && ids == NULL);

        db_row *r = wo_row_borrow(&db, 0, a, &msg);
        T_CHECK(r != NULL && r->slots[0] == val);
        if (t->scratch_hops == 0) saw_reset = 1;
        T_CHECK(t->scratch_hops <= WO_DELTA_MAX_HOPS);
        wo_row_release(&db, 0, r);
    }
    T_CHECK(saw_reset);        /* flattening actually fired during the run */

    /* the Text column, never updated, must survive every flatten: the image
     * is rebuilt from a borrowed row, which is exactly where a value of the
     * wrong representation would be written back */
    db_row *r = wo_row_borrow(&db, 0, a, &msg);
    T_CHECK(r != NULL && r->slots[0] == val);
    db_text *back = (db_text *)(uintptr_t)r->slots[1];   /* engine repr, not wo_str */
    T_CHECK(back != NULL && back->len == 1 && back->bytes[0] == 'a');
    wo_row_release(&db, 0, r);

    wo_wal_close(&w);
    wo_db_destroy(&db);

    /* And the index rebuilds from the replayed log, flattened records and all.
     * A keys-resident table comes back offset-valued, so both the probe's
     * verification and any read need a live log: replay with rt.wal NULL (it
     * lends its own read-only view), then reopen before touching the rows. */
    wo_db db2;
    T_EQ(wo_db_init(&db2, KEYS_IDX_CLASSES, 1, 0, 1), 0);
    db2.rt = &rt; rt.wal = NULL; rt.db = &db2;
    T_CHECK(wo_wal_replay(path, &db2) >= 0);
    wo_wal w2;
    T_EQ(wo_wal_open(&w2, path, 1 << 16), 0);
    rt.wal = &w2;

    uint64_t *ids; uint32_t cnt;
    T_EQ(wo_idx_probe(&db2, 0, 0, val, NULL, 0, &ids, &cnt), 1);
    T_CHECK(cnt == 1 && ids[0] == a);
    free(ids);
    T_EQ(wo_idx_probe(&db2, 0, 0, 100, NULL, 0, &ids, &cnt), 1);
    T_CHECK(cnt == 0 && ids == NULL);

    db_row *r2 = wo_row_borrow(&db2, 0, a, &msg);
    T_CHECK(r2 != NULL && r2->slots[0] == val);
    wo_row_release(&db2, 0, r2);

    wo_wal_close(&w2);
    wo_db_destroy(&db2);
    wo_rt_destroy(&rt);
}

/* class 0: Row { n: scalar, sku: Text @unique } — the Text-representation
 * gap flagged across Tasks 3, 4 and 5 and fixed in Task 6: every existing
 * keys-resident index test above indexes the SCALAR column, never
 * exercising idx_hash/idx_cols_equal/wo_idx_probe's WO_K_TEXT arm (nor
 * db.c's GET_FIELD/PROBE arms) against a keys-resident row. The root cause
 * was keys_fold_into handing back VM wo_str* where a borrowed row's slots
 * are supposed to hold engine db_text* — table.h's own "a row stores NO VM
 * pointer" doctrine, true for `resident: all` and silently false for
 * `resident: keys` until this task. This is the test that proves the fix:
 * index the TEXT column, update it, and probe by both the OLD and NEW
 * value — the same shape as test_keys_resident_update_indexed, on the
 * column that used to misread. */
static const uint8_t keys_text_idx_kinds[] = {WO_K_SCALAR, WO_K_TEXT};
static const uint32_t keys_text_idx_meta[] = {1 /*unique*/, 1, 1 /*col: sku (field 1)*/};
static const wo_classdesc KEYS_TEXT_IDX_CLASSES[] = {
    {.name = 0, .flags = WO_CLASSF_RESIDENT_KEYS, .field_cnt = 2, .kinds = keys_text_idx_kinds,
     .idx_cnt = 1, .idx_meta = keys_text_idx_meta},
};

static void test_keys_resident_update_indexed_text(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/keystextidx.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, KEYS_TEXT_IDX_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, KEYS_TEXT_IDX_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db.rt = &rt; rt.wal = &w; rt.db = &db;
    const char *msg = "";

    wo_str *sa = wo_str_new(&rt, "SKU-AAA", 7);
    wo_str *sb = wo_str_new(&rt, "SKU-BBB", 7);
    uint64_t va[2] = {1, (uint64_t)(uintptr_t)sa};
    uint64_t vb[2] = {2, (uint64_t)(uintptr_t)sb};
    uint64_t a = wo_row_insert(&db, 0, va, &msg, NULL);
    uint64_t b = wo_row_insert(&db, 0, vb, &msg, NULL);
    T_CHECK(a != 0 && b != 0);
    uint64_t off_a = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db, 0, a), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_drop_payload(&db, 0, a, off_a), 0);
    uint64_t off_b = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db, 0, b), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_drop_payload(&db, 0, b, off_b), 0);

    /* before the update: probing "SKU-AAA" finds a — through wo_idx_probe's
       verify step, which borrows the row and reads its Text slot, exactly
       the path the representation bug corrupted */
    uint64_t *ids;
    uint32_t cnt;
    T_EQ(wo_idx_probe(&db, 0, 0, 0, "SKU-AAA", 7, &ids, &cnt), 1);
    T_CHECK(cnt == 1 && ids[0] == a);
    free(ids);

    /* update a's Text column: "SKU-AAA" -> "SKU-CCC" */
    wo_str *sc = wo_str_new(&rt, "SKU-CCC", 7);
    int ek = 0;
    uint64_t roff = wo_wal_next_offset(&w);
    T_EQ(wo_row_update_field(&db, 0, a, 1, (uint64_t)(uintptr_t)sc, &msg, &ek), 0);
    T_EQ(ek, DB_ERR_NONE);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_set_offset(&db, 0, a, roff), 0);

    /* found by the NEW value */
    T_EQ(wo_idx_probe(&db, 0, 0, 0, "SKU-CCC", 7, &ids, &cnt), 1);
    T_CHECK(cnt == 1 && ids[0] == a);
    free(ids);

    /* gone from the OLD one */
    T_EQ(wo_idx_probe(&db, 0, 0, 0, "SKU-AAA", 7, &ids, &cnt), 1);
    T_CHECK(cnt == 0 && ids == NULL);

    /* b, untouched, still finds by its own value */
    T_EQ(wo_idx_probe(&db, 0, 0, 0, "SKU-BBB", 7, &ids, &cnt), 1);
    T_CHECK(cnt == 1 && ids[0] == b);
    free(ids);

    /* a genuine duplicate is still refused: updating b's sku to a's NEW
       value must trip @unique — proving idx_cols_equal reads the correct
       engine bytes on BOTH sides, not a coincidental symmetric misread */
    wo_str *sdupe = wo_str_new(&rt, "SKU-CCC", 7);
    T_EQ(wo_row_update_field(&db, 0, b, 1, (uint64_t)(uintptr_t)sdupe, &msg, &ek), -1);
    T_EQ(ek, DB_ERR_UNIQUE);

    /* the row itself reads back correctly through wo_row_borrow */
    db_row *r = wo_row_borrow(&db, 0, a, &msg);
    T_CHECK(r != NULL);
    db_text *back = (db_text *)(uintptr_t)r->slots[1];
    T_CHECK(back != NULL && back->len == 7 && memcmp(back->bytes, "SKU-CCC", 7) == 0);
    wo_row_release(&db, 0, r);

    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

/* class 0: Row { n: scalar @unique, label: Text } — same shape as
 * KEYS_IDX_CLASSES, but the index is genuinely unique this time. */
static const uint8_t keys_uniq_kinds[] = {WO_K_SCALAR, WO_K_TEXT};
static const uint32_t keys_uniq_meta[] = {1 /*unique*/, 1, 0 /*col: n*/};
static const wo_classdesc KEYS_UNIQUE_CLASSES[] = {
    {.name = 0, .flags = WO_CLASSF_RESIDENT_KEYS, .field_cnt = 2, .kinds = keys_uniq_kinds,
     .idx_cnt = 1, .idx_meta = keys_uniq_meta},
};

/* Review finding (Task 3 follow-up): the unique shadow-check borrowed its
 * candidate through wo_row_borrow, which shares ONE scratch buffer per
 * table with the row already borrowed for the update itself — so the
 * candidate borrow always failed (NULL), clash was always false, and a
 * `resident: keys` table with a `@unique` index silently accepted
 * duplicates on update. This is the test that would have caught it: two
 * rows, update one's unique column to collide with the other's value, the
 * update must be REFUSED and the row left exactly as it was. */
static void test_keys_resident_update_unique_violation_refused(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/keysuniq.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, KEYS_UNIQUE_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, KEYS_UNIQUE_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db.rt = &rt; rt.wal = &w; rt.db = &db;
    const char *msg = "";

    wo_str *sa = wo_str_new(&rt, "a", 1);
    wo_str *sb = wo_str_new(&rt, "b", 1);
    uint64_t va[2] = {100, (uint64_t)(uintptr_t)sa};
    uint64_t vb[2] = {200, (uint64_t)(uintptr_t)sb};
    uint64_t a = wo_row_insert(&db, 0, va, &msg, NULL);
    uint64_t b = wo_row_insert(&db, 0, vb, &msg, NULL);
    T_CHECK(a != 0 && b != 0);
    uint64_t off_a = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db, 0, a), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_drop_payload(&db, 0, a, off_a), 0);
    uint64_t off_b = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db, 0, b), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_drop_payload(&db, 0, b, off_b), 0);

    /* a's n: 100 -> 200 collides with b's live value — must be refused */
    int ek = 0;
    T_EQ(wo_row_update_field(&db, 0, a, 0, 200, &msg, &ek), -1);
    T_EQ(ek, DB_ERR_UNIQUE);

    /* a untouched: still 100, still the only hit for 100 */
    db_row *r = wo_row_borrow(&db, 0, a, &msg);
    T_CHECK(r != NULL && r->slots[0] == 100);
    wo_row_release(&db, 0, r);
    uint64_t *ids;
    uint32_t cnt;
    T_EQ(wo_idx_probe(&db, 0, 0, 100, NULL, 0, &ids, &cnt), 1);
    T_CHECK(cnt == 1 && ids[0] == a);
    free(ids);

    /* b untouched: still the only hit for 200 */
    T_EQ(wo_idx_probe(&db, 0, 0, 200, NULL, 0, &ids, &cnt), 1);
    T_CHECK(cnt == 1 && ids[0] == b);
    free(ids);

    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

/* Task 4 (keys-resident delta updates): the request path's group-commit
 * shape, the test the brief asked for. Two updates to the SAME row through
 * wo_row_update_field_slot (the request-path entry point) with NEITHER
 * wo_wal_commit NOR the re-point called in between — exactly two requests
 * landing in the SAME drain before its one barrier. The re-point for each
 * is only RECORDED (wo_wal_pend_repoint), mirroring db.c's request arm;
 * the barrier commits once, then wo_db_flush_drops applies both.
 *
 * The failure this catches: a back_off read straight off the (still stale,
 * pre-barrier) durable map would have the second delta name the FIRST
 * request's insert-time offset instead of the first delta — skipping it.
 * Checked two ways: the final value must reflect BOTH updates in order,
 * and delta 2's back-pointer, read straight off disk, must equal delta 1's
 * own offset, not the base insert's. */
static void test_keys_resident_two_updates_one_drain(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/keys2upd.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, KEYS_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, KEYS_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db.rt = &rt; rt.wal = &w; rt.db = &db;
    const char *msg = "";

    wo_str *s = wo_str_new(&rt, "sku", 3);
    uint64_t vals[2] = {111, (uint64_t)(uintptr_t)s};
    uint64_t id = wo_row_insert(&db, 0, vals, &msg, NULL);
    T_CHECK(id != 0);
    uint64_t base_off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db, 0, id), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_drop_payload(&db, 0, id, base_off), 0);

    /* "request" 1: field 0, 111 -> 222 — staged, NOT committed, the map
       NOT moved (only recorded as pending) */
    int ek = 0;
    uint64_t roff1 = wo_wal_next_offset(&w);
    T_EQ(wo_row_update_field_slot(&db, 0, id, 0, 222, &msg, &ek), 0);
    T_EQ(ek, DB_ERR_NONE);
    T_EQ(wo_wal_pend_repoint(&w, 0, id, roff1), 0);

    /* "request" 2, SAME drain: field 0, 222 -> 333. The id map still names
       the base insert (the re-point above is only PENDING) — back_off must
       come from the pending list, not wo_row_offset1, or this chains to
       the wrong predecessor. */
    uint64_t roff2 = wo_wal_next_offset(&w);
    T_EQ(wo_row_update_field_slot(&db, 0, id, 0, 333, &msg, &ek), 0);
    T_EQ(ek, DB_ERR_NONE);
    T_EQ(wo_wal_pend_repoint(&w, 0, id, roff2), 0);

    /* the drain's barrier: ONE commit for both staged deltas, then both
       pending re-points applied — db.c/vm.c's exact shape */
    T_EQ(wo_wal_commit(&w), 0);
    wo_db_flush_drops(&db, &w);

    /* both updates visible, in order */
    db_row *r = wo_row_borrow(&db, 0, id, &msg);
    T_CHECK(r != NULL && r->slots[0] == 333);
    db_text *back = (db_text *)(uintptr_t)r->slots[1];
    T_CHECK(back != NULL && back->len == 3 && memcmp(back->bytes, "sku", 3) == 0);
    wo_row_release(&db, 0, r);

    /* the chain itself: delta 2's back-pointer names delta 1's OWN offset,
       not the base insert's — payload layout established by
       test_delta_record (kind|class|id|field_idx|back_off|value, 33 bytes
       for a scalar field) */
    uint8_t body[33];
    T_EQ((int)pread(w.fd, body, 33, (off_t)(roff2 + 8)), 33);
    T_EQ(body[0], WO_WAL_DELTA);
    uint64_t back_off;
    memcpy(&back_off, body + 17, 8);
    T_EQ(back_off, roff1);
    T_CHECK(back_off != base_off); /* the skip this test exists to catch */

    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

/* CRITICAL 2 (review finding): wo_row_borrow itself must prefer a pending
 * re-point over the durable map, the same reason back_off and the unique
 * shadow-check's candidate lookup already do. Without it, the SECOND (and
 * every later) update to one row in one drain borrows the row via `hget` —
 * the still-DURABLE, pre-drain offset — so row_apply_field_keys's
 * idx_remove_row hashes the row's ORIGINAL column value. That value was
 * already removed from the index by the FIRST update in this drain, so the
 * remove finds nothing, and idx_add_row adds a SECOND entry. N updates to
 * one row in one drain used to leave N entries for it; this asserts
 * exactly one, however many updates ran. */
static void test_keys_resident_repeat_updates_one_drain_index(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/keysrepeatidx.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, KEYS_IDX_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, KEYS_IDX_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db.rt = &rt; rt.wal = &w; rt.db = &db;
    const char *msg = "";

    wo_str *s = wo_str_new(&rt, "x", 1);
    uint64_t vals[2] = {100, (uint64_t)(uintptr_t)s};
    uint64_t id = wo_row_insert(&db, 0, vals, &msg, NULL);
    T_CHECK(id != 0);
    uint64_t off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db, 0, id), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_drop_payload(&db, 0, id, off), 0);

    /* 5 updates to the SAME row, ALL staged behind the SAME barrier —
       db.c's request-path shape: stage each, remember its pending
       re-point, only commit + flush once at the end of the drain. */
    int ek = 0;
    uint64_t next_v = 200;
    for (int i = 0; i < 5; i++) {
        uint64_t roff = wo_wal_next_offset(&w);
        T_EQ(wo_row_update_field_slot(&db, 0, id, 0, next_v, &msg, &ek), 0);
        T_EQ(ek, DB_ERR_NONE);
        T_EQ(wo_wal_pend_repoint(&w, 0, id, roff), 0);
        next_v += 100;
    }
    T_EQ(wo_wal_commit(&w), 0);
    wo_db_flush_drops(&db, &w);

    /* exactly one entry for this row, across EVERY bucket — a leaked entry
       would sit in a STALE bucket (the pre-first-update value's), not the
       current one, so this must scan the whole index, not just probe. */
    db_table *t = &db.tables[0];
    db_index *ix = &t->indexes[0];
    uint32_t hits = 0;
    for (size_t bi = 0; bi < ix->bcap; bi++) {
        db_ibucket *b = &ix->buckets[bi];
        for (uint32_t k = 0; k < b->len; k++)
            if (b->ids[k] == id) hits++;
    }
    T_EQ(hits, 1u);

    /* and it is reachable by its final value, 600 */
    uint64_t *ids;
    uint32_t cnt;
    T_EQ(wo_idx_probe(&db, 0, 0, 600, NULL, 0, &ids, &cnt), 1);
    T_CHECK(cnt == 1 && ids[0] == id);
    free(ids);

    db_row *r = wo_row_borrow(&db, 0, id, &msg);
    T_CHECK(r != NULL && r->slots[0] == 600);
    wo_row_release(&db, 0, r);

    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

/* Task 4 follow-up (review finding): the unique shadow-check's candidate
 * lookup must ALSO prefer a pending re-point over the durable map, for the
 * same reason back_off does. Before this task, two keys-resident updates
 * in one drain could not happen at all (the second crashed). Task 4 makes
 * it possible, which makes THIS reachable: request 1 updates row A's
 * unique-indexed column to a NEW value inside a drain (staged, not
 * committed, its index bucket already moved — that part is unconditional
 * RAM apply); request 2, same drain, updates a DIFFERENT row B to that
 * SAME new value. The shadow check finds A sitting in the target bucket
 * (correct — the bucket move is immediate) but, without the fix, verifies
 * A by folding it from its still-DURABLE (pre-update) offset — reading
 * A's OLD value, which does not match, so the real clash is missed and a
 * duplicate would be committed. Request 2 must be REFUSED. */
static void test_keys_resident_unique_clash_pending_repoint(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/keysuniqpend.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, KEYS_UNIQUE_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, KEYS_UNIQUE_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db.rt = &rt; rt.wal = &w; rt.db = &db;
    const char *msg = "";

    wo_str *sa = wo_str_new(&rt, "a", 1);
    wo_str *sb = wo_str_new(&rt, "b", 1);
    uint64_t va[2] = {100, (uint64_t)(uintptr_t)sa};
    uint64_t vb[2] = {200, (uint64_t)(uintptr_t)sb};
    uint64_t a = wo_row_insert(&db, 0, va, &msg, NULL);
    uint64_t b = wo_row_insert(&db, 0, vb, &msg, NULL);
    T_CHECK(a != 0 && b != 0);
    uint64_t off_a = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db, 0, a), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_drop_payload(&db, 0, a, off_a), 0);
    uint64_t off_b = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db, 0, b), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_drop_payload(&db, 0, b, off_b), 0);

    /* "request" 1: a's n 100 -> 300 — staged, NOT committed, map NOT moved
       (only pending), exactly db.c's request arm */
    int ek = 0;
    uint64_t roff_a = wo_wal_next_offset(&w);
    T_EQ(wo_row_update_field_slot(&db, 0, a, 0, 300, &msg, &ek), 0);
    T_EQ(ek, DB_ERR_NONE);
    T_EQ(wo_wal_pend_repoint(&w, 0, a, roff_a), 0);

    /* "request" 2, SAME drain: b's n 200 -> 300 collides with a's NEW
       (still only staged) value — must be refused */
    T_EQ(wo_row_update_field_slot(&db, 0, b, 0, 300, &msg, &ek), -1);
    T_EQ(ek, DB_ERR_UNIQUE);

    /* the drain's barrier: commit a's delta, flush its pending re-point */
    T_EQ(wo_wal_commit(&w), 0);
    wo_db_flush_drops(&db, &w);

    /* b untouched: still 200, still the only hit for 200; a alone at 300 */
    db_row *r = wo_row_borrow(&db, 0, b, &msg);
    T_CHECK(r != NULL && r->slots[0] == 200);
    wo_row_release(&db, 0, r);
    uint64_t *ids;
    uint32_t cnt;
    T_EQ(wo_idx_probe(&db, 0, 0, 300, NULL, 0, &ids, &cnt), 1);
    T_CHECK(cnt == 1 && ids[0] == a);
    free(ids);

    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

/* databasev2 2 (5c): boot. A keys-resident store must come back from replay
 * with its rows readable FROM THE LOG — the map rebuilt to offsets, not slabs.
 * This is the half the round-trip test cannot cover: it runs in a fresh db,
 * exactly as a restart would. */
static void test_keys_resident_delete(void) {
    /* databasev2 2 (5d): deleting a keys-resident row. Before the fix this
     * read the id map's LOG OFFSET as a slot index and handed it to slot_row,
     * which does no bounds check — so it indexed t->slabs[] with a byte offset
     * and then freed whatever it found. ASan reports it as a wild read or a
     * bad free, not as a wrong answer, which is why the annotation stays
     * refused at the loader until every operation is honest. */
    char path[128];
    snprintf(path, sizeof path, "%s/keysdel.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, KEYS_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, KEYS_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db.rt = &rt; rt.wal = &w; rt.db = &db;
    const char *msg = "";

    uint64_t ids[3];
    for (int i = 0; i < 3; i++) {
        wo_str *sv = wo_str_new(&rt, "del", 3);
        uint64_t vals[2] = {(uint64_t)(i + 500), (uint64_t)(uintptr_t)sv};
        ids[i] = wo_row_insert(&db, 0, vals, &msg, NULL);
        T_CHECK(ids[i] != 0);
        uint64_t off = wo_wal_next_offset(&w);
        T_EQ(wo_wal_append_insert(&w, &db, 0, ids[i]), 0);
        T_EQ(wo_wal_commit(&w), 0);
        T_EQ(wo_row_drop_payload(&db, 0, ids[i], off), 0);
    }
    T_CHECK(db.tables[0].count == 3);

    /* the offset is far larger than any slot index, which is exactly what made
     * the old path walk off the slab array */
    T_EQ(wo_row_remove(&db, 0, ids[1]), 0);
    T_CHECK(db.tables[0].count == 2);

    /* gone, and the survivors still read correctly through their own offsets */
    T_CHECK(wo_row_borrow(&db, 0, ids[1], &msg) == NULL);
    for (int i = 0; i < 3; i += 2) {
        db_row *r = wo_row_borrow(&db, 0, ids[i], &msg);
        T_CHECK(r != NULL);
        T_CHECK(r->slots[0] == (uint64_t)(i + 500));
        wo_row_release(&db, 0, r);
    }

    /* and a removed row must not come back through compaction */
    T_EQ(wo_wal_compact(&w, &db), 0);
    T_CHECK(wo_row_borrow(&db, 0, ids[1], &msg) == NULL);
    T_CHECK(db.tables[0].count == 2);

    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

static void test_keys_resident_delete_then_replay(void) {
    /* Does a keys-resident table survive a RESTART after a delete? The tombstone
     * has to replay, and replay reaches wo_row_remove, whose keys arm borrows
     * the row from the log to find its index entries. Replay runs BEFORE
     * rt->wal is wired (main.c sets it after), so the borrow has no log to read
     * and the remove fails — which replay reports as corruption. */
    char path[128];
    snprintf(path, sizeof path, "%s/keysdelreplay.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, KEYS_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, KEYS_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db.rt = &rt; rt.wal = &w; rt.db = &db;
    const char *msg = "";

    uint64_t ids[2];
    for (int i = 0; i < 2; i++) {
        wo_str *sv = wo_str_new(&rt, "dr", 2);
        uint64_t vals[2] = {(uint64_t)(i + 900), (uint64_t)(uintptr_t)sv};
        ids[i] = wo_row_insert(&db, 0, vals, &msg, NULL);
        uint64_t off = wo_wal_next_offset(&w);
        T_EQ(wo_wal_append_insert(&w, &db, 0, ids[i]), 0);
        T_EQ(wo_wal_commit(&w), 0);
        T_EQ(wo_row_drop_payload(&db, 0, ids[i], off), 0);
    }
    /* delete one, logging the tombstone the way the request path does */
    T_EQ(wo_row_remove(&db, 0, ids[0]), 0);
    T_EQ(wo_wal_append_remove(&w, 0, ids[0]), 0);
    T_EQ(wo_wal_commit(&w), 0);
    wo_wal_close(&w);
    wo_db_destroy(&db);

    /* the restart: replay has no rt->wal yet, exactly as main.c orders it */
    wo_db db2;
    T_EQ(wo_db_init(&db2, KEYS_CLASSES, 1, 0, 1), 0);
    db2.rt = &rt; rt.wal = NULL; rt.db = &db2;
    int64_t n = wo_wal_replay(path, &db2);
    T_CHECK(n >= 0); /* NOT corruption: a logged delete must replay */
    T_CHECK(db2.tables[0].count == 1); /* one survivor */
    wo_db_destroy(&db2);
    wo_rt_destroy(&rt);
}

static void test_keys_resident_survives_compaction(void) {
    /* databasev2 2 (5d): the obligation recorded at wo_wal_compact. Two ways
     * to fail it, both checked here:
     *   1. compaction walks the bitmap, so keys-resident rows — which hold no
     *      bitmap bit — are never written to the new log and vanish;
     *   2. compaction writes them but leaves the id map naming OLD offsets.
     * Rows are written back in HASH order, not insertion order, so almost
     * every offset really does move: a map left un-repointed cannot pass by
     * coincidence, it lands on another row and fails the id check. */
    char path[128];
    snprintf(path, sizeof path, "%s/keyscompact.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, KEYS_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, KEYS_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db.rt = &rt; rt.wal = &w; rt.db = &db;
    const char *msg = "";

    enum { N = 5 };
    uint64_t ids[N];
    char texts[N][8];
    for (int i = 0; i < N; i++) {
        /* varying lengths, so a record's position depends on what precedes it */
        int tl = 1 + i;
        memset(texts[i], 'a' + i, (size_t)tl);
        texts[i][tl] = 0;
        wo_str *sv = wo_str_new(&rt, texts[i], (size_t)tl);
        uint64_t vals[2] = {(uint64_t)(i * 101 + 7), (uint64_t)(uintptr_t)sv};
        ids[i] = wo_row_insert(&db, 0, vals, &msg, NULL);
        T_CHECK(ids[i] != 0);
        uint64_t off = wo_wal_next_offset(&w);
        T_EQ(wo_wal_append_insert(&w, &db, 0, ids[i]), 0);
        T_EQ(wo_wal_commit(&w), 0);
        T_EQ(wo_row_drop_payload(&db, 0, ids[i], off), 0);
    }

    T_EQ(wo_wal_compact(&w, &db), 0);

    /* every row still readable, with its own values, through the new log */
    for (int i = 0; i < N; i++) {
        db_row *r = wo_row_borrow(&db, 0, ids[i], &msg);
        T_CHECK(r != NULL);
        T_CHECK(r->slots[0] == (uint64_t)(i * 101 + 7));
        db_text *back = (db_text *)(uintptr_t)r->slots[1];
        T_CHECK(back != NULL && back->len == (size_t)(1 + i));
        T_CHECK(memcmp(back->bytes, texts[i], (size_t)(1 + i)) == 0);
        wo_row_release(&db, 0, r);
    }
    wo_wal_close(&w);
    wo_db_destroy(&db);

    /* and the compacted log replays to the same set in a fresh process */
    wo_db db2;
    T_EQ(wo_db_init(&db2, KEYS_CLASSES, 1, 0, 1), 0);
    T_EQ(wo_wal_replay(path, &db2), N);
    wo_wal w2;
    T_EQ(wo_wal_open(&w2, path, 1 << 16), 0);
    db2.rt = &rt; rt.wal = &w2; rt.db = &db2;
    for (int i = 0; i < N; i++) {
        db_row *r = wo_row_borrow(&db2, 0, ids[i], &msg);
        T_CHECK(r != NULL);
        T_CHECK(r->slots[0] == (uint64_t)(i * 101 + 7));
        wo_row_release(&db2, 0, r);
    }
    wo_wal_close(&w2);
    wo_db_destroy(&db2);
    wo_rt_destroy(&rt);
}

static void test_keys_resident_replay(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/keysboot.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, KEYS_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, KEYS_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db.rt = &rt; rt.wal = &w; rt.db = &db;
    const char *msg = "";

    uint64_t ids[3];
    for (int i = 0; i < 3; i++) {
        wo_str *sv = wo_str_new(&rt, "abc", 3);
        uint64_t vals[2] = {(uint64_t)(i * 11 + 1), (uint64_t)(uintptr_t)sv};
        ids[i] = wo_row_insert(&db, 0, vals, &msg, NULL);
        T_CHECK(ids[i] != 0);
        uint64_t off = wo_wal_next_offset(&w);
        T_EQ(wo_wal_append_insert(&w, &db, 0, ids[i]), 0);
        T_EQ(wo_wal_commit(&w), 0);
        T_EQ(wo_row_drop_payload(&db, 0, ids[i], off), 0);
    }
    wo_wal_close(&w);
    wo_db_destroy(&db);

    /* a fresh process would do exactly this */
    wo_db db2;
    T_EQ(wo_db_init(&db2, KEYS_CLASSES, 1, 0, 1), 0);
    T_EQ(wo_wal_replay(path, &db2), 3);
    wo_wal w2;
    T_EQ(wo_wal_open(&w2, path, 1 << 16), 0);
    db2.rt = &rt; rt.wal = &w2; rt.db = &db2;

    T_CHECK(db2.tables[0].count == 3); /* live, though nothing is in a slab */
    for (int i = 0; i < 3; i++) {
        db_row *r = wo_row_borrow(&db2, 0, ids[i], &msg);
        T_CHECK(r != NULL);
        T_CHECK(r->slots[0] == (uint64_t)(i * 11 + 1));
        db_text *back = (db_text *)(uintptr_t)r->slots[1];
        T_CHECK(back != NULL && back->len == 3 && memcmp(back->bytes, "abc", 3) == 0);
        wo_row_release(&db2, 0, r);
    }
    wo_wal_close(&w2);
    wo_db_destroy(&db2);
    wo_rt_destroy(&rt);
}

/* Task 5 (replay and compaction fold the same way): DELTA_CLASSES with a
 * non-unique index on field 0 — the chain-of-deltas tests below need THREE
 * touched fields (one delta each) but also an indexed column among them,
 * which DELTA_CLASSES (no index) and KEYS_IDX_CLASSES (only two fields)
 * don't together provide. */
static const uint8_t delta_idx_kinds[] = {WO_K_SCALAR, WO_K_SCALAR, WO_K_SCALAR};
static const uint32_t delta_idx_meta[] = {0 /*non-unique*/, 1, 0 /*col: field 0*/};
static const wo_classdesc DELTA_IDX_CLASSES[] = {
    {.name = 0, .flags = WO_CLASSF_RESIDENT_KEYS, .field_cnt = 3, .kinds = delta_idx_kinds,
     .idx_cnt = 1, .idx_meta = delta_idx_meta},
};

/* Task 5, step 1: a row with a chain of three deltas, replayed into a fresh
 * database, must read exactly as it did before the restart — including
 * through the secondary index on the column one of the deltas changed.
 * Before this task apply_record had no DELTA arm: a DELTA record in the log
 * made replay refuse the whole file as corruption. */
static void test_delta_chain_replay(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/deltareplay.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, DELTA_IDX_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, DELTA_IDX_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db.rt = &rt; rt.wal = &w; rt.db = &db;
    const char *msg = "";

    uint64_t vals[3] = {10, 20, 30};
    uint64_t id = wo_row_insert(&db, 0, vals, &msg, NULL);
    T_CHECK(id != 0);
    uint64_t base_off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db, 0, id), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_drop_payload(&db, 0, id, base_off), 0);

    /* field 0 (indexed): 10 -> 111 */
    uint64_t d1_off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_delta(&w, &db, 0, id, 0, base_off, 111), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_set_offset(&db, 0, id, d1_off), 0);

    /* field 1: 20 -> 222, chained off the first delta */
    uint64_t d2_off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_delta(&w, &db, 0, id, 1, d1_off, 222), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_set_offset(&db, 0, id, d2_off), 0);

    /* field 2: 30 -> 333, chained off the second delta — three deltas total */
    uint64_t d3_off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_delta(&w, &db, 0, id, 2, d2_off, 333), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_set_offset(&db, 0, id, d3_off), 0);

    /* what the row reads as BEFORE the restart */
    db_row *before = wo_row_borrow(&db, 0, id, &msg);
    T_CHECK(before != NULL);
    uint64_t want0 = before->slots[0], want1 = before->slots[1], want2 = before->slots[2];
    T_EQ(want0, 111);
    T_EQ(want1, 222);
    T_EQ(want2, 333);
    wo_row_release(&db, 0, before);

    wo_wal_close(&w);
    wo_db_destroy(&db);

    /* a fresh process would do exactly this: rt.wal is NULL (main.c wires
       the real log in only AFTER replay) so a DELTA's fold, mid-replay, runs
       through the lent view wo_wal_replay_ex sets up on its own — db.rt
       itself must already be set, exactly as main.c sets DB.rt before
       calling wo_wal_replay_ex. */
    wo_db db2;
    T_EQ(wo_db_init(&db2, DELTA_IDX_CLASSES, 1, 0, 1), 0);
    db2.rt = &rt;
    rt.wal = NULL;
    T_EQ(wo_wal_replay(path, &db2), 4); /* 1 insert + 3 deltas */
    wo_wal w2;
    T_EQ(wo_wal_open(&w2, path, 1 << 16), 0);
    rt.wal = &w2; rt.db = &db2;

    db_row *after = wo_row_borrow(&db2, 0, id, &msg);
    T_CHECK(after != NULL);
    T_EQ(after->slots[0], want0);
    T_EQ(after->slots[1], want1);
    T_EQ(after->slots[2], want2);
    wo_row_release(&db2, 0, after);

    /* the index a delta changed: rebuilt at boot from the INSERT's value,
       then folded forward by the delta that touched field 0 — a fold that
       disagreed between reading and replaying would leave this probing the
       stale value */
    uint64_t *ids;
    uint32_t cnt;
    T_EQ(wo_idx_probe(&db2, 0, 0, 111, NULL, 0, &ids, &cnt), 1);
    T_CHECK(cnt == 1 && ids[0] == id);
    free(ids);
    T_EQ(wo_idx_probe(&db2, 0, 0, 10, NULL, 0, &ids, &cnt), 1);
    T_CHECK(cnt == 0 && ids == NULL); /* the superseded value: no hits */

    wo_wal_close(&w2);
    wo_db_destroy(&db2);
    wo_rt_destroy(&rt);
}

/* Task 5, step 2: the same chain, then compacted. The row must read
 * identically AND its chain must be length zero afterwards — the record its
 * offset points at must be a full row, not a delta. That second assertion
 * is the one the brief calls out as easy to skip: without it this test
 * would still pass if compaction merely copied the chain byte-for-byte
 * instead of flattening it, since a byte-for-byte copy still reads back
 * correctly — it just never shortens the chain, which is the entire point
 * of a checkpoint. */
static void test_delta_chain_compact_flattens(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/deltacompact.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, DELTA_IDX_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, DELTA_IDX_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db.rt = &rt; rt.wal = &w; rt.db = &db;
    const char *msg = "";

    uint64_t vals[3] = {10, 20, 30};
    uint64_t id = wo_row_insert(&db, 0, vals, &msg, NULL);
    T_CHECK(id != 0);
    uint64_t base_off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db, 0, id), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_drop_payload(&db, 0, id, base_off), 0);

    uint64_t d1_off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_delta(&w, &db, 0, id, 0, base_off, 111), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_set_offset(&db, 0, id, d1_off), 0);

    uint64_t d2_off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_delta(&w, &db, 0, id, 1, d1_off, 222), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_set_offset(&db, 0, id, d2_off), 0);

    uint64_t d3_off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_delta(&w, &db, 0, id, 2, d2_off, 333), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_set_offset(&db, 0, id, d3_off), 0);

    T_EQ(wo_wal_compact(&w, &db), 0);

    /* reads identically, through the compacted log */
    db_row *r = wo_row_borrow(&db, 0, id, &msg);
    T_CHECK(r != NULL);
    T_EQ(r->slots[0], 111);
    T_EQ(r->slots[1], 222);
    T_EQ(r->slots[2], 333);
    wo_row_release(&db, 0, r);

    /* THE assertion the brief calls out: the offset now names a FULL ROW,
       not a delta — chain length zero, not merely "still readable" */
    uint64_t o1 = wo_row_offset1(&db, 0, id);
    T_CHECK(o1 != 0);
    uint8_t kind_byte = 0xFF;
    T_EQ((int)pread(w.fd, &kind_byte, 1, (off_t)(o1 - 1 + 8)), 1);
    T_EQ(kind_byte, WO_WAL_INSERT);

    wo_wal_close(&w);
    wo_db_destroy(&db);

    /* the compacted log replays to the same, flattened, state */
    wo_db db2;
    T_EQ(wo_db_init(&db2, DELTA_IDX_CLASSES, 1, 0, 1), 0);
    db2.rt = &rt;
    rt.wal = NULL; /* see the replay comment above: set before replaying */
    T_EQ(wo_wal_replay(path, &db2), 1); /* one row, one INSERT — chain gone */
    wo_wal w2;
    T_EQ(wo_wal_open(&w2, path, 1 << 16), 0);
    rt.wal = &w2; rt.db = &db2;
    db_row *r2 = wo_row_borrow(&db2, 0, id, &msg);
    T_CHECK(r2 != NULL);
    T_EQ(r2->slots[0], 111);
    T_EQ(r2->slots[1], 222);
    T_EQ(r2->slots[2], 333);
    wo_row_release(&db2, 0, r2);
    wo_wal_close(&w2);
    wo_db_destroy(&db2);
    wo_rt_destroy(&rt);
}

/* Task 5, step 3: the crash window commit-before-re-point ordering exists
 * for. Append a delta, commit it (durable), and deliberately do NOT
 * re-point the map — exactly the state a crash between the barrier and the
 * flush leaves behind (wo_db_flush_drops never got to run). Replaying the
 * log into a FRESH database, which never sees this process's map at all,
 * must still surface the update: the commit alone is what makes a delta
 * recoverable, not the in-RAM re-point, and this is the test that would
 * fail if that ordering were ever reversed. */
static void test_delta_crash_window(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/deltacrash.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, DELTA_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, DELTA_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db.rt = &rt; rt.wal = &w; rt.db = &db;
    const char *msg = "";

    uint64_t vals[3] = {1, 2, 3};
    uint64_t id = wo_row_insert(&db, 0, vals, &msg, NULL);
    T_CHECK(id != 0);
    uint64_t base_off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db, 0, id), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_drop_payload(&db, 0, id, base_off), 0);

    /* field 0: 1 -> 999. Committed (durable) but NEVER re-pointed: nothing
       after wo_wal_commit below runs — this IS the crash. */
    T_EQ(wo_wal_append_delta(&w, &db, 0, id, 0, base_off, 999), 0);
    T_EQ(wo_wal_commit(&w), 0);

    wo_wal_close(&w);
    wo_db_destroy(&db);

    /* a fresh process, with no memory of this one's (never re-pointed) map */
    wo_db db2;
    T_EQ(wo_db_init(&db2, DELTA_CLASSES, 1, 0, 1), 0);
    db2.rt = &rt;
    rt.wal = NULL; /* see the replay comment in test_delta_chain_replay */
    T_EQ(wo_wal_replay(path, &db2), 2); /* insert + delta */
    wo_wal w2;
    T_EQ(wo_wal_open(&w2, path, 1 << 16), 0);
    rt.wal = &w2; rt.db = &db2;

    db_row *r = wo_row_borrow(&db2, 0, id, &msg);
    T_CHECK(r != NULL);
    T_EQ(r->slots[0], 999); /* the update IS present */
    T_EQ(r->slots[1], 2);
    T_EQ(r->slots[2], 3);
    wo_row_release(&db2, 0, r);

    wo_wal_close(&w2);
    wo_db_destroy(&db2);
    wo_rt_destroy(&rt);
}

static void test_torn_tail(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/torn.wal", g_dir);
    wo_db db;
    T_EQ(wo_db_init(&db, CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 0), 0);
    const char *msg = "";
    for (int i = 0; i < 5; i++) {
        uint64_t vals[2] = {(uint64_t)i, 0};
        uint64_t id = wo_row_insert(&db, 0, vals, &msg, NULL);
        T_EQ(wo_wal_append_insert(&w, &db, 0, id), 0);
        T_EQ(wo_wal_commit(&w), 0);
    }
    uint64_t intact_end = w.off;
    /* tear: append half a record's worth of a valid-looking header + junk */
    uint32_t fake_len = 40, fake_crc = 0xDEAD;
    uint8_t junk[20] = {7, 7, 7};
    T_CHECK(pwrite(w.fd, &fake_len, 4, (off_t)intact_end) == 4);
    T_CHECK(pwrite(w.fd, &fake_crc, 4, (off_t)(intact_end + 4)) == 4);
    T_CHECK(pwrite(w.fd, junk, sizeof junk, (off_t)(intact_end + 8)) == (ssize_t)sizeof junk);
    wo_wal_close(&w);
    wo_db_destroy(&db);

    /* the oracle sees exactly the intact prefix */
    uint64_t at = 0;
    T_EQ(wo_wal_check(path, &at), 5);
    T_EQ(at, intact_end);

    /* replay drops the tear whole */
    wo_db db2;
    T_EQ(wo_db_init(&db2, CLASSES, 1, 0, 1), 0);
    T_EQ(wo_wal_replay(path, &db2), 5);
    T_EQ(db2.tables[0].count, 5);
    wo_db_destroy(&db2);

    /* reopen positions AT the tear: the next commit overwrites it */
    wo_db db3;
    T_EQ(wo_db_init(&db3, CLASSES, 1, 0, 1), 0);
    T_EQ(wo_wal_replay(path, &db3), 5);
    wo_wal w2;
    T_EQ(wo_wal_open(&w2, path, 0), 0);
    T_EQ(w2.off, intact_end);
    uint64_t vals[2] = {100, 0};
    uint64_t id = wo_row_insert(&db3, 0, vals, &msg, NULL);
    T_EQ(wo_wal_append_insert(&w2, &db3, 0, id), 0);
    T_EQ(wo_wal_commit(&w2), 0);
    wo_wal_close(&w2);
    T_EQ(wo_wal_check(path, NULL), 6); /* tear gone, record in its place */
    wo_db_destroy(&db3);
}

/* ---- the crash battery -------------------------------------------------- */

/* Child: insert forever — RAM, WAL, COMMIT, and only then ack the id down
 * the pipe. Killed mid-stream by the parent. */
static void battery_child(const char *path, int ack_fd) {
    wo_rt rt;
    wo_db db;
    wo_wal w;
    if (wo_rt_init(&rt, 1 << 20, CLASSES, 1) != 0) _exit(9);
    if (wo_db_init(&db, CLASSES, 1, 0, 1) != 0) _exit(9);
    if (wo_wal_open(&w, path, 1 << 20) != 0) _exit(9);
    const char *msg = "";
    for (uint64_t i = 0;; i++) {
        char label[32];
        int n = snprintf(label, sizeof label, "row-%llu", (unsigned long long)i);
        wo_str *s = wo_str_new(&rt, label, (uint32_t)n);
        uint64_t vals[2] = {i * 3 + 1, (uint64_t)(uintptr_t)s};
        uint64_t id = wo_row_insert(&db, 0, vals, &msg, NULL);
        wo_str_free(&rt, s);
        if (!id) _exit(9);
        if (wo_wal_append_insert(&w, &db, 0, id) != 0) _exit(9);
        if (wo_wal_commit(&w) != 0) _exit(9); /* durable BEFORE the ack */
        ssize_t wr = write(ack_fd, &id, 8);
        if (wr != 8) _exit(0); /* parent went away */
    }
}

static void test_crash_battery(void) {
    for (int round = 0; round < 5; round++) {
        char path[128];
        snprintf(path, sizeof path, "%s/crash-%d.wal", g_dir, round);
        int pipefd[2];
        T_EQ(pipe(pipefd), 0);
        pid_t pid = fork();
        T_CHECK(pid >= 0);
        if (pid == 0) {
            close(pipefd[0]);
            battery_child(path, pipefd[1]);
            _exit(0);
        }
        close(pipefd[1]);
        /* collect acks for a few ms, then kill mid-stream — no sync with
           the child's commit loop, which is the point */
        struct timespec ts = {0, (20 + round * 13) * 1000000L};
        while (nanosleep(&ts, &ts) != 0) {}
        kill(pid, SIGKILL);
        int status;
        waitpid(pid, &status, 0);
        /* drain every ack that made it into the pipe */
        uint64_t acked[65536];
        size_t n_acked = 0;
        for (;;) {
            uint64_t id;
            ssize_t n = read(pipefd[0], &id, 8);
            if (n != 8) break;
            if (n_acked < 65536) acked[n_acked++] = id;
        }
        close(pipefd[0]);
        T_CHECK(n_acked > 0); /* the child got at least one commit out */

        /* offline oracle: the file's intact prefix covers every ack */
        int64_t intact = wo_wal_check(path, NULL);
        T_CHECK(intact >= (int64_t)n_acked);

        /* replay and verify: every acked id present, contents exact */
        wo_rt rt;
        T_EQ(wo_rt_init(&rt, 1 << 22, CLASSES, 1), 0);
        wo_db db;
        T_EQ(wo_db_init(&db, CLASSES, 1, 0, 1), 0);
        int64_t applied = wo_wal_replay(path, &db);
        T_CHECK(applied >= (int64_t)n_acked);
        const char *msg = "";
        int bad = 0;
        for (size_t i = 0; i < n_acked; i++) {
            uint64_t out[2];
            if (wo_row_read(&db, &rt, 0, acked[i], out, &msg) != 0) {
                bad++;
                continue;
            }
            /* id = i+1 (shard 0 of 1), field 0 = i*3+1, label = "row-i" */
            char want[32];
            int wl = snprintf(want, sizeof want, "row-%llu",
                              (unsigned long long)(acked[i] - 1));
            wo_str *s = (wo_str *)(uintptr_t)out[1];
            if (out[0] != (acked[i] - 1) * 3 + 1 || s->len != (uint32_t)wl ||
                memcmp(s->data, want, (size_t)wl) != 0)
                bad++;
            wo_str_free(&rt, s);
        }
        T_EQ(bad, 0); /* zero acked-but-missing, zero acked-but-wrong */
        wo_db_destroy(&db);
        wo_rt_destroy(&rt);
    }
}

/* iteration 19: a Float column and a Bytes column survive a WAL round trip
 * BIT-EXACT. Bit-exact is the whole assertion — the durability path must not
 * render a float as decimal anywhere, or NaN, the infinities and -0.0 would
 * each come back as something else. Bytes goes through the same length-
 * prefixed blob a Text does and must come back as a Bytes, not a Text. */
static const uint8_t fb_kinds[] = {WO_K_FLOAT, WO_K_BYTES};
static const wo_classdesc FB_CLASSES[] = {
    {.name = 0, .flags = 0, .field_cnt = 2, .kinds = fb_kinds},
};

static void test_float_bytes_replay(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/floatbytes.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, FB_CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, FB_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    const char *msg = "";

    /* the values a decimal round trip would destroy, plus a NUL-bearing blob
     * that a NUL-terminated string path would truncate */
    const double vals_f[] = {9.99, 0.0 / 0.0, 1.0 / 0.0, -1.0 / 0.0, -0.0, 1e308};
    const char blob[] = {'a', '\0', 'b'};
    enum { N = sizeof vals_f / sizeof vals_f[0] };
    uint64_t ids[N];
    for (int i = 0; i < N; i++) {
        wo_str *b = wo_bytes_new(&rt, blob, sizeof blob);
        T_CHECK(b != NULL);
        uint64_t vals[2] = {wo_bits(vals_f[i]), (uint64_t)(uintptr_t)b};
        ids[i] = wo_row_insert(&db, 0, vals, &msg, NULL);
        T_CHECK(ids[i] != 0);
        T_EQ(wo_wal_append_insert(&w, &db, 0, ids[i]), 0);
        wo_str_free(&rt, b);
    }
    T_EQ(wo_wal_commit(&w), 0);
    wo_wal_close(&w);
    wo_db_destroy(&db);

    wo_db db2;
    T_EQ(wo_db_init(&db2, FB_CLASSES, 1, 0, 1), 0);
    T_EQ(wo_wal_replay(path, &db2), N);
    for (int i = 0; i < N; i++) {
        uint64_t out[2];
        T_EQ(wo_row_read(&db2, &rt, 0, ids[i], out, &msg), 0);
        /* BITS, not value: NaN != NaN and -0.0 == 0.0, so a value comparison
         * would pass while silently having lost the payload or the sign */
        T_EQ(out[0], wo_bits(vals_f[i]));
        wo_str *b = (wo_str *)(uintptr_t)out[1];
        T_CHECK(b != NULL);
        T_EQ(b->h.class_id, WO_CLS_BYTES); /* a Bytes column yields a Bytes */
        T_CHECK(b->len == sizeof blob && memcmp(b->data, blob, sizeof blob) == 0);
        wo_str_free(&rt, b);
    }
    wo_db_destroy(&db2);
    wo_rt_destroy(&rt);
}


/* databasev2 2: offset capture. wo_wal_next_offset must name exactly where a
 * record lands, so a resident:keys table can read it back by that offset
 * later. A wrong offset is the worst possible bug here: it reads a
 * NEIGHBOURING record, which passes its own CRC and returns the wrong row
 * silently. So this asserts the recovered id per record, not just that a
 * record parses.
 *
 * Covers the two awkward cases the design called out: records straddling a
 * buffer growth (stage() doubles from 4096, so 400 rows with Text payloads
 * cross it repeatedly), and a batch spanning several commits. */
static void test_offset_capture(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/offsets.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    const char *msg = "";

    enum { N = 400 };
    uint64_t ids[N], offs[N];

    /* commit in uneven batches so offsets are exercised both mid-buffer and
     * immediately after a flush reset len to 0 */
    for (int i = 0; i < N; i++) {
        char lbl[32];
        int ln = snprintf(lbl, sizeof lbl, "label-%d-padding", i);
        wo_str *s = wo_str_new(&rt, lbl, (uint32_t)ln);
        uint64_t vals[2] = {(uint64_t)i, (uint64_t)(uintptr_t)s};
        ids[i] = wo_row_insert(&db, 0, vals, &msg, NULL);
        T_CHECK(ids[i] != 0);
        /* BEFORE the append: this is the contract */
        offs[i] = wo_wal_next_offset(&w);
        T_EQ(wo_wal_append_insert(&w, &db, 0, ids[i]), 0);
        wo_str_free(&rt, s);
        if (i % 7 == 6) T_EQ(wo_wal_commit(&w), 0);
    }
    T_EQ(wo_wal_commit(&w), 0);

    /* offsets must be strictly increasing and inside the written region */
    for (int i = 1; i < N; i++) T_CHECK(offs[i] > offs[i - 1]);

    /* read each record back BY ITS REPORTED OFFSET and check the id matches:
     * payload is [kind u8][class u32][id u64], after the 8-byte len+crc head */
    int checked = 0;
    for (int i = 0; i < N; i++) {
        uint8_t head[8], body[13];
        T_EQ((int)pread(w.fd, head, 8, (off_t)offs[i]), 8);
        T_EQ((int)pread(w.fd, body, 13, (off_t)(offs[i] + 8)), 13);
        T_EQ(body[0], WO_WAL_INSERT);
        uint32_t cid;
        uint64_t rid;
        memcpy(&cid, body + 1, 4);
        memcpy(&rid, body + 5, 8);
        T_EQ(cid, 0u);
        T_EQ(rid, ids[i]);
        checked++;
    }
    T_EQ(checked, N);

    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

/* databasev2 2: an offset reported for a record whose commit FAILED must
 * never be trusted. Simulated by closing the fd under the wal so pwrite
 * fails: the offset accessor must not have advanced past the durable tail,
 * so a later successful commit reuses the same place. */
static void test_offset_after_failed_commit(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/offfail.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    const char *msg = "";

    uint64_t vals[2] = {7u, 0u};
    uint64_t id1 = wo_row_insert(&db, 0, vals, &msg, NULL);
    T_CHECK(id1 != 0);
    uint64_t at1 = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db, 0, id1), 0);

    /* break the fd, so the commit cannot succeed */
    int saved = dup(w.fd);
    T_CHECK(saved >= 0);
    close(w.fd);
    w.fd = -1;
    T_CHECK(wo_wal_commit(&w) != 0);
    /* The DURABLE TAIL is what must not move. `next_offset` legitimately
     * points PAST the still-staged record (off unchanged, len still holding
     * it) — asserting otherwise was this test's own first mistake. The
     * invariant that matters: off is untouched, so the record still lands at
     * the offset already reported for it. */
    T_EQ(w.off, at1);

    /* restore and commit for real: the record lands exactly where promised */
    w.fd = saved;
    T_EQ(wo_wal_commit(&w), 0);
    uint8_t body[13];
    T_EQ((int)pread(w.fd, body, 13, (off_t)(at1 + 8)), 13);
    uint64_t rid;
    memcpy(&rid, body + 5, 8);
    T_EQ(rid, id1);

    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}


/* databasev2 2 (5b): read rows back BY OFFSET and deep-compare.
 *
 * The point is not that a record parses — test_offset_capture already showed
 * the offsets are right. The point is that the VALUES come back intact,
 * including a nil Text, and that the two refusal paths refuse instead of
 * handing back something plausible. */
static void test_read_row_at(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/readat.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, CLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    const char *msg = "";

    enum { N = 24 };
    uint64_t ids[N], offs[N];
    const char *labels[N];

    for (int i = 0; i < N; i++) {
        /* every third row has a NIL Text, so the nil path is covered */
        wo_str *s = NULL;
        if (i % 3 != 0) {
            char lbl[24];
            int ln = snprintf(lbl, sizeof lbl, "row-%d", i);
            s = wo_str_new(&rt, lbl, (uint32_t)ln);
            T_CHECK(s != NULL);
        }
        uint64_t vals[2] = {(uint64_t)(i * 3 + 1), (uint64_t)(uintptr_t)s};
        ids[i] = wo_row_insert(&db, 0, vals, &msg, NULL);
        T_CHECK(ids[i] != 0);
        labels[i] = (i % 3 != 0) ? "set" : "nil";
        offs[i] = wo_wal_next_offset(&w);
        T_EQ(wo_wal_append_insert(&w, &db, 0, ids[i]), 0);
        if (s) wo_str_free(&rt, s);
    }
    T_EQ(wo_wal_commit(&w), 0);

    /* read each row back by offset and compare field by field */
    for (int i = 0; i < N; i++) {
        uint64_t got[2] = {0, 0};
        uint32_t cid = 0xFFFFFFFFu;
        uint64_t id = 0;
        T_EQ(wo_wal_read_row_at(&w, &db, &rt, offs[i], &cid, &id, got, &msg), 0);
        T_EQ(cid, 0u);
        T_EQ(id, ids[i]);
        T_EQ(got[0], (uint64_t)(i * 3 + 1));
        if (labels[i][0] == 'n') {
            T_EQ(got[1], 0u); /* nil Text stays nil through the round trip */
        } else {
            wo_str *back = (wo_str *)(uintptr_t)got[1];
            T_CHECK(back != NULL);
            char want[24];
            int wl = snprintf(want, sizeof want, "row-%d", i);
            T_EQ((int)back->len, wl);
            T_EQ(memcmp(back->data, want, (size_t)wl), 0);
            wo_str_free(&rt, back); /* out-gate: the VM value is ours to free */
        }
    }

    /* refusal 1: a tombstone is refused, not decoded as a live row */
    T_EQ(wo_row_remove(&db, 0, ids[0]), 0);
    uint64_t tomb_off = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_remove(&w, 0, ids[0]), 0);
    T_EQ(wo_wal_commit(&w), 0);
    {
        uint64_t got[2] = {0, 0};
        T_EQ(wo_wal_read_row_at(&w, &db, &rt, tomb_off, NULL, NULL, got, &msg), -1);
    }

    /* refusal 2: a wrong offset (mid-record) refuses rather than returning a
     * neighbouring row -- the silent-wrong-row failure this guards */
    {
        uint64_t got[2] = {0, 0};
        T_EQ(wo_wal_read_row_at(&w, &db, &rt, offs[5] + 3u, NULL, NULL, got, &msg), -1);
    }

    /* refusal 3: past the end of the intact prefix */
    {
        uint64_t got[2] = {0, 0};
        T_EQ(wo_wal_read_row_at(&w, &db, &rt, w.off + 4096u, NULL, NULL, got, &msg), -1);
    }

    wo_wal_close(&w);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

/* Task 6, Step 5: the oracle test. `resident: all` never goes near a delta —
 * every update is a direct slab mutation — so running the SAME sequence of
 * updates against a `resident: all` table and a `resident: keys` table and
 * asserting the rows read identically at every step is the strongest
 * available proof that the fold agrees with ordinary storage: the resident
 * table is the oracle, exactly what an independent implementation would be,
 * without needing to write one. CLASSES (flags=0) and KEYS_CLASSES
 * (WO_CLASSF_RESIDENT_KEYS) share the same shape — {n: scalar, label: Text}
 * — already declared above for other tests. */
static void assert_rows_equal(wo_db *db_all, uint64_t id_all, wo_db *db_keys,
                              uint64_t id_keys, wo_rt *rt, const char *step) {
    uint64_t out_all[2], out_keys[2];
    const char *msg = "";
    T_EQ(wo_row_read(db_all, rt, 0, id_all, out_all, &msg), 0);
    T_EQ(wo_row_read(db_keys, rt, 0, id_keys, out_keys, &msg), 0);
    T_CHECK(out_all[0] == out_keys[0]);
    wo_str *sa = (wo_str *)(uintptr_t)out_all[1];
    wo_str *sk = (wo_str *)(uintptr_t)out_keys[1];
    int text_eq = (!sa && !sk) ||
                  (sa && sk && sa->len == sk->len && memcmp(sa->data, sk->data, sa->len) == 0);
    if (!text_eq) fprintf(stderr, "oracle mismatch at %s\n", step);
    T_CHECK(text_eq);
    if (sa) wo_str_free(rt, sa);
    if (sk) wo_str_free(rt, sk);
}

static void test_oracle_all_vs_keys_same_update_sequence(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/oracle.wal", g_dir);
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, KEYS_CLASSES, 1), 0);
    /* the oracle needs no WAL at all: row_apply_field_slot never touches one */
    wo_db db_all;
    T_EQ(wo_db_init(&db_all, CLASSES, 1, 0, 1), 0);
    wo_db db_keys;
    T_EQ(wo_db_init(&db_keys, KEYS_CLASSES, 1, 0, 1), 0);
    wo_wal w;
    T_EQ(wo_wal_open(&w, path, 1 << 16), 0);
    db_keys.rt = &rt; rt.wal = &w; rt.db = &db_keys;
    const char *msg = "";

    wo_str *s0a = wo_str_new(&rt, "start", 5);
    wo_str *s0k = wo_str_new(&rt, "start", 5);
    uint64_t va[2] = {10, (uint64_t)(uintptr_t)s0a};
    uint64_t vk[2] = {10, (uint64_t)(uintptr_t)s0k};
    uint64_t id_all = wo_row_insert(&db_all, 0, va, &msg, NULL);
    uint64_t id_keys = wo_row_insert(&db_keys, 0, vk, &msg, NULL);
    T_CHECK(id_all != 0 && id_keys != 0);
    /* drop the keys row's payload to the log now, exactly as a post-barrier
       flush would — every update below folds it back out of the WAL */
    uint64_t koff = wo_wal_next_offset(&w);
    T_EQ(wo_wal_append_insert(&w, &db_keys, 0, id_keys), 0);
    T_EQ(wo_wal_commit(&w), 0);
    T_EQ(wo_row_drop_payload(&db_keys, 0, id_keys, koff), 0);
    assert_rows_equal(&db_all, id_all, &db_keys, id_keys, &rt, "insert");

    /* the sequence: scalar and Text fields both move, more than once each,
       so the fold is exercised on a multi-hop chain the same shape a real
       catalogue would build one small update at a time */
    struct { int field; uint64_t scalar; const char *text; } steps[] = {
        {0, 20, NULL},   {1, 0, "mid1"},  {0, 30, NULL},
        {1, 0, "mid2"},  {0, 40, NULL},   {1, 0, "end"},
    };
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        int ek = 0;
        uint64_t val_all, val_keys;
        if (steps[i].field == 0) {
            val_all = steps[i].scalar;
            val_keys = steps[i].scalar;
        } else {
            uint32_t tl = (uint32_t)strlen(steps[i].text);
            val_all = (uint64_t)(uintptr_t)wo_str_new(&rt, steps[i].text, tl);
            val_keys = (uint64_t)(uintptr_t)wo_str_new(&rt, steps[i].text, tl);
        }
        T_EQ(wo_row_update_field(&db_all, 0, id_all, (uint32_t)steps[i].field, val_all,
                                 &msg, &ek),
             0);
        T_EQ(ek, DB_ERR_NONE);

        uint64_t roff = wo_wal_next_offset(&w);
        T_EQ(wo_row_update_field(&db_keys, 0, id_keys, (uint32_t)steps[i].field, val_keys,
                                 &msg, &ek),
             0);
        T_EQ(ek, DB_ERR_NONE);
        T_EQ(wo_wal_commit(&w), 0);
        T_EQ(wo_row_set_offset(&db_keys, 0, id_keys, roff), 0);

        char label[32];
        snprintf(label, sizeof label, "step %zu", i);
        assert_rows_equal(&db_all, id_all, &db_keys, id_keys, &rt, label);
    }

    /* databasev2 11 closure: the same sequence continued 3×K steps past the
       six above, alternating scalar and Text, so the keys chain is TERMINATED
       by a full-row image more than once while the oracle keeps mutating its
       slab. Equality is re-asserted after every step; the fold's hop count
       proves the boundary was crossed, not merely approached. */
    uint32_t peak = 0, resets = 0;
    for (uint32_t n = 1; n <= WO_DELTA_MAX_HOPS * 3u; n++) {
        uint32_t field = n & 1u;
        uint64_t val_all, val_keys;
        if (field == 0) {
            val_all = val_keys = 100u + n;
        } else {
            char text[16];
            snprintf(text, sizeof text, "t%u", n);
            uint32_t tl = (uint32_t)strlen(text);
            val_all = (uint64_t)(uintptr_t)wo_str_new(&rt, text, tl);
            val_keys = (uint64_t)(uintptr_t)wo_str_new(&rt, text, tl);
        }
        int ek = 0;
        T_EQ(wo_row_update_field(&db_all, 0, id_all, field, val_all, &msg, &ek), 0);
        T_EQ(ek, DB_ERR_NONE);
        chain_update(&db_keys, &w, 0, id_keys, field, val_keys);

        uint32_t hops = 0;
        uint64_t out[2] = {0, 0};
        const char *fm = "";
        uint64_t o1 = wo_row_offset1(&db_keys, 0, id_keys);
        T_CHECK(o1 != 0);
        T_EQ(wo_wal_fold_row_at(&w, &db_keys, o1 - 1, NULL, NULL, out, &hops, &fm), 0);
        for (uint32_t i = 0; i < 2; i++) wo_db_val_free(&db_keys, KEYS_CLASSES[0].kinds[i], out[i]);
        if (hops > peak) peak = hops;
        if (hops == 0) resets++;

        char label[32];
        snprintf(label, sizeof label, "flatten step %u", n);
        assert_rows_equal(&db_all, id_all, &db_keys, id_keys, &rt, label);
    }
    T_CHECK(peak <= WO_DELTA_MAX_HOPS);   /* the bound held throughout */
    T_CHECK(resets >= 2);                 /* and was actually crossed, twice */

    /* and across a restart: replay the keys log into a fresh store and compare
       it against the oracle, which never left RAM — the criterion as written */
    wo_wal_close(&w);
    wo_db_destroy(&db_keys);
    wo_db db_keys2;
    T_EQ(wo_db_init(&db_keys2, KEYS_CLASSES, 1, 0, 1), 0);
    db_keys2.rt = &rt; rt.wal = NULL; rt.db = &db_keys2;
    T_CHECK(wo_wal_replay(path, &db_keys2) >= 0);
    wo_wal w2;
    T_EQ(wo_wal_open(&w2, path, 1 << 16), 0);
    rt.wal = &w2;
    assert_rows_equal(&db_all, id_all, &db_keys2, id_keys, &rt, "after replay");

    wo_wal_close(&w2);
    wo_db_destroy(&db_all);
    wo_db_destroy(&db_keys2);
    wo_rt_destroy(&rt);
}

int main(void) {
    snprintf(g_dir, sizeof g_dir, "/tmp/wo-wal-test-XXXXXX");
    if (!mkdtemp(g_dir)) return 1;
    test_roundtrip_replay();
    test_commit_failure_detected();
    test_compact_shortens_and_replays_equal();
    test_keys_resident_round_trip();
    test_delta_record();
    test_delta_fold_two_fields();
    test_delta_fold_same_field_newest_wins();
    test_fold_refuses_self_pointing_delta();
    test_fold_refuses_forward_pointing_delta();
    test_keys_resident_update_field();
    test_keys_resident_update_indexed();
    test_keys_resident_fresh_log_first_row();
    test_keys_resident_indexed_across_flatten();
    test_migrate_reorder_owned();
    test_migrate_delta_splice();
    test_migrate_add_field();
    test_migrate_delete_field();
    test_migrate_crash_before_rename();
    test_migrate_poison_needs_records();
    test_migrate_corrupt_input();
    test_schema_diff_verdicts();
    test_schema_roundtrip();
    test_schema_fresh_log();
    test_fold_row_at_tolerates_null_msg();
    test_schema_compaction_adopts_legacy();
    test_schema_read_absent();
    test_should_compact_absolute_and_ceiling();
    test_delta_chain_flattens_at_k();
    test_delta_chain_flatten_replays();
    test_keys_resident_update_indexed_text();
    test_keys_resident_update_unique_violation_refused();
    test_keys_resident_two_updates_one_drain();
    test_keys_resident_repeat_updates_one_drain_index();
    test_keys_resident_unique_clash_pending_repoint();
    test_keys_resident_replay();
    test_keys_resident_survives_compaction();
    test_delta_chain_replay();
    test_delta_chain_compact_flattens();
    test_delta_crash_window();
    test_keys_resident_delete();
    test_keys_resident_delete_then_replay();
    test_stale_compact_temp_is_removed();
    test_resolve_data_path();
    test_file_form_temps_beside_log();
    test_should_compact_policy();
    test_compact_refuses_with_staged_records();
    test_torn_tail();
    test_float_bytes_replay();
    test_offset_capture();
    test_offset_after_failed_commit();
    test_read_row_at();
    test_crash_battery();
    test_compact_crash_battery();
    test_oracle_all_vs_keys_same_update_sequence();
    /* leave the dir for a failed run's forensics only */
    if (!t_fail) {
        char cmd[128];
        snprintf(cmd, sizeof cmd, "rm -rf %s", g_dir);
        if (system(cmd) != 0) fprintf(stderr, "cleanup failed, kept %s\n", g_dir);
    } else {
        fprintf(stderr, "kept %s\n", g_dir);
    }
    return t_report("test_wal");
}

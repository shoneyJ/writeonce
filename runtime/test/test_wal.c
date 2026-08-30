/* test_wal — iteration 9 Task 2: typed WAL + boot replay.
 * Round-trip through a replay, torn-tail drop, reopen-overwrites-tear,
 * and the commit-then-kill crash battery: a forked child inserts rows and
 * acks each COMMITTED id over a pipe; SIGKILL lands mid-stream; the parent
 * verifies with the offline oracle and a replay that every acked id is
 * present with the right contents. */
#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
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
    wo_str *back = (wo_str *)(uintptr_t)r->slots[1];
    T_CHECK(back != NULL && back->len == 5 && memcmp(back->data, "hello", 5) == 0);
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
 * a `resident: keys` table is not implemented". */
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
    T_EQ(wo_row_update_field(&db, 0, id, 0, 999, &msg, &ek), 0);
    T_EQ(ek, DB_ERR_NONE);

    db_row *r = wo_row_borrow(&db, 0, id, &msg);
    T_CHECK(r != NULL);
    T_CHECK(r->slots[0] == 999);
    wo_str *back = (wo_str *)(uintptr_t)r->slots[1];
    T_CHECK(back != NULL && back->len == 5 && memcmp(back->data, "hello", 5) == 0);
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

/* Task 3, the test that matters: updating an INDEXED column on a
 * keys-resident row must move the row in the index too, not just in the
 * log — queried through wo_idx_probe, the row is found by its NEW value and
 * gone from its OLD one. */
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
    T_EQ(wo_row_update_field(&db, 0, a, 0, 150, &msg, &ek), 0);
    T_EQ(ek, DB_ERR_NONE);

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
        wo_str *back = (wo_str *)(uintptr_t)r->slots[1];
        T_CHECK(back != NULL && back->len == (size_t)(1 + i));
        T_CHECK(memcmp(back->data, texts[i], (size_t)(1 + i)) == 0);
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
        wo_str *back = (wo_str *)(uintptr_t)r->slots[1];
        T_CHECK(back != NULL && back->len == 3 && memcmp(back->data, "abc", 3) == 0);
        wo_row_release(&db2, 0, r);
    }
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
    test_keys_resident_replay();
    test_keys_resident_survives_compaction();
    test_keys_resident_delete();
    test_keys_resident_delete_then_replay();
    test_stale_compact_temp_is_removed();
    test_should_compact_policy();
    test_compact_refuses_with_staged_records();
    test_torn_tail();
    test_float_bytes_replay();
    test_offset_capture();
    test_offset_after_failed_commit();
    test_read_row_at();
    test_crash_battery();
    test_compact_crash_battery();
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

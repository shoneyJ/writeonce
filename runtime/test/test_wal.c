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

int main(void) {
    snprintf(g_dir, sizeof g_dir, "/tmp/wo-wal-test-XXXXXX");
    if (!mkdtemp(g_dir)) return 1;
    test_roundtrip_replay();
    test_commit_failure_detected();
    test_torn_tail();
    test_float_bytes_replay();
    test_crash_battery();
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

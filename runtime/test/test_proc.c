/* test_proc — iteration 42: proc.run's contract, then its bounds.
 *
 * Task 2 pins what the one-shot form already promises (exit code, captured
 * stdout, the execvp-failed 127) so the parked rework in Task 3 has a
 * baseline that must not move. Later tasks add the bounds legs: deadline,
 * output caps, ceiling, fd hygiene, stop/unwind reaping. */
#define _POSIX_C_SOURCE 200809L /* sigaction/clock_gettime under -std=c11 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cont.h"
#include "gc.h"
#include "loader.h"
#include "t.h"
#include "vm.h"
#include "wob_build.h"

static wo_vm VM;

/* One module per scenario: a Proc class (code, out, err) and one method
 * that runs `cmd argv...` and returns the record. Register layout inside
 * the method: r1 cmd, r2 argv multi, r3 Proc class id, result r0. */
static uint8_t *run_module(const char *cmd, const char **args, uint32_t nargs,
                           size_t *len) {
    wb_t *b = wb_new();
    uint32_t kproc = wb_const_text(b, "Proc");
    uint32_t kname = wb_const_text(b, "m");
    uint32_t kcmd = wb_const_text(b, cmd);
    uint32_t kargs[8];
    T_CHECK(nargs <= 8);
    for (uint32_t i = 0; i < nargs; i++) kargs[i] = wb_const_text(b, args[i]);
    uint8_t kinds[3] = {WO_K_SCALAR, WO_K_TEXT, WO_K_TEXT};
    uint32_t cls = wb_class(b, kproc, 0, kinds, 3);
    uint32_t kcls = wb_const_int(b, (int64_t)cls);

    uint32_t code[24];
    uint32_t n = 0;
    code[n++] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)kcmd);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 2, WO_K_TEXT, WO_B_MULTI_NEW);
    for (uint32_t i = 0; i < nargs; i++) {
        /* MULTI_PUSH: container at R[B], element at R[B+1] */
        code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kargs[i]);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 4, 2, WO_B_MULTI_PUSH);
    }
    code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kcls);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 0, 1, WO_B_PROC_RUN);
    code[n++] = wo_ins_abc(WOP_DROP, 2, 0, 0); /* the argv multi */
    code[n++] = wo_ins_abc(WOP_RET, 0, 0, 0);
    wb_method(b, kname, WOB_NONE, 0, 6, code, n, NULL, 0, NULL, 0);
    return wb_finish(b, len);
}

/* Run the scenario module; on rc 0 copy the record's fields out (code +
 * both texts) BEFORE the vm dies. out/err buffers are 64 KiB — the legs
 * here stay far under that. */
static int run_proc(const char *cmd, const char **args, uint32_t nargs,
                    int64_t *pcode, char *out, size_t outcap, char *errs,
                    size_t errcap, wo_err *err) {
    size_t len;
    uint8_t *img = run_module(cmd, args, nargs, &len);
    wo_module mod;
    char lerr[256];
    T_EQ(wo_load_buf(&mod, img, len, lerr, sizeof lerr), 0);
    T_EQ(wo_vm_init(&VM, &mod, 1 << 20), 0);
    uint64_t ret = 0;
    int rc = wo_vm_call(&VM, 0, NULL, 0, &ret, err);
    if (rc == 0) {
        wo_hdr *o = (wo_hdr *)(uintptr_t)ret;
        T_CHECK(o != NULL);
        uint64_t *fp = wo_fields(o);
        *pcode = (int64_t)fp[0];
        const wo_str *so = (const wo_str *)(uintptr_t)fp[1];
        const wo_str *se = (const wo_str *)(uintptr_t)fp[2];
        T_CHECK(so && se);
        T_CHECK(so->len < outcap && se->len < errcap);
        memcpy(out, so->data, so->len);
        out[so->len] = '\0';
        memcpy(errs, se->data, se->len);
        errs[se->len] = '\0';
        wo_drop_obj(&VM.rt, o);
    }
    wo_vm_destroy(&VM);
    wo_module_free(&mod);
    free(img);
    return rc;
}

/* Like run_proc but keeps only the exit code and output LENGTHS — for legs
 * whose output is too big to copy out. */
static int run_proc_lens(const char *cmd, const char **args, uint32_t nargs,
                         int64_t *pcode, size_t *outlen, size_t *errlen,
                         wo_err *err) {
    size_t len;
    uint8_t *img = run_module(cmd, args, nargs, &len);
    wo_module mod;
    char lerr[256];
    T_EQ(wo_load_buf(&mod, img, len, lerr, sizeof lerr), 0);
    T_EQ(wo_vm_init(&VM, &mod, 1 << 20), 0);
    uint64_t ret = 0;
    int rc = wo_vm_call(&VM, 0, NULL, 0, &ret, err);
    if (rc == 0) {
        wo_hdr *o = (wo_hdr *)(uintptr_t)ret;
        T_CHECK(o != NULL);
        uint64_t *fp = wo_fields(o);
        *pcode = (int64_t)fp[0];
        *outlen = ((const wo_str *)(uintptr_t)fp[1])->len;
        *errlen = ((const wo_str *)(uintptr_t)fp[2])->len;
        wo_drop_obj(&VM.rt, o);
    }
    wo_vm_destroy(&VM);
    wo_module_free(&mod);
    free(img);
    return rc;
}

static char OUTBUF[1 << 16], ERRBUF[1 << 16];

/* a child that exits 0 with known stdout */
static void test_echo(void) {
    const char *args[] = {"hi"};
    int64_t code = -99;
    wo_err err;
    T_EQ(run_proc("echo", args, 1, &code, OUTBUF, sizeof OUTBUF, ERRBUF,
                  sizeof ERRBUF, &err),
         0);
    T_EQ(code, 0);
    T_STREQ(OUTBUF, "hi\n");
    T_STREQ(ERRBUF, "");
}

/* a child that exits with a known nonzero code */
static void test_false(void) {
    int64_t code = -99;
    wo_err err;
    T_EQ(run_proc("false", NULL, 0, &code, OUTBUF, sizeof OUTBUF, ERRBUF,
                  sizeof ERRBUF, &err),
         0);
    T_EQ(code, 1);
    T_STREQ(OUTBUF, "");
}

/* a command that does not exist: exec fails in the child, code 127 */
static void test_missing(void) {
    int64_t code = -99;
    wo_err err;
    T_EQ(run_proc("wo-no-such-cmd", NULL, 0, &code, OUTBUF, sizeof OUTBUF,
                  ERRBUF, sizeof ERRBUF, &err),
         0);
    T_EQ(code, 127);
}

static int64_t mono_ms(void);

/* Module for the *_dl legs: method 0 = worker(m, tag) pushing tag five
 * times (test_fiber's pattern — proof the shard schedules while the main
 * fiber's child runs); method 1 = main running `cmd argv...` through
 * WO_B_PROC_RUN_DL with explicit bounds, after an optional pre-sleep.
 * Register layout in main: r1 cmd, r2 argv, r3 dl_ms, r4 out_cap,
 * r5 err_cap, r6 Proc class id, result r0. */
static uint8_t *run_dl_module(const char *cmd, const char **args,
                              uint32_t nargs, int64_t dl_ms, int64_t out_cap,
                              int64_t err_cap, int64_t presleep_ms,
                              size_t *len) {
    wb_t *b = wb_new();
    uint32_t kproc = wb_const_text(b, "Proc");
    uint32_t kw = wb_const_text(b, "worker");
    uint32_t km = wb_const_text(b, "main");
    uint32_t kcmd = wb_const_text(b, cmd);
    uint32_t kargs[8];
    T_CHECK(nargs <= 8);
    for (uint32_t i = 0; i < nargs; i++) kargs[i] = wb_const_text(b, args[i]);
    uint8_t kinds[3] = {WO_K_SCALAR, WO_K_TEXT, WO_K_TEXT};
    uint32_t cls = wb_class(b, kproc, 0, kinds, 3);
    uint32_t kcls = wb_const_int(b, (int64_t)cls);
    uint32_t kdl = wb_const_int(b, dl_ms);
    uint32_t koc = wb_const_int(b, out_cap);
    uint32_t kec = wb_const_int(b, err_cap);
    uint32_t k0 = wb_const_int(b, 0);
    uint32_t kK = wb_const_int(b, 5);
    uint32_t k1 = wb_const_int(b, 1);
    uint32_t kpre = wb_const_int(b, presleep_ms);

    { /* worker(m, tag): push tag 5 times, backward JMP yields */
        uint32_t code[9];
        code[0] = wo_ins_abx(WOP_LOADK, 2, (uint16_t)k0);
        code[1] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kK);
        code[2] = wo_ins_abc(WOP_LT, 4, 2, 3);
        code[3] = wo_ins_asbx(WOP_JZ, 4, 4);
        code[4] = wo_ins_abc(WOP_BUILTIN, 4, 0, WO_B_MULTI_PUSH);
        code[5] = wo_ins_abx(WOP_LOADK, 4, (uint16_t)k1);
        code[6] = wo_ins_abc(WOP_ADD, 2, 2, 4);
        code[7] = wo_ins_asbx(WOP_JMP, 0, -6);
        code[8] = wo_ins_abc(WOP_RET0, 0, 0, 0);
        wb_method(b, kw, WOB_NONE, 2, 8, code, 9, NULL, 0, NULL, 0);
    }
    { /* main */
        uint32_t code[24];
        uint32_t n = 0;
        if (presleep_ms > 0) {
            code[n++] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)kpre);
            code[n++] = wo_ins_abc(WOP_BUILTIN, 0, 1, WO_B_TIME_SLEEP);
        }
        code[n++] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)kcmd);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 2, WO_K_TEXT, WO_B_MULTI_NEW);
        for (uint32_t i = 0; i < nargs; i++) {
            code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kargs[i]);
            code[n++] = wo_ins_abc(WOP_BUILTIN, 4, 2, WO_B_MULTI_PUSH);
        }
        code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kdl);
        code[n++] = wo_ins_abx(WOP_LOADK, 4, (uint16_t)koc);
        code[n++] = wo_ins_abx(WOP_LOADK, 5, (uint16_t)kec);
        code[n++] = wo_ins_abx(WOP_LOADK, 6, (uint16_t)kcls);
        uint32_t run_pc = n;
        code[n++] = wo_ins_abc(WOP_BUILTIN, 0, 1, WO_B_PROC_RUN_DL);
        code[n++] = wo_ins_abc(WOP_DROP, 2, 0, 0);
        code[n++] = wo_ins_abc(WOP_RET, 0, 0, 0);
        /* a trapping run must still free the argv multi in r2 */
        wb_drop drops[] = {{.pc = run_pc, .owned = 1u << 2, .gc = 0}};
        wb_method(b, km, WOB_NONE, 0, 7, code, n, NULL, 0, drops, 1);
    }
    return wb_finish(b, len);
}

static int fd_count(void) {
    int n = 0;
    char p[64];
    for (int fd = 0; fd < 1024; fd++) {
        snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
        if (access(p, F_OK) == 0) n++;
    }
    return n;
}

/* deadline: a sleeping child against a 100 ms deadline — the trap names
 * the deadline, no child survives, no fd leaks. And the progress proof: a
 * worker fiber runs to completion while the main fiber is parked. */
static void test_deadline_kills_and_shard_schedules(void) {
    size_t len;
    uint8_t *img = run_dl_module("sleep", (const char *[]){"10"}, 1, 100,
                                 0, 0, 0, &len);
    wo_module mod;
    char lerr[256];
    T_EQ(wo_load_buf(&mod, img, len, lerr, sizeof lerr), 0);
    T_EQ(wo_vm_init(&VM, &mod, 1 << 20), 0);
    int fds0 = fd_count();
    wo_multi *m = wo_multi_new(&VM.rt, WO_K_SCALAR);
    T_CHECK(m != NULL);
    uint64_t wargs[2] = {(uint64_t)(uintptr_t)m, 7};
    T_CHECK(wo_vm_spawn_fiber(&VM, 0, wargs, 2) != NULL);
    int64_t t0 = mono_ms();
    uint64_t ret = 0;
    wo_err err;
    memset(&err, 0, sizeof err);
    T_EQ(wo_vm_call(&VM, 1, NULL, 0, &ret, &err), -1);
    int64_t elapsed = mono_ms() - t0;
    T_EQ(err.code, WO_T_IO);
    T_CHECK(strstr(err.msg, "deadline") != NULL);
    T_CHECK(elapsed < 3000); /* 100 ms deadline, not sleep's 10 s */
    T_EQ(m->len, 5u); /* the worker ran while main was parked */
    /* the child is gone: this test process has NO children left */
    int st;
    T_EQ(waitpid(-1, &st, WNOHANG), -1);
    T_EQ(errno, ECHILD);
    T_EQ(fd_count(), fds0); /* pipes, pidfd and epoll all closed */
    wo_drop_obj(&VM.rt, (wo_hdr *)m);
    wo_vm_destroy(&VM);
    wo_module_free(&mod);
    free(img);
}

/* output caps: a child writing past the stdout cap is killed and the trap
 * names the cap and its value */
static void test_cap_refuses_by_name(void) {
    size_t len;
    uint8_t *img = run_dl_module(
        "sh", (const char *[]){"-c", "head -c 5000 /dev/zero"}, 2, 5000,
        1000, 0, 0, &len);
    wo_module mod;
    char lerr[256];
    T_EQ(wo_load_buf(&mod, img, len, lerr, sizeof lerr), 0);
    T_EQ(wo_vm_init(&VM, &mod, 1 << 20), 0);
    int fds0 = fd_count();
    uint64_t ret = 0;
    wo_err err;
    memset(&err, 0, sizeof err);
    T_EQ(wo_vm_call(&VM, 1, NULL, 0, &ret, &err), -1);
    T_EQ(err.code, WO_T_IO);
    T_CHECK(strstr(err.msg, "stdout cap 1000") != NULL);
    int st;
    T_EQ(waitpid(-1, &st, WNOHANG), -1);
    T_EQ(errno, ECHILD);
    T_EQ(fd_count(), fds0);
    wo_vm_destroy(&VM);
    wo_module_free(&mod);
    free(img);
}

/* the stderr cap refuses by its own name */
static void test_errcap_refuses_by_name(void) {
    size_t len;
    uint8_t *img = run_dl_module(
        "sh", (const char *[]){"-c", "head -c 5000 /dev/zero >&2"}, 2, 5000,
        0, 1000, 0, &len);
    wo_module mod;
    char lerr[256];
    T_EQ(wo_load_buf(&mod, img, len, lerr, sizeof lerr), 0);
    T_EQ(wo_vm_init(&VM, &mod, 1 << 20), 0);
    uint64_t ret = 0;
    wo_err err;
    memset(&err, 0, sizeof err);
    T_EQ(wo_vm_call(&VM, 1, NULL, 0, &ret, &err), -1);
    T_EQ(err.code, WO_T_IO);
    T_CHECK(strstr(err.msg, "stderr cap 1000") != NULL);
    int st;
    T_EQ(waitpid(-1, &st, WNOHANG), -1);
    T_EQ(errno, ECHILD);
    wo_vm_destroy(&VM);
    wo_module_free(&mod);
    free(img);
}

/* Module for the ceiling/unwind legs: method 0 = runner (arity 0) that
 * runs `sleep <secs>` through RUN_DL and drops the record; method 1 =
 * main that pre-sleeps, then either does the same run (ceiling) or just
 * returns (unwind). */
static uint8_t *sleeper_module(const char *secs, int64_t presleep_ms,
                               int main_runs_too, size_t *len) {
    wb_t *b = wb_new();
    uint32_t kproc = wb_const_text(b, "Proc");
    uint32_t kw = wb_const_text(b, "runner");
    uint32_t km = wb_const_text(b, "main");
    uint32_t kcmd = wb_const_text(b, "sleep");
    uint32_t karg = wb_const_text(b, secs);
    uint8_t kinds[3] = {WO_K_SCALAR, WO_K_TEXT, WO_K_TEXT};
    uint32_t cls = wb_class(b, kproc, 0, kinds, 3);
    uint32_t kcls = wb_const_int(b, (int64_t)cls);
    uint32_t kdl = wb_const_int(b, 30000);
    uint32_t kz = wb_const_int(b, 0);
    uint32_t kpre = wb_const_int(b, presleep_ms);

    { /* runner: run `sleep secs`, drop record, return */
        uint32_t code[16];
        uint32_t n = 0;
        code[n++] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)kcmd);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 2, WO_K_TEXT, WO_B_MULTI_NEW);
        code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)karg);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 4, 2, WO_B_MULTI_PUSH);
        code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kdl);
        code[n++] = wo_ins_abx(WOP_LOADK, 4, (uint16_t)kz);
        code[n++] = wo_ins_abx(WOP_LOADK, 5, (uint16_t)kz);
        code[n++] = wo_ins_abx(WOP_LOADK, 6, (uint16_t)kcls);
        uint32_t run_pc = n;
        code[n++] = wo_ins_abc(WOP_BUILTIN, 0, 1, WO_B_PROC_RUN_DL);
        code[n++] = wo_ins_abc(WOP_DROP, 2, 0, 0);
        code[n++] = wo_ins_abc(WOP_DROP, 0, 0, 0);
        code[n++] = wo_ins_abc(WOP_RET0, 0, 0, 0);
        wb_drop drops[] = {{.pc = run_pc, .owned = 1u << 2, .gc = 0}};
        wb_method(b, kw, WOB_NONE, 0, 7, code, n, NULL, 0, drops, 1);
    }
    { /* main: presleep, then optionally the same run */
        uint32_t code[16];
        uint32_t n = 0;
        code[n++] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)kpre);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 0, 1, WO_B_TIME_SLEEP);
        if (main_runs_too) {
            code[n++] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)kcmd);
            code[n++] = wo_ins_abc(WOP_BUILTIN, 2, WO_K_TEXT, WO_B_MULTI_NEW);
            code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)karg);
            code[n++] = wo_ins_abc(WOP_BUILTIN, 4, 2, WO_B_MULTI_PUSH);
            code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kdl);
            code[n++] = wo_ins_abx(WOP_LOADK, 4, (uint16_t)kz);
            code[n++] = wo_ins_abx(WOP_LOADK, 5, (uint16_t)kz);
            code[n++] = wo_ins_abx(WOP_LOADK, 6, (uint16_t)kcls);
            uint32_t run_pc = n;
            code[n++] = wo_ins_abc(WOP_BUILTIN, 0, 1, WO_B_PROC_RUN_DL);
            code[n++] = wo_ins_abc(WOP_DROP, 2, 0, 0);
            code[n++] = wo_ins_abc(WOP_DROP, 0, 0, 0);
            code[n++] = wo_ins_abc(WOP_RET0, 0, 0, 0);
            wb_drop drops[] = {{.pc = run_pc, .owned = 1u << 2, .gc = 0}};
            wb_method(b, km, WOB_NONE, 0, 7, code, n, NULL, 0, drops, 1);
        } else {
            code[n++] = wo_ins_abc(WOP_RET0, 0, 0, 0);
            wb_method(b, km, WOB_NONE, 0, 3, code, n, NULL, 0, NULL, 0);
        }
    }
    return wb_finish(b, len);
}

/* the ceiling: 32 fibers each hold a live child; the 33rd spawn (main's)
 * fails closed naming the ceiling, and the 32 are unharmed until reaped */
static void test_ceiling_fails_closed(void) {
    size_t len;
    uint8_t *img = sleeper_module("2", 400, 1, &len);
    wo_module mod;
    char lerr[256];
    T_EQ(wo_load_buf(&mod, img, len, lerr, sizeof lerr), 0);
    T_EQ(wo_vm_init(&VM, &mod, 1 << 20), 0);
    for (int i = 0; i < 32; i++)
        T_CHECK(wo_vm_spawn_fiber(&VM, 0, NULL, 0) != NULL);
    uint64_t ret = 0;
    wo_err err;
    memset(&err, 0, sizeof err);
    T_EQ(wo_vm_call(&VM, 1, NULL, 0, &ret, &err), -1);
    T_EQ(err.code, WO_T_IO);
    T_CHECK(strstr(err.msg, "ceiling") != NULL);
    wo_vm_destroy(&VM); /* sweeps the 32 sleepers */
    int st;
    T_EQ(waitpid(-1, &st, WNOHANG), -1);
    T_EQ(errno, ECHILD);
    wo_module_free(&mod);
    free(img);
}

/* a fiber parked on a live child is reaped when main returns — its child
 * dies with it (fib_reap -> wo_proc_abandon) */
static void test_unwind_reaps_child(void) {
    size_t len;
    uint8_t *img = sleeper_module("10", 300, 0, &len);
    wo_module mod;
    char lerr[256];
    T_EQ(wo_load_buf(&mod, img, len, lerr, sizeof lerr), 0);
    T_EQ(wo_vm_init(&VM, &mod, 1 << 20), 0);
    T_CHECK(wo_vm_spawn_fiber(&VM, 0, NULL, 0) != NULL);
    uint64_t ret = 0;
    wo_err err;
    memset(&err, 0, sizeof err);
    T_EQ(wo_vm_call(&VM, 1, NULL, 0, &ret, &err), 0); /* main returns */
    T_EQ(VM.nchildren, 0u); /* the reap already killed the sleeper */
    wo_vm_destroy(&VM);
    int st;
    T_EQ(waitpid(-1, &st, WNOHANG), -1);
    T_EQ(errno, ECHILD);
    wo_module_free(&mod);
    free(img);
}

/* one thousand sequential children leave the fd table flat and every
 * slot released (the iteration 24 measurement style) */
static uint8_t *churn_module(size_t *len) {
    wb_t *b = wb_new();
    uint32_t kproc = wb_const_text(b, "Proc");
    uint32_t km = wb_const_text(b, "main");
    uint32_t kcmd = wb_const_text(b, "true");
    uint8_t kinds[3] = {WO_K_SCALAR, WO_K_TEXT, WO_K_TEXT};
    uint32_t cls = wb_class(b, kproc, 0, kinds, 3);
    uint32_t kcls = wb_const_int(b, (int64_t)cls);
    uint32_t kdl = wb_const_int(b, 30000);
    uint32_t kz = wb_const_int(b, 0);
    uint32_t klim = wb_const_int(b, 1000);
    uint32_t k1 = wb_const_int(b, 1);
    uint32_t code[20];
    uint32_t n = 0;
    code[n++] = wo_ins_abx(WOP_LOADK, 7, (uint16_t)kz);   /* i = 0 */
    code[n++] = wo_ins_abx(WOP_LOADK, 8, (uint16_t)klim); /* limit */
    uint32_t loop_pc = n;
    code[n++] = wo_ins_abc(WOP_LT, 9, 7, 8);
    uint32_t jz_pc = n;
    code[n++] = wo_ins_asbx(WOP_JZ, 9, 0); /* patched below */
    code[n++] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)kcmd);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 2, WO_K_TEXT, WO_B_MULTI_NEW);
    code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kdl);
    code[n++] = wo_ins_abx(WOP_LOADK, 4, (uint16_t)kz);
    code[n++] = wo_ins_abx(WOP_LOADK, 5, (uint16_t)kz);
    code[n++] = wo_ins_abx(WOP_LOADK, 6, (uint16_t)kcls);
    uint32_t run_pc = n;
    code[n++] = wo_ins_abc(WOP_BUILTIN, 0, 1, WO_B_PROC_RUN_DL);
    code[n++] = wo_ins_abc(WOP_DROP, 2, 0, 0);
    code[n++] = wo_ins_abc(WOP_DROP, 0, 0, 0);
    code[n++] = wo_ins_abx(WOP_LOADK, 9, (uint16_t)k1);
    code[n++] = wo_ins_abc(WOP_ADD, 7, 7, 9);
    uint32_t jmp_pc = n;
    code[n++] = wo_ins_asbx(WOP_JMP, 0, (int)loop_pc - ((int)jmp_pc + 1));
    uint32_t exit_pc = n;
    code[n++] = wo_ins_abc(WOP_RET0, 0, 0, 0);
    code[jz_pc] = wo_ins_asbx(WOP_JZ, 9, (int)exit_pc - ((int)jz_pc + 1));
    wb_drop drops[] = {{.pc = run_pc, .owned = 1u << 2, .gc = 0}};
    wb_method(b, km, WOB_NONE, 0, 10, code, n, NULL, 0, drops, 1);
    return wb_finish(b, len);
}

static void test_thousand_spawns_fd_flat(void) {
    size_t len;
    uint8_t *img = churn_module(&len);
    wo_module mod;
    char lerr[256];
    T_EQ(wo_load_buf(&mod, img, len, lerr, sizeof lerr), 0);
    T_EQ(wo_vm_init(&VM, &mod, 1 << 20), 0);
    int fds0 = fd_count();
    uint64_t ret = 0;
    wo_err err;
    memset(&err, 0, sizeof err);
    T_EQ(wo_vm_call(&VM, 0, NULL, 0, &ret, &err), 0);
    T_EQ(fd_count(), fds0);
    T_EQ(VM.nchildren, 0u);
    wo_vm_destroy(&VM);
    int st;
    T_EQ(waitpid(-1, &st, WNOHANG), -1);
    T_EQ(errno, ECHILD);
    wo_module_free(&mod);
    free(img);
}

/* SIGTERM while a child lives: the run answers STOPPED (rc 1) and no
 * child survives. A helper process delivers the signal 200 ms in; the
 * handlers must be installed first (env.stopping does that in real
 * programs — the test installs the same ones via a first stopping call
 * from bytecode is overkill; raise() through a helper matches how the
 * process is actually told to stop). */
static uint8_t *stop_module(size_t *len) {
    wb_t *b = wb_new();
    uint32_t kproc = wb_const_text(b, "Proc");
    uint32_t km = wb_const_text(b, "main");
    uint32_t kcmd = wb_const_text(b, "sleep");
    uint32_t karg = wb_const_text(b, "10");
    uint8_t kinds[3] = {WO_K_SCALAR, WO_K_TEXT, WO_K_TEXT};
    uint32_t cls = wb_class(b, kproc, 0, kinds, 3);
    uint32_t kcls = wb_const_int(b, (int64_t)cls);
    uint32_t kdl = wb_const_int(b, 30000);
    uint32_t kz = wb_const_int(b, 0);
    uint32_t code[16];
    uint32_t n = 0;
    /* env.stopping first: installs the SIGTERM handler exactly as a real
     * program's drain loop would have */
    code[n++] = wo_ins_abc(WOP_BUILTIN, 0, 0, WO_B_ENV_STOPPING);
    code[n++] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)kcmd);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 2, WO_K_TEXT, WO_B_MULTI_NEW);
    code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)karg);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 4, 2, WO_B_MULTI_PUSH);
    code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kdl);
    code[n++] = wo_ins_abx(WOP_LOADK, 4, (uint16_t)kz);
    code[n++] = wo_ins_abx(WOP_LOADK, 5, (uint16_t)kz);
    code[n++] = wo_ins_abx(WOP_LOADK, 6, (uint16_t)kcls);
    uint32_t run_pc = n;
    code[n++] = wo_ins_abc(WOP_BUILTIN, 0, 1, WO_B_PROC_RUN_DL);
    code[n++] = wo_ins_abc(WOP_DROP, 2, 0, 0);
    code[n++] = wo_ins_abc(WOP_DROP, 0, 0, 0);
    code[n++] = wo_ins_abc(WOP_RET0, 0, 0, 0);
    wb_drop drops[] = {{.pc = run_pc, .owned = 1u << 2, .gc = 0}};
    wb_method(b, km, WOB_NONE, 0, 7, code, n, NULL, 0, drops, 1);
    return wb_finish(b, len);
}

static void test_stop_kills_child(void) {
    size_t len;
    uint8_t *img = stop_module(&len);
    wo_module mod;
    char lerr[256];
    T_EQ(wo_load_buf(&mod, img, len, lerr, sizeof lerr), 0);
    T_EQ(wo_vm_init(&VM, &mod, 1 << 20), 0);
    pid_t helper = fork();
    if (helper == 0) { /* deliver SIGTERM 200 ms in, then vanish */
        struct timespec ts = {0, 200 * 1000000};
        nanosleep(&ts, NULL);
        kill(getppid(), SIGTERM);
        _exit(0);
    }
    T_CHECK(helper > 0);
    int64_t t0 = mono_ms();
    uint64_t ret = 0;
    wo_err err;
    memset(&err, 0, sizeof err);
    int rc = wo_vm_call(&VM, 0, NULL, 0, &ret, &err);
    int64_t elapsed = mono_ms() - t0;
    T_EQ(rc, 1); /* STOPPED, not a trap */
    T_CHECK(elapsed < 5000); /* nowhere near sleep 10 */
    T_EQ(VM.nchildren, 0u);
    int st;
    T_EQ(waitpid(helper, &st, 0), helper); /* reap the helper itself */
    T_EQ(waitpid(-1, &st, WNOHANG), -1);   /* and NOTHING else is left */
    T_EQ(errno, ECHILD);
    wo_vm_destroy(&VM);
    wo_module_free(&mod);
    free(img);
}

/* ---- runtime-v2 1: the streaming child ---------------------------------
 * Child record class (id, stdin, stdout, stderr — all scalar) is built
 * into each module; the fds are driven by the NET verbs, which is the
 * whole design. */

/* echo module: spawn `cat`, write "hi\n" to Child.stdin, read it back
 * from Child.stdout, close all three fds, return the text. */
static uint8_t *stream_echo_module(size_t *len) {
    wb_t *b = wb_new();
    uint32_t kchild = wb_const_text(b, "Child");
    uint32_t km = wb_const_text(b, "main");
    uint32_t kcmd = wb_const_text(b, "cat");
    uint32_t khi = wb_const_text(b, "hi\n");
    uint8_t kinds[4] = {WO_K_SCALAR, WO_K_SCALAR, WO_K_SCALAR, WO_K_SCALAR};
    uint32_t cls = wb_class(b, kchild, 0, kinds, 4);
    uint32_t kcls = wb_const_int(b, (int64_t)cls);
    uint32_t kms = wb_const_int(b, 3000);
    uint32_t kmax = wb_const_int(b, 16);
    uint32_t code[32];
    uint32_t n = 0;
    code[n++] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)kcmd);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 2, WO_K_TEXT, WO_B_MULTI_NEW);
    code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kcls);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 0, 1, WO_B_PROC_SPAWN);
    code[n++] = wo_ins_abc(WOP_DROP, 2, 0, 0); /* argv multi */
    code[n++] = wo_ins_abc(WOP_GETF, 1, 0, 1); /* stdin */
    code[n++] = wo_ins_abc(WOP_GETF, 2, 0, 2); /* stdout */
    code[n++] = wo_ins_abc(WOP_GETF, 3, 0, 3); /* stderr */
    /* write "hi\n" with a deadline */
    code[n++] = wo_ins_abc(WOP_MOVE, 4, 1, 0);
    code[n++] = wo_ins_abx(WOP_LOADK, 5, (uint16_t)khi);
    code[n++] = wo_ins_abx(WOP_LOADK, 6, (uint16_t)kms);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 7, 4, WO_B_NET_WRITE_DL);
    /* read it back */
    code[n++] = wo_ins_abc(WOP_MOVE, 4, 2, 0);
    code[n++] = wo_ins_abx(WOP_LOADK, 5, (uint16_t)kmax);
    code[n++] = wo_ins_abx(WOP_LOADK, 6, (uint16_t)kms);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 7, 4, WO_B_NET_READ_DL);
    /* close the caller-owned fds */
    code[n++] = wo_ins_abc(WOP_BUILTIN, 8, 1, WO_B_NET_CLOSE);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 8, 2, WO_B_NET_CLOSE);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 8, 3, WO_B_NET_CLOSE);
    code[n++] = wo_ins_abc(WOP_DROP, 0, 0, 0); /* the Child record */
    code[n++] = wo_ins_abc(WOP_RET, 7, 0, 0);
    wb_method(b, km, WOB_NONE, 0, 9, code, n, NULL, 0, NULL, 0);
    return wb_finish(b, len);
}

static void test_stream_echo(void) {
    size_t len;
    uint8_t *img = stream_echo_module(&len);
    wo_module mod;
    char lerr[256];
    T_EQ(wo_load_buf(&mod, img, len, lerr, sizeof lerr), 0);
    T_EQ(wo_vm_init(&VM, &mod, 1 << 20), 0);
    uint64_t ret = 0;
    wo_err err;
    memset(&err, 0, sizeof err);
    T_EQ(wo_vm_call(&VM, 0, NULL, 0, &ret, &err), 0);
    const wo_str *s = (const wo_str *)(uintptr_t)ret;
    T_CHECK(s != NULL);
    if (s) {
        T_EQ(s->len, 3u);
        T_CHECK(memcmp(s->data, "hi\n", 3) == 0);
        wo_str_free(&VM.rt, (wo_str *)s);
    }
    wo_vm_destroy(&VM); /* cat (EOF'd) reaped here if still live */
    int st;
    T_EQ(waitpid(-1, &st, WNOHANG), -1);
    T_EQ(errno, ECHILD);
    wo_module_free(&mod);
    free(img);
}

/* wait module: spawn `cmd arg`, close the fds, then EITHER one wait
 * (ms1) returning its result, OR wait(ms1) -> signal(sig) -> wait(ms2)
 * returning the second result. */
static uint8_t *stream_wait_module(const char *cmd, const char *arg,
                                   int64_t ms1, int64_t sig, int64_t ms2,
                                   size_t *len) {
    wb_t *b = wb_new();
    uint32_t kchild = wb_const_text(b, "Child");
    uint32_t km = wb_const_text(b, "main");
    uint32_t kcmd = wb_const_text(b, cmd);
    uint32_t karg = arg ? wb_const_text(b, arg) : 0;
    uint8_t kinds[4] = {WO_K_SCALAR, WO_K_SCALAR, WO_K_SCALAR, WO_K_SCALAR};
    uint32_t cls = wb_class(b, kchild, 0, kinds, 4);
    uint32_t kcls = wb_const_int(b, (int64_t)cls);
    uint32_t kms1 = wb_const_int(b, ms1);
    uint32_t ksig = wb_const_int(b, sig);
    uint32_t kms2 = wb_const_int(b, ms2);
    uint32_t code[40];
    uint32_t n = 0;
    code[n++] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)kcmd);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 2, WO_K_TEXT, WO_B_MULTI_NEW);
    if (arg) {
        code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)karg);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 4, 2, WO_B_MULTI_PUSH);
    }
    code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kcls);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 0, 1, WO_B_PROC_SPAWN);
    code[n++] = wo_ins_abc(WOP_DROP, 2, 0, 0);
    code[n++] = wo_ins_abc(WOP_GETF, 1, 0, 1);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 8, 1, WO_B_NET_CLOSE);
    code[n++] = wo_ins_abc(WOP_GETF, 1, 0, 2);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 8, 1, WO_B_NET_CLOSE);
    code[n++] = wo_ins_abc(WOP_GETF, 1, 0, 3);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 8, 1, WO_B_NET_CLOSE);
    code[n++] = wo_ins_abc(WOP_GETF, 4, 0, 0); /* id stays in r4 */
    code[n++] = wo_ins_abx(WOP_LOADK, 5, (uint16_t)kms1);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 7, 4, WO_B_PROC_WAIT_DL);
    if (sig > 0) {
        code[n++] = wo_ins_abx(WOP_LOADK, 5, (uint16_t)ksig);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 8, 4, WO_B_PROC_SIGNAL);
        code[n++] = wo_ins_abx(WOP_LOADK, 5, (uint16_t)kms2);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 7, 4, WO_B_PROC_WAIT_DL);
    }
    code[n++] = wo_ins_abc(WOP_DROP, 0, 0, 0);
    code[n++] = wo_ins_abc(WOP_RET, 7, 0, 0);
    wb_method(b, km, WOB_NONE, 0, 9, code, n, NULL, 0, NULL, 0);
    return wb_finish(b, len);
}

static int64_t run_stream_wait(const char *cmd, const char *arg, int64_t ms1,
                               int64_t sig, int64_t ms2, int expect_rc) {
    size_t len;
    uint8_t *img = stream_wait_module(cmd, arg, ms1, sig, ms2, &len);
    wo_module mod;
    char lerr[256];
    T_EQ(wo_load_buf(&mod, img, len, lerr, sizeof lerr), 0);
    T_EQ(wo_vm_init(&VM, &mod, 1 << 20), 0);
    uint64_t ret = 0;
    wo_err err;
    memset(&err, 0, sizeof err);
    T_EQ(wo_vm_call(&VM, 0, NULL, 0, &ret, &err), expect_rc);
    wo_vm_destroy(&VM);
    int st;
    T_EQ(waitpid(-1, &st, WNOHANG), -1);
    T_EQ(errno, ECHILD);
    wo_module_free(&mod);
    free(img);
    return (int64_t)ret;
}

static void test_stream_wait(void) {
    /* a fast child answers its code */
    T_EQ(run_stream_wait("true", NULL, 3000, 0, 0, 0), 0);
    /* a slow child answers nil at the deadline (and is untouched, then
     * swept by destroy — ECHILD proves the sweep) */
    T_EQ((uint64_t)run_stream_wait("sleep", "10", 100, 0, 0, 0),
         WO_NIL_SCALAR);
    /* signal SIGKILL, then the wait observes the signal death (-1) */
    T_EQ(run_stream_wait("sleep", "10", 100, 9, 3000, 0), -1);
}

/* double-wait: a fiber parks as the waiter; main tries second, refused */
static uint8_t *double_wait_module(size_t *len) {
    wb_t *b = wb_new();
    uint32_t kchild = wb_const_text(b, "Child");
    uint32_t kspawn = wb_const_text(b, "spawner");
    uint32_t kw = wb_const_text(b, "waiter");
    uint32_t ksec = wb_const_text(b, "second");
    uint32_t kcmd = wb_const_text(b, "sleep");
    uint32_t karg = wb_const_text(b, "1");
    uint8_t kinds[4] = {WO_K_SCALAR, WO_K_SCALAR, WO_K_SCALAR, WO_K_SCALAR};
    uint32_t cls = wb_class(b, kchild, 0, kinds, 4);
    uint32_t kcls = wb_const_int(b, (int64_t)cls);
    uint32_t k3000 = wb_const_int(b, 3000);
    uint32_t k200 = wb_const_int(b, 200);
    uint32_t k100 = wb_const_int(b, 100);
    { /* spawner (method 0): spawn sleep 1, close fds, RET id */
        uint32_t code[24];
        uint32_t n = 0;
        code[n++] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)kcmd);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 2, WO_K_TEXT, WO_B_MULTI_NEW);
        code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)karg);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 4, 2, WO_B_MULTI_PUSH);
        code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kcls);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 0, 1, WO_B_PROC_SPAWN);
        code[n++] = wo_ins_abc(WOP_DROP, 2, 0, 0);
        code[n++] = wo_ins_abc(WOP_GETF, 1, 0, 1);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 8, 1, WO_B_NET_CLOSE);
        code[n++] = wo_ins_abc(WOP_GETF, 1, 0, 2);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 8, 1, WO_B_NET_CLOSE);
        code[n++] = wo_ins_abc(WOP_GETF, 1, 0, 3);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 8, 1, WO_B_NET_CLOSE);
        code[n++] = wo_ins_abc(WOP_GETF, 4, 0, 0);
        code[n++] = wo_ins_abc(WOP_DROP, 0, 0, 0);
        code[n++] = wo_ins_abc(WOP_RET, 4, 0, 0);
        wb_method(b, kspawn, WOB_NONE, 0, 9, code, n, NULL, 0, NULL, 0);
    }
    { /* waiter (method 1, argc 1: r0 = id): wait 3000, RET0 */
        uint32_t code[4];
        code[0] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)k3000);
        code[1] = wo_ins_abc(WOP_BUILTIN, 2, 0, WO_B_PROC_WAIT_DL);
        code[2] = wo_ins_abc(WOP_RET0, 0, 0, 0);
        wb_method(b, kw, WOB_NONE, 1, 3, code, 3, NULL, 0, NULL, 0);
    }
    { /* second (method 2, argc 1): sleep 200 so the fiber parks first,
       * then wait 100 — must trap "already has a waiter" */
        uint32_t code[6];
        code[0] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)k200);
        code[1] = wo_ins_abc(WOP_BUILTIN, 2, 1, WO_B_TIME_SLEEP);
        code[2] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)k100);
        code[3] = wo_ins_abc(WOP_BUILTIN, 2, 0, WO_B_PROC_WAIT_DL);
        code[4] = wo_ins_abc(WOP_RET0, 0, 0, 0);
        wb_method(b, ksec, WOB_NONE, 1, 3, code, 5, NULL, 0, NULL, 0);
    }
    return wb_finish(b, len);
}

static void test_stream_one_waiter(void) {
    size_t len;
    uint8_t *img = double_wait_module(&len);
    wo_module mod;
    char lerr[256];
    T_EQ(wo_load_buf(&mod, img, len, lerr, sizeof lerr), 0);
    T_EQ(wo_vm_init(&VM, &mod, 1 << 20), 0);
    uint64_t id = 0;
    wo_err err;
    memset(&err, 0, sizeof err);
    T_EQ(wo_vm_call(&VM, 0, NULL, 0, &id, &err), 0); /* spawner */
    uint64_t warg[1] = {id};
    T_CHECK(wo_vm_spawn_fiber(&VM, 1, warg, 1) != NULL); /* waiter */
    uint64_t ret = 0;
    T_EQ(wo_vm_call(&VM, 2, warg, 1, &ret, &err), -1); /* second */
    T_EQ(err.code, WO_T_IO);
    T_CHECK(strstr(err.msg, "waiter") != NULL);
    wo_vm_destroy(&VM);
    int st;
    T_EQ(waitpid(-1, &st, WNOHANG), -1);
    T_EQ(errno, ECHILD);
    wo_module_free(&mod);
    free(img);
}

/* churn: 200 spawn/wait/close rounds leave the fd table flat */
static uint8_t *stream_churn_module(size_t *len) {
    wb_t *b = wb_new();
    uint32_t kchild = wb_const_text(b, "Child");
    uint32_t km = wb_const_text(b, "main");
    uint32_t kcmd = wb_const_text(b, "true");
    uint8_t kinds[4] = {WO_K_SCALAR, WO_K_SCALAR, WO_K_SCALAR, WO_K_SCALAR};
    uint32_t cls = wb_class(b, kchild, 0, kinds, 4);
    uint32_t kcls = wb_const_int(b, (int64_t)cls);
    uint32_t k5000 = wb_const_int(b, 5000);
    uint32_t kz = wb_const_int(b, 0);
    uint32_t klim = wb_const_int(b, 200);
    uint32_t k1 = wb_const_int(b, 1);
    uint32_t code[40];
    uint32_t n = 0;
    code[n++] = wo_ins_abx(WOP_LOADK, 10, (uint16_t)kz);
    code[n++] = wo_ins_abx(WOP_LOADK, 11, (uint16_t)klim);
    uint32_t loop_pc = n;
    code[n++] = wo_ins_abc(WOP_LT, 12, 10, 11);
    uint32_t jz_pc = n;
    code[n++] = wo_ins_asbx(WOP_JZ, 12, 0); /* patched */
    code[n++] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)kcmd);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 2, WO_K_TEXT, WO_B_MULTI_NEW);
    code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kcls);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 0, 1, WO_B_PROC_SPAWN);
    code[n++] = wo_ins_abc(WOP_DROP, 2, 0, 0);
    code[n++] = wo_ins_abc(WOP_GETF, 1, 0, 1);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 8, 1, WO_B_NET_CLOSE);
    code[n++] = wo_ins_abc(WOP_GETF, 1, 0, 2);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 8, 1, WO_B_NET_CLOSE);
    code[n++] = wo_ins_abc(WOP_GETF, 1, 0, 3);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 8, 1, WO_B_NET_CLOSE);
    code[n++] = wo_ins_abc(WOP_GETF, 4, 0, 0);
    code[n++] = wo_ins_abx(WOP_LOADK, 5, (uint16_t)k5000);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 7, 4, WO_B_PROC_WAIT_DL);
    code[n++] = wo_ins_abc(WOP_DROP, 0, 0, 0);
    code[n++] = wo_ins_abx(WOP_LOADK, 12, (uint16_t)k1);
    code[n++] = wo_ins_abc(WOP_ADD, 10, 10, 12);
    uint32_t jmp_pc = n;
    code[n++] = wo_ins_asbx(WOP_JMP, 0, (int)loop_pc - ((int)jmp_pc + 1));
    uint32_t exit_pc = n;
    code[n++] = wo_ins_abc(WOP_RET0, 0, 0, 0);
    code[jz_pc] = wo_ins_asbx(WOP_JZ, 12, (int)exit_pc - ((int)jz_pc + 1));
    wb_method(b, km, WOB_NONE, 0, 13, code, n, NULL, 0, NULL, 0);
    return wb_finish(b, len);
}

static void test_stream_churn(void) {
    size_t len;
    uint8_t *img = stream_churn_module(&len);
    wo_module mod;
    char lerr[256];
    T_EQ(wo_load_buf(&mod, img, len, lerr, sizeof lerr), 0);
    T_EQ(wo_vm_init(&VM, &mod, 1 << 20), 0);
    int fds0 = fd_count();
    uint64_t ret = 0;
    wo_err err;
    memset(&err, 0, sizeof err);
    T_EQ(wo_vm_call(&VM, 0, NULL, 0, &ret, &err), 0);
    T_EQ(fd_count(), fds0);
    T_EQ(VM.nchildren, 0u);
    wo_vm_destroy(&VM);
    wo_module_free(&mod);
    free(img);
}

/* ---- runtime-v2 2: the PTY child ----------------------------------------
 * pty module: spawn_pty `sh -c <script>` at cols x rows; optionally sleep
 * then resize; read once from the master; close; return the text. */
static uint8_t *pty_module(const char *script, int64_t cols, int64_t rows,
                           int do_resize, int64_t rcols, int64_t rrows,
                           size_t *len) {
    wb_t *b = wb_new();
    uint32_t kchild = wb_const_text(b, "Child");
    uint32_t km = wb_const_text(b, "main");
    uint32_t kcmd = wb_const_text(b, "sh");
    uint32_t kdc = wb_const_text(b, "-c");
    uint32_t kscript = wb_const_text(b, script);
    uint8_t kinds[4] = {WO_K_SCALAR, WO_K_SCALAR, WO_K_SCALAR, WO_K_SCALAR};
    uint32_t cls = wb_class(b, kchild, 0, kinds, 4);
    uint32_t kcls = wb_const_int(b, (int64_t)cls);
    uint32_t kcols = wb_const_int(b, cols);
    uint32_t krows = wb_const_int(b, rows);
    uint32_t krc = wb_const_int(b, rcols);
    uint32_t krr = wb_const_int(b, rrows);
    uint32_t k100 = wb_const_int(b, 100);
    uint32_t kms = wb_const_int(b, 4000);
    uint32_t kmax = wb_const_int(b, 64);
    uint32_t code[40];
    uint32_t n = 0;
    code[n++] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)kcmd);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 2, WO_K_TEXT, WO_B_MULTI_NEW);
    code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kdc);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 6, 2, WO_B_MULTI_PUSH);
    code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kscript);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 6, 2, WO_B_MULTI_PUSH);
    code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kcols);
    code[n++] = wo_ins_abx(WOP_LOADK, 4, (uint16_t)krows);
    code[n++] = wo_ins_abx(WOP_LOADK, 5, (uint16_t)kcls);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 0, 1, WO_B_PROC_SPAWN_PTY);
    code[n++] = wo_ins_abc(WOP_DROP, 2, 0, 0);
    code[n++] = wo_ins_abc(WOP_GETF, 9, 0, 1); /* the master */
    code[n++] = wo_ins_abc(WOP_GETF, 4, 0, 0); /* the id */
    if (do_resize) { /* let the child start, then resize */
        code[n++] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)k100);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 8, 1, WO_B_TIME_SLEEP);
        code[n++] = wo_ins_abx(WOP_LOADK, 5, (uint16_t)krc);
        code[n++] = wo_ins_abx(WOP_LOADK, 6, (uint16_t)krr);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 8, 4, WO_B_PROC_RESIZE);
    }
    code[n++] = wo_ins_abc(WOP_MOVE, 4, 9, 0);
    code[n++] = wo_ins_abx(WOP_LOADK, 5, (uint16_t)kmax);
    code[n++] = wo_ins_abx(WOP_LOADK, 6, (uint16_t)kms);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 7, 4, WO_B_NET_READ_DL);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 8, 9, WO_B_NET_CLOSE);
    code[n++] = wo_ins_abc(WOP_DROP, 0, 0, 0);
    code[n++] = wo_ins_abc(WOP_RET, 7, 0, 0);
    wb_method(b, km, WOB_NONE, 0, 10, code, n, NULL, 0, NULL, 0);
    return wb_finish(b, len);
}

/* run a pty module, copy the returned text into out */
static void run_pty(const char *script, int64_t cols, int64_t rows,
                    int do_resize, int64_t rcols, int64_t rrows, char *out,
                    size_t outcap) {
    size_t len;
    uint8_t *img = pty_module(script, cols, rows, do_resize, rcols, rrows, &len);
    wo_module mod;
    char lerr[256];
    T_EQ(wo_load_buf(&mod, img, len, lerr, sizeof lerr), 0);
    T_EQ(wo_vm_init(&VM, &mod, 1 << 20), 0);
    uint64_t ret = 0;
    wo_err err;
    memset(&err, 0, sizeof err);
    T_EQ(wo_vm_call(&VM, 0, NULL, 0, &ret, &err), 0);
    out[0] = '\0';
    const wo_str *s = (const wo_str *)(uintptr_t)ret;
    if (ret != 0 && ret != WO_NIL_SCALAR && s) {
        size_t c = s->len < outcap - 1 ? s->len : outcap - 1;
        memcpy(out, s->data, c);
        out[c] = '\0';
        wo_str_free(&VM.rt, (wo_str *)s);
    }
    wo_vm_destroy(&VM);
    int st;
    T_EQ(waitpid(-1, &st, WNOHANG), -1);
    T_EQ(errno, ECHILD);
    wo_module_free(&mod);
    free(img);
}

static void test_pty_is_a_tty(void) {
    char out[128];
    run_pty("test -t 0 && test -t 1 && echo yes-tty", 24, 80, 0, 0, 0, out,
            sizeof out);
    T_CHECK(strncmp(out, "yes-tty", 7) == 0); /* discipline adds \r\n */
}

static void test_pty_size_and_resize(void) {
    char out[128];
    /* the initial size is what spawn_pty stated */
    run_pty("stty size", 80, 24, 0, 0, 0, out, sizeof out);
    T_CHECK(strncmp(out, "24 80", 5) == 0);
    /* a resize during the child's sleep is what stty then reports */
    run_pty("sleep 0.3; stty size", 80, 24, 1, 120, 40, out, sizeof out);
    T_CHECK(strncmp(out, "40 120", 6) == 0);
}

/* resize on a pipe child refuses by name */
static uint8_t *pipe_resize_module(size_t *len) {
    wb_t *b = wb_new();
    uint32_t kchild = wb_const_text(b, "Child");
    uint32_t km = wb_const_text(b, "main");
    uint32_t kcmd = wb_const_text(b, "sleep");
    uint32_t karg = wb_const_text(b, "1");
    uint8_t kinds[4] = {WO_K_SCALAR, WO_K_SCALAR, WO_K_SCALAR, WO_K_SCALAR};
    uint32_t cls = wb_class(b, kchild, 0, kinds, 4);
    uint32_t kcls = wb_const_int(b, (int64_t)cls);
    uint32_t k80 = wb_const_int(b, 80);
    uint32_t k24 = wb_const_int(b, 24);
    uint32_t code[24];
    uint32_t n = 0;
    code[n++] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)kcmd);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 2, WO_K_TEXT, WO_B_MULTI_NEW);
    code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)karg);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 6, 2, WO_B_MULTI_PUSH);
    code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kcls);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 0, 1, WO_B_PROC_SPAWN);
    code[n++] = wo_ins_abc(WOP_DROP, 2, 0, 0);
    code[n++] = wo_ins_abc(WOP_GETF, 1, 0, 1);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 8, 1, WO_B_NET_CLOSE);
    code[n++] = wo_ins_abc(WOP_GETF, 1, 0, 2);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 8, 1, WO_B_NET_CLOSE);
    code[n++] = wo_ins_abc(WOP_GETF, 1, 0, 3);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 8, 1, WO_B_NET_CLOSE);
    code[n++] = wo_ins_abc(WOP_GETF, 4, 0, 0);
    code[n++] = wo_ins_abc(WOP_DROP, 0, 0, 0);
    code[n++] = wo_ins_abx(WOP_LOADK, 5, (uint16_t)k80);
    code[n++] = wo_ins_abx(WOP_LOADK, 6, (uint16_t)k24);
    code[n++] = wo_ins_abc(WOP_BUILTIN, 8, 4, WO_B_PROC_RESIZE);
    code[n++] = wo_ins_abc(WOP_RET0, 0, 0, 0);
    wb_method(b, km, WOB_NONE, 0, 9, code, n, NULL, 0, NULL, 0);
    return wb_finish(b, len);
}

static void test_resize_refuses_on_pipe_child(void) {
    size_t len;
    uint8_t *img = pipe_resize_module(&len);
    wo_module mod;
    char lerr[256];
    T_EQ(wo_load_buf(&mod, img, len, lerr, sizeof lerr), 0);
    T_EQ(wo_vm_init(&VM, &mod, 1 << 20), 0);
    uint64_t ret = 0;
    wo_err err;
    memset(&err, 0, sizeof err);
    T_EQ(wo_vm_call(&VM, 0, NULL, 0, &ret, &err), -1);
    T_EQ(err.code, WO_T_IO);
    T_CHECK(strstr(err.msg, "terminal") != NULL);
    wo_vm_destroy(&VM);
    int st;
    T_EQ(waitpid(-1, &st, WNOHANG), -1);
    T_EQ(errno, ECHILD);
    wo_module_free(&mod);
    free(img);
}

static void on_alarm(int sig) { (void)sig; } /* interrupt, don't die */

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The Task 3 deadlock repro: a child that writes 200 KB to stdout and only
 * THEN a line to stderr. The old sequential drain stopped reading stdout at
 * its 8 KiB cap, the child blocked on the full pipe and never reached
 * stderr, and the parent sat in the stderr read forever. The leg demands
 * completion well under the 5 s alarm — the alarm exists so the OLD code
 * FAILS instead of hanging the suite. */
static void test_chatty_child_completes(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm; /* no SA_RESTART: reads must EINTR */
    sigaction(SIGALRM, &sa, NULL);
    alarm(5);
    int64_t t0 = mono_ms();
    const char *args[] = {"-c", "head -c 200000 /dev/zero; echo done >&2"};
    int64_t code = -99;
    size_t outlen = 0, errlen = 0;
    wo_err err;
    int rc = run_proc_lens("sh", args, 2, &code, &outlen, &errlen, &err);
    int64_t elapsed = mono_ms() - t0;
    alarm(0);
    T_EQ(rc, 0);
    T_EQ(code, 0);
    T_CHECK(elapsed < 4000); /* the old drain needs the alarm to escape */
    T_EQ(outlen, 200000u);
    T_EQ(errlen, 5u); /* "done\n" */
}

int main(void) {
    test_echo();
    test_false();
    test_missing();
    test_chatty_child_completes();
    test_deadline_kills_and_shard_schedules();
    test_cap_refuses_by_name();
    test_errcap_refuses_by_name();
    test_ceiling_fails_closed();
    test_unwind_reaps_child();
    test_thousand_spawns_fd_flat();
    test_stream_echo();
    test_stream_wait();
    test_stream_one_waiter();
    test_stream_churn();
    test_pty_is_a_tty();
    test_pty_size_and_resize();
    test_resize_refuses_on_pipe_child();
    test_stop_kills_child(); /* last: it latches the stop flag */
    return t_report("test_proc");
}

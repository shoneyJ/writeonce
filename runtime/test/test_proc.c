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
    return t_report("test_proc");
}

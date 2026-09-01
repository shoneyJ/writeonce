/* test_term — runtime-v2 3/4/5: signals as events, termios adoption,
 * fd passing. Named for the terminal-facing half of the track. */
#define _XOPEN_SOURCE 700 /* posix_openpt/grantpt/unlockpt + 200809L base */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "cont.h"
#include "gc.h"
#include "loader.h"
#include "t.h"
#include "vm.h"
#include "wob_build.h"

static wo_vm VM;

/* ---- runtime-v2 3: signal.on -------------------------------------------
 * classes: 0 Signal {sig}, 1 Collector {m multi}, 2 Proc {code,out,err}.
 * methods: 0 receive(self, msg) pushes msg.sig into self.m;
 *          1 main(addr): signal.on(SIGUSR1, addr) then a child kills
 *            the test process with USR1; 2 refuse(addr): SIGTERM. */
static uint8_t *signal_module(size_t *len) {
    wb_t *b = wb_new();
    uint32_t ksig = wb_const_text(b, "Signal");
    uint32_t kcol = wb_const_text(b, "Collector");
    uint32_t kproc = wb_const_text(b, "Proc");
    uint32_t krecv = wb_const_text(b, "receive");
    uint32_t kmain = wb_const_text(b, "main");
    uint32_t kref = wb_const_text(b, "refuse");
    uint32_t kcmd = wb_const_text(b, "sh");
    uint32_t kdc = wb_const_text(b, "-c");
    uint32_t kkill = wb_const_text(b, "kill -USR1 $PPID");
    uint8_t sk[1] = {WO_K_SCALAR};
    uint32_t cls_sig = wb_class(b, ksig, 0, sk, 1);
    uint8_t ck[1] = {WO_K_MULTI};
    uint32_t cls_col = wb_class(b, kcol, 0, ck, 1);
    (void)cls_col;
    uint8_t pk[3] = {WO_K_SCALAR, WO_K_TEXT, WO_K_TEXT};
    uint32_t cls_proc = wb_class(b, kproc, 0, pk, 3);
    uint32_t kcls_sig = wb_const_int(b, (int64_t)cls_sig);
    uint32_t kcls_proc = wb_const_int(b, (int64_t)cls_proc);
    uint32_t kusr1 = wb_const_int(b, SIGUSR1);
    uint32_t kterm = wb_const_int(b, SIGTERM);
    uint32_t k100 = wb_const_int(b, 100);
    { /* receive(self, msg): self.m gets msg.sig */
        uint32_t code[8];
        uint32_t n = 0;
        code[n++] = wo_ins_abc(WOP_GETF, 2, 1, 0); /* msg.sig */
        code[n++] = wo_ins_abc(WOP_GETF, 3, 0, 0); /* self.m */
        code[n++] = wo_ins_abc(WOP_MOVE, 4, 2, 0);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 5, 3, WO_B_MULTI_PUSH);
        code[n++] = wo_ins_abc(WOP_RET0, 0, 0, 0);
        wb_method(b, krecv, WOB_NONE, 2, 6, code, n, NULL, 0, NULL, 0);
    }
    { /* main(addr) */
        uint32_t code[24];
        uint32_t n = 0;
        code[n++] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)kusr1);
        code[n++] = wo_ins_abc(WOP_MOVE, 2, 0, 0);
        code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kcls_sig);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 4, 1, WO_B_SIGNAL_ON);
        /* a child delivers SIGUSR1 to this process */
        code[n++] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)kcmd);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 2, WO_K_TEXT, WO_B_MULTI_NEW);
        code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kdc);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 6, 2, WO_B_MULTI_PUSH);
        code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kkill);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 6, 2, WO_B_MULTI_PUSH);
        code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kcls_proc);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 0, 1, WO_B_PROC_RUN);
        code[n++] = wo_ins_abc(WOP_DROP, 2, 0, 0);
        code[n++] = wo_ins_abc(WOP_DROP, 0, 0, 0);
        /* give the queued delivery a slice */
        code[n++] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)k100);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 2, 1, WO_B_TIME_SLEEP);
        code[n++] = wo_ins_abc(WOP_RET0, 0, 0, 0);
        wb_drop drops[] = {{.pc = 11, .owned = 1u << 2, .gc = 0}};
        wb_method(b, kmain, WOB_NONE, 1, 7, code, n, NULL, 0, drops, 1);
    }
    { /* refuse(addr): SIGTERM must refuse naming the stop latch */
        uint32_t code[8];
        uint32_t n = 0;
        code[n++] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)kterm);
        code[n++] = wo_ins_abc(WOP_MOVE, 2, 0, 0);
        code[n++] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kcls_sig);
        code[n++] = wo_ins_abc(WOP_BUILTIN, 4, 1, WO_B_SIGNAL_ON);
        code[n++] = wo_ins_abc(WOP_RET0, 0, 0, 0);
        wb_method(b, kref, WOB_NONE, 1, 5, code, n, NULL, 0, NULL, 0);
    }
    return wb_finish(b, len);
}

static void test_signal_on_delivers_record(void) {
    size_t len;
    uint8_t *img = signal_module(&len);
    wo_module mod;
    char lerr[256];
    T_EQ(wo_load_buf(&mod, img, len, lerr, sizeof lerr), 0);
    T_EQ(wo_vm_init(&VM, &mod, 1 << 20), 0);
    /* the collector actor: state = Collector{m} */
    wo_multi *m = wo_multi_new(&VM.rt, WO_K_SCALAR);
    T_CHECK(m != NULL);
    wo_hdr *inst = wo_obj_new(&VM.rt, 1 /* Collector */);
    T_CHECK(inst != NULL);
    wo_fields(inst)[0] = (uint64_t)(uintptr_t)m;
    uint64_t addr = 0;
    const char *emsg = NULL;
    T_EQ(wo_vm_actor_spawn(&VM, (uint64_t)(uintptr_t)inst, 0, &addr, &emsg), 0);
    uint64_t args[1] = {addr};
    uint64_t ret = 0;
    wo_err err;
    memset(&err, 0, sizeof err);
    T_EQ(wo_vm_call(&VM, 1, args, 1, &ret, &err), 0);
    T_EQ(m->len, 1u); /* one delivery, coalesced */
    if (m->len == 1) {
        uint64_t v = 0;
        T_EQ(wo_multi_get(m, 0, &v), 0);
        T_EQ(v, (uint64_t)SIGUSR1);
    }
    /* SIGTERM registration refuses naming the stop latch */
    memset(&err, 0, sizeof err);
    T_EQ(wo_vm_call(&VM, 2, args, 1, &ret, &err), -1);
    T_EQ(err.code, WO_T_IO);
    T_CHECK(strstr(err.msg, "stop latch") != NULL);
    wo_vm_destroy(&VM); /* drops the actor state and the multi */
    int st;
    T_EQ(waitpid(-1, &st, WNOHANG), -1);
    T_EQ(errno, ECHILD);
    wo_module_free(&mod);
    free(img);
    /* leave no handler behind for later suites in this process */
    signal(SIGUSR1, SIG_DFL);
}

/* ---- runtime-v2 4: termios adoption --------------------------------------
 * methods on an fd argument: 0 raw(fd), 1 restore(fd), 2 trap_while_raw
 * (raw then DIV0 — the unwind must restore). */
static uint8_t *term_module(size_t *len) {
    wb_t *b = wb_new();
    uint32_t kraw = wb_const_text(b, "raw");
    uint32_t kres = wb_const_text(b, "restore");
    uint32_t ktrap = wb_const_text(b, "trap_while_raw");
    uint32_t kone = wb_const_int(b, 1);
    uint32_t kzero = wb_const_int(b, 0);
    { /* raw(fd) */
        uint32_t code[3];
        code[0] = wo_ins_abc(WOP_BUILTIN, 1, 0, WO_B_TERM_RAW);
        code[1] = wo_ins_abc(WOP_RET0, 0, 0, 0);
        wb_method(b, kraw, WOB_NONE, 1, 2, code, 2, NULL, 0, NULL, 0);
    }
    { /* restore(fd) */
        uint32_t code[3];
        code[0] = wo_ins_abc(WOP_BUILTIN, 1, 0, WO_B_TERM_RESTORE);
        code[1] = wo_ins_abc(WOP_RET0, 0, 0, 0);
        wb_method(b, kres, WOB_NONE, 1, 2, code, 2, NULL, 0, NULL, 0);
    }
    { /* trap_while_raw(fd): raw, then DIV0 */
        uint32_t code[6];
        code[0] = wo_ins_abc(WOP_BUILTIN, 1, 0, WO_B_TERM_RAW);
        code[1] = wo_ins_abx(WOP_LOADK, 1, (uint16_t)kone);
        code[2] = wo_ins_abx(WOP_LOADK, 2, (uint16_t)kzero);
        code[3] = wo_ins_abc(WOP_DIV, 3, 1, 2);
        code[4] = wo_ins_abc(WOP_RET0, 0, 0, 0);
        wb_method(b, ktrap, WOB_NONE, 1, 4, code, 5, NULL, 0, NULL, 0);
    }
    return wb_finish(b, len);
}

static void test_term_raw_restore(void) {
    /* a real tty pair, made by the TEST (no builtin involved) */
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    T_CHECK(master >= 0);
    T_EQ(grantpt(master), 0);
    T_EQ(unlockpt(master), 0);
    char sname[128];
    T_EQ(ptsname_r(master, sname, sizeof sname), 0);
    int slave = open(sname, O_RDWR | O_NOCTTY);
    T_CHECK(slave >= 0);
    struct termios t0;
    T_EQ(tcgetattr(slave, &t0), 0);
    T_CHECK(t0.c_lflag & ECHO);

    size_t len;
    uint8_t *img = term_module(&len);
    wo_module mod;
    char lerr[256];
    T_EQ(wo_load_buf(&mod, img, len, lerr, sizeof lerr), 0);
    T_EQ(wo_vm_init(&VM, &mod, 1 << 20), 0);
    uint64_t args[1] = {(uint64_t)slave};
    uint64_t ret = 0;
    wo_err err;

    /* raw clears ECHO/ICANON */
    memset(&err, 0, sizeof err);
    T_EQ(wo_vm_call(&VM, 0, args, 1, &ret, &err), 0);
    struct termios tr;
    T_EQ(tcgetattr(slave, &tr), 0);
    T_CHECK((tr.c_lflag & (ECHO | ICANON)) == 0);
    /* restore brings the saved flags back */
    memset(&err, 0, sizeof err);
    T_EQ(wo_vm_call(&VM, 1, args, 1, &ret, &err), 0);
    struct termios t1;
    T_EQ(tcgetattr(slave, &t1), 0);
    T_EQ((long long)t1.c_lflag, (long long)t0.c_lflag);
    /* restore with nothing saved refuses by name */
    memset(&err, 0, sizeof err);
    T_EQ(wo_vm_call(&VM, 1, args, 1, &ret, &err), -1);
    T_CHECK(strstr(err.msg, "never made raw") != NULL);
    /* double raw refuses by name — and the refusal is a TRAP, so the
     * full unwind restores the first raw (the obligation, observed) */
    memset(&err, 0, sizeof err);
    T_EQ(wo_vm_call(&VM, 0, args, 1, &ret, &err), 0);
    memset(&err, 0, sizeof err);
    T_EQ(wo_vm_call(&VM, 0, args, 1, &ret, &err), -1);
    T_CHECK(strstr(err.msg, "already raw") != NULL);
    T_EQ(tcgetattr(slave, &t1), 0);
    T_EQ((long long)t1.c_lflag, (long long)t0.c_lflag);
    /* a trap while raw still restores — the obligation, second proof */
    memset(&err, 0, sizeof err);
    T_EQ(wo_vm_call(&VM, 2, args, 1, &ret, &err), -1);
    T_EQ(tcgetattr(slave, &t1), 0);
    T_EQ((long long)t1.c_lflag, (long long)t0.c_lflag);

    wo_vm_destroy(&VM);
    wo_module_free(&mod);
    free(img);
    close(slave);
    close(master);
}

int main(void) {
    test_signal_on_delivers_record();
    test_term_raw_restore();
    return t_report("test_term");
}

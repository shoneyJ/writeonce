/* test_proc — iteration 42: proc.run's contract, then its bounds.
 *
 * Task 2 pins what the one-shot form already promises (exit code, captured
 * stdout, the execvp-failed 127) so the parked rework in Task 3 has a
 * baseline that must not move. Later tasks add the bounds legs: deadline,
 * output caps, ceiling, fd hygiene, stop/unwind reaping. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void) {
    test_echo();
    test_false();
    test_missing();
    return t_report("test_proc");
}

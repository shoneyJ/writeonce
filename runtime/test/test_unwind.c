/* test_unwind — borrow/rc opcodes and drop-map trap unwinding: the spec's
 * "traps never leak" promise. Classes use the malloc-path trick (~130
 * fields) so ASan proves every free on the trap paths. */
#include <stdlib.h>

#include "loader.h"
#include "t.h"
#include "vm.h"
#include "wob_build.h"

#define BIG 130

static wo_vm VM;

static int run_img(uint8_t *img, size_t len, uint64_t *ret, wo_err *err) {
    wo_module mod;
    char lerr[256];
    if (wo_load_buf(&mod, img, len, lerr, sizeof lerr) != 0) {
        fprintf(stderr, "loader rejected test image: %s\n", lerr);
        return -2;
    }
    if (wo_vm_init(&VM, &mod, 1 << 20) != 0) {
        wo_module_free(&mod);
        return -2;
    }
    int rc = wo_vm_call(&VM, 0, NULL, 0, ret, err);
    wo_vm_destroy(&VM);
    wo_module_free(&mod);
    return rc;
}

static uint8_t big_kinds[BIG]; /* all scalars */

/* Double-exclusive borrow traps BORROW at the right line; the owned object
 * named in the frame's drop mask is freed during unwinding. */
static void test_borrow_violation_frees_owned(void) {
    wb_t *b = wb_new();
    uint32_t kc = wb_const_text(b, "Big");
    uint32_t kf = wb_const_text(b, "main");
    wb_class(b, kc, 0, big_kinds, BIG);
    uint32_t code[] = {
        wo_ins_abx(WOP_NEW, 0, 0),
        wo_ins_abc(WOP_BORROW_X, 0, 0, 0), /* ok */
        wo_ins_abc(WOP_BORROW_X, 0, 0, 0), /* violation: line 99 */
        wo_ins_abc(WOP_RET0, 0, 0, 0),
    };
    uint32_t lines[] = {2, 99};
    wb_drop drops[] = {{.pc = 1, .owned = 1u << 0, .gc = 0}};
    wb_method(b, kf, WOB_NONE, 0, 2, code, 4, lines, 1, drops, 1);
    size_t len;
    uint8_t *img = wb_finish(b, &len);
    uint64_t ret = 0;
    wo_err err = {0};
    T_EQ(run_img(img, len, &ret, &err), -1);
    T_EQ(err.code, WO_T_BORROW);
    T_EQ(err.line, 99);
    T_EQ(wo_vm_depth(&VM), 0);
    free(img); /* ASan: the Big instance must have been freed by unwinding */
}

/* A child method traps mid-body: owned objects in BOTH frames are freed —
 * the child's argument (moved in), the child's own allocation, and the
 * caller's local. */
static void test_two_frame_unwind_frees_both(void) {
    wb_t *b = wb_new();
    uint32_t kc = wb_const_text(b, "Big");
    uint32_t kchild = wb_const_text(b, "child");
    uint32_t kmain = wb_const_text(b, "main");
    wb_class(b, kc, 0, big_kinds, BIG);
    /* method 0 = child(o): allocates its own Big, then traps */
    uint32_t ccode[] = {
        wo_ins_abx(WOP_NEW, 1, 0),
        wo_ins_abx(WOP_TRAP, 0, WO_T_EXPLICIT),
    };
    wb_drop cdrops[] = {{.pc = 1, .owned = (1u << 0) | (1u << 1), .gc = 0}};
    wb_method(b, kchild, WOB_NONE, 1, 3, ccode, 2, NULL, 0, cdrops, 1);
    /* method 1 = main: local Big in r0, arg Big in the call window r2 */
    uint32_t mcode[] = {
        wo_ins_abx(WOP_NEW, 0, 0),
        wo_ins_abx(WOP_NEW, 2, 0),
        wo_ins_abx(WOP_CALL, 2, 0), /* pc 2: ownership of r2 moves in */
        wo_ins_abc(WOP_DROP, 0, 0, 0),
        wo_ins_abc(WOP_RET0, 0, 0, 0),
    };
    wb_drop mdrops[] = {{.pc = 2, .owned = 1u << 0, .gc = 0}};
    wb_method(b, kmain, WOB_NONE, 0, 4, mcode, 5, NULL, 0, mdrops, 1);
    wb_entry(b, 1);
    size_t len;
    uint8_t *img = wb_finish(b, &len);

    wo_module mod;
    char lerr[256];
    T_EQ(wo_load_buf(&mod, img, len, lerr, sizeof lerr), 0);
    T_EQ(wo_vm_init(&VM, &mod, 1 << 20), 0);
    uint64_t ret = 0;
    wo_err err = {0};
    T_EQ(wo_vm_call(&VM, 1, NULL, 0, &ret, &err), -1);
    T_EQ(err.code, WO_T_EXPLICIT);
    T_STREQ(err.method, "child");
    T_EQ(wo_vm_depth(&VM), 0);
    wo_vm_destroy(&VM);
    wo_module_free(&mod);
    free(img); /* ASan: all three Big instances freed across both frames */
}

/* rc inc/dec through opcodes frees exactly at zero (@gc malloc-path class) */
/* iteration 7b: opcodes 27-28 (the old RC_INC/RC_DEC) are reserved in .wob
 * v4 — the loader must reject an image that carries one, exactly like any
 * other unknown opcode. A traced instance abandoned by a clean return is
 * rt_destroy's to free (ASan proves it). */
static void test_reserved_rc_opcode_rejected(void) {
    wb_t *b = wb_new();
    uint32_t kc = wb_const_text(b, "GBig");
    uint32_t kf = wb_const_text(b, "main");
    wb_class(b, kc, WO_CLASSF_GC, big_kinds, BIG);
    uint32_t code[] = {
        wo_ins_abx(WOP_NEW, 0, 0),
        wo_ins_abc(27 /* retired RC_INC */, 0, 0, 0),
        wo_ins_abc(WOP_RET0, 0, 0, 0),
    };
    wb_method(b, kf, WOB_NONE, 0, 1, code, 3, NULL, 0, NULL, 0);
    size_t len;
    uint8_t *img = wb_finish(b, &len);
    uint64_t ret = 0;
    wo_err err;
    T_EQ(run_img(img, len, &ret, &err), -2); /* loader rejection */
    free(img);
}

static void test_abandoned_traced_freed_at_destroy(void) {
    wb_t *b = wb_new();
    uint32_t kc = wb_const_text(b, "GBig");
    uint32_t kf = wb_const_text(b, "main");
    wb_class(b, kc, WO_CLASSF_GC, big_kinds, BIG);
    uint32_t code[] = {
        wo_ins_abx(WOP_NEW, 0, 0), /* traced, linked; never dropped */
        wo_ins_abc(WOP_RET0, 0, 0, 0),
    };
    wb_method(b, kf, WOB_NONE, 0, 1, code, 2, NULL, 0, NULL, 0);
    size_t len;
    uint8_t *img = wb_finish(b, &len);
    uint64_t ret = 0;
    wo_err err;
    T_EQ(run_img(img, len, &ret, &err), 0); /* ASan: rt_destroy frees it */
    free(img);
}

int main(void) {
    test_borrow_violation_frees_owned();
    test_two_frame_unwind_frees_both();
    test_reserved_rc_opcode_rejected();
    test_abandoned_traced_freed_at_destroy();
    return t_report("test_unwind");
}

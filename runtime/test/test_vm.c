/* test_vm — interpreter core: window calls, fib recursion, div-by-zero
 * with line lookup, frame-cap stack overflow, depth zero after traps. */
#include <stdlib.h>

#include "loader.h"
#include "t.h"
#include "vm.h"
#include "wob_build.h"

static wo_vm VM; /* 32K value stack: keep it off the C stack */

/* run one image's entry-shaped method by index with args */
static int run(uint8_t *img, size_t len, uint32_t mi, const uint64_t *args,
               uint32_t argc, uint64_t *ret, wo_err *err) {
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
    int rc = wo_vm_call(&VM, mi, args, argc, ret, err);
    int depth_after = (int)wo_vm_depth(&VM);
    wo_vm_destroy(&VM);
    wo_module_free(&mod);
    return rc == 0 ? 0 : (depth_after == 0 ? -1 : -3);
}

static void test_add_window_convention(void) {
    wb_t *b = wb_new();
    uint32_t kn = wb_const_text(b, "add");
    uint32_t code[] = {
        wo_ins_abc(WOP_ADD, 2, 0, 1),
        wo_ins_abc(WOP_RET, 2, 0, 0),
    };
    wb_method(b, kn, WOB_NONE, 2, 3, code, 2, NULL, 0, NULL, 0);
    size_t len;
    uint8_t *img = wb_finish(b, &len);
    uint64_t args[] = {3, 4}, ret = 0;
    wo_err err;
    T_EQ(run(img, len, 0, args, 2, &ret, &err), 0);
    T_EQ(ret, 7);
    free(img);
}

static void test_fib_recursion(void) {
    wb_t *b = wb_new();
    uint32_t kn = wb_const_text(b, "fib");
    uint32_t k1 = wb_const_int(b, 1);
    uint32_t k2 = wb_const_int(b, 2);
    /* fib(n): n<2 -> n; else fib(n-1)+fib(n-2). regc 6, argc 1. */
    uint32_t code[] = {
        wo_ins_abx(WOP_LOADK, 1, (uint16_t)k2), /* 0: r1 = 2 */
        wo_ins_abc(WOP_LT, 2, 0, 1),            /* 1: r2 = n < 2 */
        wo_ins_asbx(WOP_JZ, 2, 1),              /* 2: if !r2 -> pc 4 */
        wo_ins_abc(WOP_RET, 0, 0, 0),           /* 3: return n */
        wo_ins_abx(WOP_LOADK, 1, (uint16_t)k1), /* 4: r1 = 1 */
        wo_ins_abc(WOP_SUB, 4, 0, 1),           /* 5: r4 = n-1 */
        wo_ins_abx(WOP_CALL, 4, 0),             /* 6: r4 = fib(r4) */
        wo_ins_abx(WOP_LOADK, 1, (uint16_t)k2), /* 7: r1 = 2 */
        wo_ins_abc(WOP_SUB, 5, 0, 1),           /* 8: r5 = n-2 */
        wo_ins_abx(WOP_CALL, 5, 0),             /* 9: r5 = fib(r5) */
        wo_ins_abc(WOP_ADD, 0, 4, 5),           /* 10: r0 = r4+r5 */
        wo_ins_abc(WOP_RET, 0, 0, 0),           /* 11 */
    };
    wb_method(b, kn, WOB_NONE, 1, 6, code, 12, NULL, 0, NULL, 0);
    size_t len;
    uint8_t *img = wb_finish(b, &len);
    uint64_t args[] = {10}, ret = 0;
    wo_err err;
    T_EQ(run(img, len, 0, args, 1, &ret, &err), 0);
    T_EQ(ret, 55);
    free(img);
}

static void test_div_zero_traps_with_line(void) {
    wb_t *b = wb_new();
    uint32_t kn = wb_const_text(b, "divit");
    uint32_t k10 = wb_const_int(b, 10);
    uint32_t k0 = wb_const_int(b, 0);
    uint32_t code[] = {
        wo_ins_abx(WOP_LOADK, 0, (uint16_t)k10),
        wo_ins_abx(WOP_LOADK, 1, (uint16_t)k0),
        wo_ins_abc(WOP_DIV, 2, 0, 1), /* pc 2, line 42 */
        wo_ins_abc(WOP_RET, 2, 0, 0),
    };
    uint32_t lines[] = {0, 40, 2, 42, 3, 43};
    wb_method(b, kn, WOB_NONE, 0, 3, code, 4, lines, 3, NULL, 0);
    size_t len;
    uint8_t *img = wb_finish(b, &len);
    uint64_t ret = 0;
    wo_err err = {0};
    T_EQ(run(img, len, 0, NULL, 0, &ret, &err), -1); /* trap, depth 0 */
    T_EQ(err.code, WO_T_DIV0);
    T_EQ(err.line, 42);
    T_STREQ(err.method, "divit");
    free(img);
}

static void test_stack_overflow_at_frame_cap(void) {
    wb_t *b = wb_new();
    uint32_t kn = wb_const_text(b, "boom");
    uint32_t code[] = {
        wo_ins_abx(WOP_CALL, 1, 0), /* call self forever, window +1 */
        wo_ins_abc(WOP_RET0, 0, 0, 0),
    };
    wb_method(b, kn, WOB_NONE, 0, 2, code, 2, NULL, 0, NULL, 0);
    size_t len;
    uint8_t *img = wb_finish(b, &len);
    uint64_t ret = 0;
    wo_err err = {0};
    T_EQ(run(img, len, 0, NULL, 0, &ret, &err), -1);
    T_EQ(err.code, WO_T_STACK);
    free(img);
}

int main(void) {
    test_add_window_convention();
    test_fib_recursion();
    test_div_zero_traps_with_line();
    test_stack_overflow_at_frame_cap();
    return t_report("test_vm");
}

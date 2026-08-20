/* test_fiber — the arc's stage 1 Task 2: run queue + reduction budget.
 *
 *  1. Under WO_REDUCTIONS=1, three fibers pushing tags into one shared
 *     multi interleave in EXACT round-robin — the deterministic-
 *     scheduling criterion.
 *  2. Main returning reaps a still-looping fiber holding an owned
 *     object: the drop map runs (ASan proves the free).
 *  3. A spawned fiber's uncaught trap (DIV0) kills that fiber alone;
 *     main finishes with rc 0 — the isolation rule.
 */
#define _POSIX_C_SOURCE 200112L /* setenv/unsetenv under -std=c11 */
#include <stdlib.h>

#include "cont.h"
#include "gc.h"
#include "loader.h"
#include "t.h"
#include "vm.h"
#include "wob_build.h"

#define BIG 130 /* malloc-path class so ASan sees the free (test_unwind's trick) */

static wo_vm VM;
static uint8_t big_kinds[BIG];

/* Worker: argc=2 (r0 = shared multi as a raw word, r1 = tag), pushes the
 * tag K times. The backward JMP is the budget's yield site.
 *   pc0 LOADK r2,#0   pc1 LOADK r3,#K
 *   pc2 LT r4,r2,r3   pc3 JZ r4,+4 -> pc8
 *   pc4 BUILTIN r4, base=0, MULTI_PUSH  (container r0, element r1)
 *   pc5 LOADK r4,#1   pc6 ADD r2,r2,r4
 *   pc7 JMP -6 -> pc2   pc8 RET0 */
static void worker_code(uint32_t *code, uint32_t k0, uint32_t kK, uint32_t k1) {
    code[0] = wo_ins_abx(WOP_LOADK, 2, (uint16_t)k0);
    code[1] = wo_ins_abx(WOP_LOADK, 3, (uint16_t)kK);
    code[2] = wo_ins_abc(WOP_LT, 4, 2, 3);
    code[3] = wo_ins_asbx(WOP_JZ, 4, 4);
    code[4] = wo_ins_abc(WOP_BUILTIN, 4, 0, WO_B_MULTI_PUSH);
    code[5] = wo_ins_abx(WOP_LOADK, 4, (uint16_t)k1);
    code[6] = wo_ins_abc(WOP_ADD, 2, 2, 4);
    code[7] = wo_ins_asbx(WOP_JMP, 0, -6);
    code[8] = wo_ins_abc(WOP_RET0, 0, 0, 0);
}

static void test_round_robin(void) {
    wb_t *b = wb_new();
    uint32_t kname = wb_const_text(b, "worker");
    uint32_t k0 = wb_const_int(b, 0);
    uint32_t kK = wb_const_int(b, 5);
    uint32_t k1 = wb_const_int(b, 1);
    uint32_t code[9];
    worker_code(code, k0, kK, k1);
    uint32_t lines[] = {0, 1};
    wb_method(b, kname, WOB_NONE, 2, 8, code, 9, lines, 1, NULL, 0);
    size_t len;
    uint8_t *img = wb_finish(b, &len);

    wo_module mod;
    char lerr[256];
    T_EQ(wo_load_buf(&mod, img, len, lerr, sizeof lerr), 0);
    setenv("WO_REDUCTIONS", "1", 1);
    T_EQ(wo_vm_init(&VM, &mod, 1 << 20), 0);
    unsetenv("WO_REDUCTIONS");

    wo_multi *m = wo_multi_new(&VM.rt, WO_K_SCALAR);
    T_CHECK(m != NULL);
    uint64_t a2[2] = {(uint64_t)(uintptr_t)m, 2};
    uint64_t a3[2] = {(uint64_t)(uintptr_t)m, 3};
    T_CHECK(wo_vm_spawn_fiber(&VM, 0, a2, 2) != NULL);
    T_CHECK(wo_vm_spawn_fiber(&VM, 0, a3, 2) != NULL);

    uint64_t ret = 0;
    wo_err err = {0};
    uint64_t a1[2] = {(uint64_t)(uintptr_t)m, 1};
    T_EQ(wo_vm_call(&VM, 0, a1, 2, &ret, &err), 0);

    /* budget 1: every backward JMP yields, so one push per turn — the
     * order is main(1), fiber(2), fiber(3), repeated exactly */
    T_EQ(m->len, 15u);
    for (uint32_t i = 0; i < 15; i++) {
        uint64_t v = 0;
        T_EQ(wo_multi_get(m, i, &v), 0);
        T_EQ(v, (uint64_t)(i % 3) + 1);
    }
    wo_drop_obj(&VM.rt, (wo_hdr *)m); /* the test owns m; drop frees items */
    wo_vm_destroy(&VM);
    wo_module_free(&mod);
    free(img);
}

/* Main loops 3 times (yielding), then returns; the worker allocated a Big
 * (owned, in its drop mask) and loops forever. Main's return must reap it
 * drop-clean — ASan fails this test if the Big leaks. */
static void test_main_return_reaps(void) {
    wb_t *b = wb_new();
    uint32_t kbig = wb_const_text(b, "Big");
    uint32_t kw = wb_const_text(b, "spin");
    uint32_t km = wb_const_text(b, "main");
    wb_class(b, kbig, 0, big_kinds, BIG);
    /* spin: NEW r0 Big, then loop forever (backward JMP = yields) */
    uint32_t wcode[] = {
        wo_ins_abx(WOP_NEW, 0, 0),
        wo_ins_asbx(WOP_JMP, 0, -1), /* pc1 -> pc1: jump to itself */
    };
    wb_drop wdrops[] = {{.pc = 1, .owned = 1u << 0, .gc = 0}};
    uint32_t wl[] = {0, 1};
    wb_method(b, kw, WOB_NONE, 0, 2, wcode, 2, wl, 1, wdrops, 1);
    /* main: count 0..3 with backward jumps, then RET0 */
    uint32_t k0 = wb_const_int(b, 0);
    uint32_t kK = wb_const_int(b, 3);
    uint32_t k1 = wb_const_int(b, 1);
    uint32_t mcode[] = {
        wo_ins_abx(WOP_LOADK, 0, (uint16_t)k0),
        wo_ins_abx(WOP_LOADK, 1, (uint16_t)kK),
        wo_ins_abc(WOP_LT, 2, 0, 1),
        wo_ins_asbx(WOP_JZ, 2, 3),
        wo_ins_abx(WOP_LOADK, 2, (uint16_t)k1),
        wo_ins_abc(WOP_ADD, 0, 0, 2),
        wo_ins_asbx(WOP_JMP, 0, -5),
        wo_ins_abc(WOP_RET0, 0, 0, 0),
    };
    uint32_t ml[] = {0, 1};
    wb_method(b, km, WOB_NONE, 0, 3, mcode, 8, ml, 1, NULL, 0);
    size_t len;
    uint8_t *img = wb_finish(b, &len);

    wo_module mod;
    char lerr[256];
    T_EQ(wo_load_buf(&mod, img, len, lerr, sizeof lerr), 0);
    setenv("WO_REDUCTIONS", "1", 1);
    T_EQ(wo_vm_init(&VM, &mod, 1 << 20), 0);
    unsetenv("WO_REDUCTIONS");
    T_CHECK(wo_vm_spawn_fiber(&VM, 0, NULL, 0) != NULL); /* spin */
    uint64_t ret = 0;
    wo_err err = {0};
    T_EQ(wo_vm_call(&VM, 1, NULL, 0, &ret, &err), 0); /* main */
    wo_vm_destroy(&VM);
    wo_module_free(&mod);
    free(img); /* ASan: spin's Big must have been freed by the reap */
}

/* A spawned fiber divides by zero; the program (main) still answers 0. */
static void test_fiber_trap_isolated(void) {
    wb_t *b = wb_new();
    uint32_t kw = wb_const_text(b, "boom");
    uint32_t km = wb_const_text(b, "main");
    uint32_t kone = wb_const_int(b, 1);
    uint32_t kzero = wb_const_int(b, 0);
    uint32_t wcode[] = {
        wo_ins_abx(WOP_LOADK, 0, (uint16_t)kone),
        wo_ins_abx(WOP_LOADK, 1, (uint16_t)kzero),
        wo_ins_abc(WOP_DIV, 2, 0, 1), /* DIV0: uncaught, fiber dies alone */
        wo_ins_abc(WOP_RET0, 0, 0, 0),
    };
    uint32_t wl[] = {0, 1};
    wb_method(b, kw, WOB_NONE, 0, 3, wcode, 4, wl, 1, NULL, 0);
    uint32_t kK = wb_const_int(b, 3);
    uint32_t mcode[] = {
        wo_ins_abx(WOP_LOADK, 0, (uint16_t)kzero),
        wo_ins_abx(WOP_LOADK, 1, (uint16_t)kK),
        wo_ins_abc(WOP_LT, 2, 0, 1),
        wo_ins_asbx(WOP_JZ, 2, 3),
        wo_ins_abx(WOP_LOADK, 2, (uint16_t)kone),
        wo_ins_abc(WOP_ADD, 0, 0, 2),
        wo_ins_asbx(WOP_JMP, 0, -5),
        wo_ins_abc(WOP_RET0, 0, 0, 0),
    };
    uint32_t ml[] = {0, 1};
    wb_method(b, km, WOB_NONE, 0, 3, mcode, 8, ml, 1, NULL, 0);
    size_t len;
    uint8_t *img = wb_finish(b, &len);

    wo_module mod;
    char lerr[256];
    T_EQ(wo_load_buf(&mod, img, len, lerr, sizeof lerr), 0);
    setenv("WO_REDUCTIONS", "1", 1);
    T_EQ(wo_vm_init(&VM, &mod, 1 << 20), 0);
    unsetenv("WO_REDUCTIONS");
    T_CHECK(wo_vm_spawn_fiber(&VM, 0, NULL, 0) != NULL); /* boom */
    uint64_t ret = 0;
    wo_err err = {0};
    T_EQ(wo_vm_call(&VM, 1, NULL, 0, &ret, &err), 0);
    wo_vm_destroy(&VM);
    wo_module_free(&mod);
    free(img);
}

int main(void) {
    test_round_robin();
    test_main_return_reaps();
    test_fiber_trap_isolated();
    return t_report("test_fiber");
}

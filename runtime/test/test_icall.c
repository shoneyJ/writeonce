/* test_icall — structural-interface dispatch: one ICALL site, two classes,
 * two different answers chosen by the receiver's class; missing vtable
 * entry traps with the no-vtable message. */
#include <stdlib.h>

#include "loader.h"
#include "t.h"
#include "vm.h"
#include "wob_build.h"

static wo_vm VM;

static int run_img(uint8_t *img, size_t len, uint32_t mi, uint64_t *ret,
                   wo_err *err) {
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
    int rc = wo_vm_call(&VM, mi, NULL, 0, ret, err);
    wo_vm_destroy(&VM);
    wo_module_free(&mod);
    return rc;
}

static uint8_t *build(int with_vtabs, size_t *len) {
    wb_t *b = wb_new();
    uint32_t kA = wb_const_text(b, "CA");
    uint32_t kB = wb_const_text(b, "CB");
    uint32_t kI = wb_const_text(b, "Priced");
    uint32_t km = wb_const_text(b, "price");
    uint32_t kmain = wb_const_text(b, "main");
    uint32_t k10 = wb_const_int(b, 10);
    uint32_t k20 = wb_const_int(b, 20);
    uint8_t kinds[] = {WO_K_SCALAR};
    uint32_t ca = wb_class(b, kA, 0, kinds, 1);
    uint32_t cb = wb_class(b, kB, 0, kinds, 1);
    wb_iface(b, kI, 1); /* one method -> global slot 0 */

    /* method 0: CA.price(self) -> 10 ; method 1: CB.price(self) -> 20 */
    uint32_t code_a[] = {
        wo_ins_abx(WOP_LOADK, 1, (uint16_t)k10),
        wo_ins_abc(WOP_RET, 1, 0, 0),
    };
    uint32_t ma = wb_method(b, km, ca, 1, 2, code_a, 2, NULL, 0, NULL, 0);
    uint32_t code_b[] = {
        wo_ins_abx(WOP_LOADK, 1, (uint16_t)k20),
        wo_ins_abc(WOP_RET, 1, 0, 0),
    };
    uint32_t mb = wb_method(b, km, cb, 1, 2, code_b, 2, NULL, 0, NULL, 0);
    if (with_vtabs) {
        wb_vtab(b, ca, 0, &ma, 1);
        wb_vtab(b, cb, 0, &mb, 1);
    }

    /* method 2: main — same ICALL site, both classes */
    uint32_t code_m[] = {
        wo_ins_abx(WOP_NEW, 1, (uint16_t)ca),
        wo_ins_abx(WOP_ICALL, 1, 0), /* r1 = priced(r1) via slot 0 */
        wo_ins_abx(WOP_NEW, 2, (uint16_t)cb),
        wo_ins_abx(WOP_ICALL, 2, 0), /* r2 = priced(r2) via the SAME slot */
        wo_ins_abc(WOP_ADD, 0, 1, 2),
        wo_ins_abc(WOP_RET, 0, 0, 0),
    };
    wb_method(b, kmain, WOB_NONE, 0, 3, code_m, 6, NULL, 0, NULL, 0);
    return wb_finish(b, len);
}

static void test_dynamic_dispatch_by_class(void) {
    size_t len;
    uint8_t *img = build(1, &len);
    uint64_t ret = 0;
    wo_err err;
    T_EQ(run_img(img, len, 2, &ret, &err), 0);
    T_EQ(ret, 30); /* 10 + 20: both impls reached through one call site */
    free(img);
}

static void test_missing_vtable_traps(void) {
    size_t len;
    uint8_t *img = build(0, &len); /* same classes, no vtable rows */
    uint64_t ret = 0;
    wo_err err = {0};
    T_EQ(run_img(img, len, 2, &ret, &err), -1);
    T_EQ(err.code, WO_T_BOUNDS);
    T_CHECK(strstr(err.msg, "no vtable entry") != NULL);
    free(img);
}

int main(void) {
    test_dynamic_dispatch_by_class();
    test_missing_vtable_traps();
    return t_report("test_icall");
}

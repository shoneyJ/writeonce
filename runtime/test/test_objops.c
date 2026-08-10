/* test_objops — NEW / GETF / SETF / DROP through bytecode, plus the
 * residual runtime checks (field bounds, null receiver) trapping BOUNDS. */
#include <stdlib.h>

#include "loader.h"
#include "t.h"
#include "vm.h"
#include "wob_build.h"

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

/* image with class Point{x,y} and a single free fn built from `code` */
static uint8_t *point_image(const uint32_t *code, uint32_t n, size_t *len) {
    wb_t *b = wb_new();
    uint32_t kp = wb_const_text(b, "Point");
    uint32_t kf = wb_const_text(b, "go");
    wb_const_int(b, 7); /* constant index 2 */
    uint8_t kinds[] = {WO_K_SCALAR, WO_K_SCALAR};
    wb_class(b, kp, 0, kinds, 2);
    wb_method(b, kf, WOB_NONE, 0, 3, code, n, NULL, 0, NULL, 0);
    return wb_finish(b, len);
}

static void test_new_set_get_drop_roundtrip(void) {
    uint32_t code[] = {
        wo_ins_abx(WOP_NEW, 0, 0),      /* r0 = new Point */
        wo_ins_abx(WOP_LOADK, 1, 2),    /* r1 = 7 */
        wo_ins_abc(WOP_SETF, 0, 0, 1),  /* r0.f0 = r1 */
        wo_ins_abc(WOP_GETF, 2, 0, 0),  /* r2 = r0.f0 */
        wo_ins_abc(WOP_DROP, 0, 0, 0),  /* drop r0 (nulls the register) */
        wo_ins_abc(WOP_RET, 2, 0, 0),
    };
    size_t len;
    uint8_t *img = point_image(code, 6, &len);
    uint64_t ret = 0;
    wo_err err;
    T_EQ(run_img(img, len, &ret, &err), 0);
    T_EQ(ret, 7);
    free(img);
}

static void test_field_index_out_of_range_traps(void) {
    uint32_t code[] = {
        wo_ins_abx(WOP_NEW, 0, 0),
        wo_ins_abc(WOP_GETF, 2, 0, 9), /* Point has 2 fields */
        wo_ins_abc(WOP_RET, 2, 0, 0),
    };
    size_t len;
    uint8_t *img = point_image(code, 3, &len);
    uint64_t ret = 0;
    wo_err err = {0};
    T_EQ(run_img(img, len, &ret, &err), -1);
    T_EQ(err.code, WO_T_BOUNDS);
    T_EQ(wo_vm_depth(&VM), 0);
    free(img);
}

static void test_null_receiver_traps(void) {
    uint32_t code[] = {
        /* r0 was never assigned: zeroed by the frame setup */
        wo_ins_abc(WOP_GETF, 2, 0, 0),
        wo_ins_abc(WOP_RET, 2, 0, 0),
    };
    size_t len;
    uint8_t *img = point_image(code, 2, &len);
    uint64_t ret = 0;
    wo_err err = {0};
    T_EQ(run_img(img, len, &ret, &err), -1);
    T_EQ(err.code, WO_T_BOUNDS);
    free(img);
}

int main(void) {
    test_new_set_get_drop_roundtrip();
    test_field_index_out_of_range_traps();
    test_null_receiver_traps();
    return t_report("test_objops");
}

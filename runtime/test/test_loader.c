/* test_loader — happy path from a builder image, then a rejection battery.
 * Every reject must produce a nonempty error and leak nothing (ASan). */
#include <stdlib.h>

#include "loader.h"
#include "t.h"
#include "wob_build.h"

/* a valid two-const, one-class, one-method image */
static uint8_t *valid_image(size_t *len) {
    wb_t *b = wb_new();
    uint32_t k42 = wb_const_int(b, 42);
    uint32_t kname = wb_const_text(b, "main");
    uint8_t kinds[] = {WO_K_SCALAR, WO_K_SCALAR};
    wb_class(b, kname, 0, kinds, 2);
    uint32_t code[] = {
        wo_ins_abx(WOP_LOADK, 0, (uint16_t)k42),
        wo_ins_abc(WOP_RET, 0, 0, 0),
    };
    uint32_t lines[] = {0, 10, 1, 11};
    uint32_t m = wb_method(b, kname, WOB_NONE, 0, 2, code, 2, lines, 2, NULL, 0);
    wb_entry(b, m);
    return wb_finish(b, len);
}

static void test_happy_path(void) {
    size_t len;
    uint8_t *img = valid_image(&len);
    wo_module m;
    char err[256] = "";
    T_EQ(wo_load_buf(&m, img, len, err, sizeof err), 0);
    T_EQ(m.const_cnt, 2);
    T_EQ(m.consts[0].tag, WOB_K_INT);
    T_EQ(m.consts[0].i, 42);
    T_EQ(m.consts[1].tag, WOB_K_TEXT);
    T_CHECK(m.consts[1].s != NULL);
    T_CHECK(m.consts[1].s->h.flags & WO_F_CONST); /* interned */
    T_EQ(m.consts[1].s->len, 4);
    T_CHECK(memcmp(m.consts[1].s->data, "main", 4) == 0);
    T_EQ(m.class_cnt, 1);
    T_EQ(m.classes[0].field_cnt, 2);
    T_EQ(m.method_cnt, 1);
    T_EQ(m.methods[0].reg_cnt, 2);
    T_EQ(m.methods[0].ninstr, 2);
    T_EQ(m.methods[0].line_cnt, 2);
    T_EQ(m.methods[0].lines[1].pc, 1);
    T_EQ(m.methods[0].lines[1].line, 11);
    T_EQ(m.entry, 0);
    wo_module_free(&m);
    free(img);
}

/* build an image with one patched method-code instruction */
static uint8_t *image_with_code(uint32_t i0, uint32_t i1, size_t *len) {
    wb_t *b = wb_new();
    wb_const_int(b, 42);
    uint32_t kname = wb_const_text(b, "main");
    uint32_t code[] = {i0, i1};
    wb_method(b, kname, WOB_NONE, 0, 2, code, 2, NULL, 0, NULL, 0);
    return wb_finish(b, len);
}

static void expect_reject(uint8_t *img, size_t len, const char *what) {
    wo_module m;
    char err[256] = "";
    int rc = wo_load_buf(&m, img, len, err, sizeof err);
    T_EQ(rc, -1);
    if (rc != 0 && err[0] == '\0')
        fprintf(stderr, "empty error for reject case: %s\n", what);
    T_CHECK(err[0] != '\0');
}

static void test_rejects(void) {
    size_t len;

    /* corrupted magic */
    uint8_t *img = valid_image(&len);
    img[0] ^= 0xFF;
    expect_reject(img, len, "magic");
    free(img);

    /* unknown opcode */
    img = image_with_code(wo_ins_abc(200, 0, 0, 0),
                          wo_ins_abc(WOP_RET, 0, 0, 0), &len);
    expect_reject(img, len, "opcode");
    free(img);

    /* constant index out of range */
    img = image_with_code(wo_ins_abx(WOP_LOADK, 0, 99),
                          wo_ins_abc(WOP_RET, 0, 0, 0), &len);
    expect_reject(img, len, "const oob");
    free(img);

    /* register out of range (regc = 2) */
    img = image_with_code(wo_ins_abc(WOP_MOVE, 5, 0, 0),
                          wo_ins_abc(WOP_RET, 0, 0, 0), &len);
    expect_reject(img, len, "reg oob");
    free(img);

    /* last instruction is not a terminator */
    img = image_with_code(wo_ins_abc(WOP_RET, 0, 0, 0),
                          wo_ins_abx(WOP_LOADK, 0, 0), &len);
    expect_reject(img, len, "non-terminator tail");
    free(img);

    /* truncated buffer */
    img = valid_image(&len);
    expect_reject(img, len / 2, "truncated");
    free(img);
}

/* a valid one-class image whose class descriptor carries `flags` */
static uint8_t *image_with_class_flags(uint32_t flags, size_t *len) {
    wb_t *b = wb_new();
    wb_const_int(b, 42);
    uint32_t kname = wb_const_text(b, "main");
    uint8_t kinds[] = {WO_K_SCALAR, WO_K_SCALAR};
    wb_class(b, kname, flags, kinds, 2);
    uint32_t code[] = {wo_ins_abc(WOP_RET, 0, 0, 0)};
    wb_method(b, kname, WOB_NONE, 0, 1, code, 1, NULL, 0, NULL, 0);
    return wb_finish(b, len);
}

/* databasev2 2 (6a, .wob v8): the storage bits describe a @table's rows, so
 * they are meaningless — and refused — on a class without WO_CLASSF_TABLE.
 * woc never emits the combination; the loader refuses it independently. */
static void test_storage_flags_need_table(void) {
    size_t len;
    uint8_t *img = image_with_class_flags(WO_CLASSF_VOLATILE, &len);
    expect_reject(img, len, "volatile without table");
    free(img);

    img = image_with_class_flags(WO_CLASSF_RESIDENT_KEYS, &len);
    expect_reject(img, len, "resident:keys without table");
    free(img);

    /* the same bits WITH the table bit load */
    img = image_with_class_flags(WO_CLASSF_TABLE | WO_CLASSF_VOLATILE, &len);
    wo_module m;
    char err[256] = "";
    T_EQ(wo_load_buf(&m, img, len, err, sizeof err), 0);
    T_EQ(m.classes[0].flags, WO_CLASSF_TABLE | WO_CLASSF_VOLATILE);
    wo_module_free(&m);
    free(img);
}

int main(void) {
    test_happy_path();
    test_rejects();
    test_storage_flags_need_table();
    return t_report("test_loader");
}

/* test_wobbuild — the in-memory .wob assembler emits exactly the format:
 * raw header bytes, section offsets, first constant at its stated offset. */
#include <stdlib.h>

#include "t.h"
#include "wob_build.h"

static uint32_t rd_u32(const uint8_t *p, size_t off) {
    uint32_t v;
    memcpy(&v, p + off, 4);
    return v;
}

int main(void) {
    wb_t *b = wb_new();
    uint32_t k_int = wb_const_int(b, 42);
    uint32_t k_name = wb_const_text(b, "main");
    T_EQ(k_int, 0);
    T_EQ(k_name, 1);

    uint32_t code[] = {
        wo_ins_abx(WOP_LOADK, 0, (uint16_t)k_int),
        wo_ins_abc(WOP_RET, 0, 0, 0),
    };
    uint32_t m = wb_method(b, k_name, WOB_NONE, 0, 1, code, 2, NULL, 0, NULL, 0);
    T_EQ(m, 0);
    wb_entry(b, m);

    size_t len = 0;
    uint8_t *img = wb_finish(b, &len);
    T_CHECK(img != NULL);
    T_CHECK(len > WOB_HDR_SIZE);

    /* header */
    T_EQ(rd_u32(img, WOB_OFF_MAGIC), WOB_MAGIC);
    T_EQ(rd_u32(img, WOB_OFF_VERSION), WOB_VERSION);
    uint32_t const_off = rd_u32(img, WOB_OFF_CONST);
    T_EQ(const_off, WOB_HDR_SIZE); /* constants sit right after the header */
    T_EQ(rd_u32(img, WOB_OFF_CONST + 4), 2); /* const count */
    T_EQ(rd_u32(img, WOB_OFF_CLASS + 4), 0);
    T_EQ(rd_u32(img, WOB_OFF_IFACE + 4), 0);
    T_EQ(rd_u32(img, WOB_OFF_METHOD + 4), 1);
    T_EQ(rd_u32(img, WOB_OFF_ENTRY), 0);

    /* first constant: tag 0 (int), i64 42 */
    T_EQ(img[const_off], WOB_K_INT);
    int64_t iv;
    memcpy(&iv, img + const_off + 1, 8);
    T_EQ(iv, 42);

    /* second constant: tag 1 (text), len 4, "main" */
    size_t t2 = const_off + 1 + 8;
    T_EQ(img[t2], WOB_K_TEXT);
    T_EQ(rd_u32(img, t2 + 1), 4);
    T_CHECK(memcmp(img + t2 + 5, "main", 4) == 0);

    /* method section: name, class, argc/regc, code length */
    uint32_t moff = rd_u32(img, WOB_OFF_METHOD);
    T_EQ(rd_u32(img, moff), k_name);
    T_EQ(rd_u32(img, moff + 4), WOB_NONE);
    T_EQ(img[moff + 8], 0); /* argc */
    T_EQ(img[moff + 9], 1); /* regc */
    T_EQ(rd_u32(img, moff + 12), 8); /* code_len bytes */
    T_EQ(rd_u32(img, moff + 16), code[0]);
    T_EQ(rd_u32(img, moff + 20), code[1]);

    free(img);
    return t_report("test_wobbuild");
}

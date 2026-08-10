/* test_builtin — builtins driven from bytecode: print output captured and
 * diffed (the same observation seam the conformance corpus will use),
 * containers, words, missing-key and empty-latest traps, DB_STUB, and the
 * text ops CONCAT/EQS. */
#include <stdio.h>
#include <stdlib.h>

#include "loader.h"
#include "t.h"
#include "vm.h"
#include "wob_build.h"

static wo_vm VM;
static wo_module MOD;

/* module with one method per scenario; built once */
enum { M_PRINT, M_MULTI, M_MAP, M_KEY, M_LATEST, M_WORDS, M_DB, M_CONCAT };

static uint8_t *build_module(size_t *len) {
    wb_t *b = wb_new();
    uint32_t khi = wb_const_text(b, "hi");     /* 0 */
    uint32_t k5 = wb_const_int(b, 5);          /* 1 */
    uint32_t k9 = wb_const_int(b, 9);          /* 2 */
    uint32_t k100 = wb_const_int(b, 100);      /* 3 */
    uint32_t k1 = wb_const_int(b, 1);          /* 4 */
    uint32_t kwords = wb_const_text(b, " two words \t here\n"); /* 5 */
    uint32_t kab = wb_const_text(b, "ab");     /* 6 */
    uint32_t kcd = wb_const_text(b, "cd");     /* 7 */
    uint32_t kabcd = wb_const_text(b, "abcd"); /* 8 */
    uint32_t kn = wb_const_text(b, "m");       /* shared method name */

    { /* M_PRINT: print "hi", print_int 5 */
        uint32_t code[] = {
            wo_ins_abx(WOP_LOADK, 1, (uint16_t)khi),
            wo_ins_abc(WOP_BUILTIN, 0, 1, WO_B_PRINT),
            wo_ins_abx(WOP_LOADK, 1, (uint16_t)k5),
            wo_ins_abc(WOP_BUILTIN, 0, 1, WO_B_PRINT_INT),
            wo_ins_abc(WOP_RET0, 0, 0, 0),
        };
        wb_method(b, kn, WOB_NONE, 0, 2, code, 5, NULL, 0, NULL, 0);
    }
    { /* M_MULTI: push 5, push 9 -> count*100 + latest = 209 */
        uint32_t code[] = {
            wo_ins_abc(WOP_BUILTIN, 0, WO_K_SCALAR, WO_B_MULTI_NEW),
            wo_ins_abx(WOP_LOADK, 1, (uint16_t)k5),
            wo_ins_abc(WOP_BUILTIN, 2, 0, WO_B_MULTI_PUSH),
            wo_ins_abx(WOP_LOADK, 1, (uint16_t)k9),
            wo_ins_abc(WOP_BUILTIN, 2, 0, WO_B_MULTI_PUSH),
            wo_ins_abc(WOP_BUILTIN, 3, 0, WO_B_COUNT),
            wo_ins_abc(WOP_BUILTIN, 4, 0, WO_B_LATEST),
            wo_ins_abx(WOP_LOADK, 1, (uint16_t)k100),
            wo_ins_abc(WOP_MUL, 3, 3, 1),
            wo_ins_abc(WOP_ADD, 3, 3, 4),
            wo_ins_abc(WOP_DROP, 0, 0, 0),
            wo_ins_abc(WOP_RET, 3, 0, 0),
        };
        wb_method(b, kn, WOB_NONE, 0, 5, code, 12, NULL, 0, NULL, 0);
    }
    { /* M_MAP: set 1->5; get(1)*10 + has(1)*100 + has(9) = 150 */
        uint32_t code[] = {
            wo_ins_abc(WOP_BUILTIN, 0, 0 /* scalar/scalar nibbles */,
                       WO_B_MAP_NEW),
            wo_ins_abx(WOP_LOADK, 1, (uint16_t)k1),
            wo_ins_abx(WOP_LOADK, 2, (uint16_t)k5),
            wo_ins_abc(WOP_BUILTIN, 3, 0, WO_B_MAP_SET),
            wo_ins_abc(WOP_BUILTIN, 3, 0, WO_B_MAP_GET), /* r3 = 5 */
            wo_ins_abc(WOP_BUILTIN, 4, 0, WO_B_MAP_HAS), /* r4 = has(1) = 1 */
            wo_ins_abx(WOP_LOADK, 5, (uint16_t)k100),
            wo_ins_abc(WOP_MUL, 4, 4, 5),
            wo_ins_abx(WOP_LOADK, 5, (uint16_t)k9),
            wo_ins_abc(WOP_MOVE, 1, 5, 0),
            wo_ins_abc(WOP_BUILTIN, 5, 0, WO_B_MAP_HAS), /* r5 = has(9) = 0 */
            wo_ins_abx(WOP_LOADK, 6, (uint16_t)k5),
            /* r3 = r3*10: 10 not in pool — use 5+5 */
            wo_ins_abc(WOP_ADD, 6, 6, 6),
            wo_ins_abc(WOP_MUL, 3, 3, 6),
            wo_ins_abc(WOP_ADD, 3, 3, 4),
            wo_ins_abc(WOP_ADD, 3, 3, 5),
            wo_ins_abc(WOP_DROP, 0, 0, 0),
            wo_ins_abc(WOP_RET, 3, 0, 0),
        };
        wb_method(b, kn, WOB_NONE, 0, 7, code, 18, NULL, 0, NULL, 0);
    }
    { /* M_KEY: get of a missing key traps KEY */
        uint32_t code[] = {
            wo_ins_abc(WOP_BUILTIN, 0, 0, WO_B_MAP_NEW),
            wo_ins_abx(WOP_LOADK, 1, (uint16_t)k1),
            wo_ins_abc(WOP_BUILTIN, 2, 0, WO_B_MAP_GET),
            wo_ins_abc(WOP_RET0, 0, 0, 0),
        };
        wb_drop drops[] = {{.pc = 1, .owned = 1u << 0, .gc = 0}};
        wb_method(b, kn, WOB_NONE, 0, 3, code, 4, NULL, 0, drops, 1);
    }
    { /* M_LATEST: latest of an empty multi traps BOUNDS */
        uint32_t code[] = {
            wo_ins_abc(WOP_BUILTIN, 0, WO_K_SCALAR, WO_B_MULTI_NEW),
            wo_ins_abc(WOP_BUILTIN, 1, 0, WO_B_LATEST),
            wo_ins_abc(WOP_RET0, 0, 0, 0),
        };
        wb_drop drops[] = {{.pc = 1, .owned = 1u << 0, .gc = 0}};
        wb_method(b, kn, WOB_NONE, 0, 2, code, 3, NULL, 0, drops, 1);
    }
    { /* M_WORDS: words(" two words \t here\n") = 3 */
        uint32_t code[] = {
            wo_ins_abx(WOP_LOADK, 1, (uint16_t)kwords),
            wo_ins_abc(WOP_BUILTIN, 0, 1, WO_B_WORDS),
            wo_ins_abc(WOP_RET, 0, 0, 0),
        };
        wb_method(b, kn, WOB_NONE, 0, 2, code, 3, NULL, 0, NULL, 0);
    }
    { /* M_DB: DB_STUB traps DB */
        uint32_t code[] = {wo_ins_abc(WOP_DB_STUB, 0, 0, 0)};
        wb_method(b, kn, WOB_NONE, 0, 1, code, 1, NULL, 0, NULL, 0);
    }
    { /* M_CONCAT: "ab"+"cd" == "abcd" -> 1 */
        uint32_t code[] = {
            wo_ins_abx(WOP_LOADK, 1, (uint16_t)kab),
            wo_ins_abx(WOP_LOADK, 2, (uint16_t)kcd),
            wo_ins_abc(WOP_CONCAT, 3, 1, 2),
            wo_ins_abx(WOP_LOADK, 4, (uint16_t)kabcd),
            wo_ins_abc(WOP_EQS, 5, 3, 4),
            wo_ins_abc(WOP_DROP, 3, 0, 0),
            wo_ins_abc(WOP_RET, 5, 0, 0),
        };
        wb_method(b, kn, WOB_NONE, 0, 6, code, 7, NULL, 0, NULL, 0);
    }
    return wb_finish(b, len);
}

static int call(uint32_t mi, uint64_t *ret, wo_err *err, FILE *cap) {
    T_EQ(wo_vm_init(&VM, &MOD, 1 << 20), 0);
    if (cap) VM.rt.out = cap;
    int rc = wo_vm_call(&VM, mi, NULL, 0, ret, err);
    wo_vm_destroy(&VM);
    return rc;
}

int main(void) {
    size_t len;
    uint8_t *img = build_module(&len);
    char lerr[256];
    if (wo_load_buf(&MOD, img, len, lerr, sizeof lerr) != 0) {
        fprintf(stderr, "loader rejected: %s\n", lerr);
        return 1;
    }
    uint64_t ret;
    wo_err err;

    /* print output captured and diffed */
    FILE *cap = tmpfile();
    T_EQ(call(M_PRINT, &ret, &err, cap), 0);
    fflush(cap);
    rewind(cap);
    char out[64] = {0};
    size_t n = fread(out, 1, sizeof out - 1, cap);
    (void)n;
    T_STREQ(out, "hi\n5\n");
    fclose(cap);

    T_EQ(call(M_MULTI, &ret, &err, NULL), 0);
    T_EQ(ret, 209);

    T_EQ(call(M_MAP, &ret, &err, NULL), 0);
    T_EQ(ret, 150);

    T_EQ(call(M_KEY, &ret, &err, NULL), -1);
    T_EQ(err.code, WO_T_KEY);

    T_EQ(call(M_LATEST, &ret, &err, NULL), -1);
    T_EQ(err.code, WO_T_BOUNDS);

    T_EQ(call(M_WORDS, &ret, &err, NULL), 0);
    T_EQ(ret, 3);

    T_EQ(call(M_DB, &ret, &err, NULL), -1);
    T_EQ(err.code, WO_T_DB);
    T_STREQ(err.msg, "engine not linked");

    T_EQ(call(M_CONCAT, &ret, &err, NULL), 0);
    T_EQ(ret, 1);

    wo_module_free(&MOD);
    free(img);
    return t_report("test_builtin");
}

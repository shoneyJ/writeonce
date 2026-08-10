/* mkwob — fixture generator for the CLI smoke test, built from the
 * test-side assembler: writes <outdir>/hello.wob and <outdir>/trap.wob. */
#include <stdio.h>
#include <stdlib.h>

#include "wob_build.h"

static int write_img(const char *dir, const char *name, uint8_t *img,
                     size_t len) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "mkwob: cannot write %s\n", path);
        return -1;
    }
    size_t n = fwrite(img, 1, len, f);
    fclose(f);
    free(img);
    return n == len ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: mkwob <outdir>\n");
        return 2;
    }
    size_t len;

    /* hello.wob: print "hello, wovm", print_int 42 */
    wb_t *b = wb_new();
    uint32_t kh = wb_const_text(b, "hello, wovm");
    uint32_t k42 = wb_const_int(b, 42);
    uint32_t kn = wb_const_text(b, "main");
    uint32_t hello[] = {
        wo_ins_abx(WOP_LOADK, 1, (uint16_t)kh),
        wo_ins_abc(WOP_BUILTIN, 0, 1, WO_B_PRINT),
        wo_ins_abx(WOP_LOADK, 1, (uint16_t)k42),
        wo_ins_abc(WOP_BUILTIN, 0, 1, WO_B_PRINT_INT),
        wo_ins_abc(WOP_RET0, 0, 0, 0),
    };
    wb_entry(b, wb_method(b, kn, WOB_NONE, 0, 2, hello, 5, NULL, 0, NULL, 0));
    uint8_t *img = wb_finish(b, &len);
    if (write_img(argv[1], "hello.wob", img, len) != 0) return 1;

    /* trap.wob: division by zero at source line 7 */
    b = wb_new();
    uint32_t k1 = wb_const_int(b, 1);
    uint32_t k0 = wb_const_int(b, 0);
    kn = wb_const_text(b, "main");
    uint32_t trap[] = {
        wo_ins_abx(WOP_LOADK, 0, (uint16_t)k1),
        wo_ins_abx(WOP_LOADK, 1, (uint16_t)k0),
        wo_ins_abc(WOP_DIV, 2, 0, 1),
        wo_ins_abc(WOP_RET, 2, 0, 0),
    };
    uint32_t lines[] = {0, 5, 2, 7};
    wb_entry(b, wb_method(b, kn, WOB_NONE, 0, 3, trap, 4, lines, 2, NULL, 0));
    img = wb_finish(b, &len);
    if (write_img(argv[1], "trap.wob", img, len) != 0) return 1;
    return 0;
}

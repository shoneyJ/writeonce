#include "wob_build.h"

#include <stdlib.h>
#include <string.h>

/* growable byte buffer */
typedef struct {
    uint8_t *p;
    size_t len, cap;
} buf_t;

static void put(buf_t *b, const void *src, size_t n) {
    if (n == 0) return;
    if (b->len + n > b->cap) {
        size_t ncap = b->cap ? b->cap : 64;
        while (ncap < b->len + n) ncap *= 2;
        b->p = realloc(b->p, ncap); /* test helper: abort-on-OOM is fine */
        b->cap = ncap;
    }
    memcpy(b->p + b->len, src, n);
    b->len += n;
}
static void put_u8(buf_t *b, uint8_t v) { put(b, &v, 1); }
static void put_u16(buf_t *b, uint16_t v) { put(b, &v, 2); }
static void put_u32(buf_t *b, uint32_t v) { put(b, &v, 4); }
static void put_u64(buf_t *b, uint64_t v) { put(b, &v, 8); }

struct wb_t {
    buf_t consts, classes, ifaces, vtabs, methods;
    uint32_t const_cnt, class_cnt, iface_cnt, vtab_cnt, method_cnt;
    uint32_t next_slot; /* global interface slot ids accumulate */
    uint32_t entry;
};

wb_t *wb_new(void) {
    wb_t *b = calloc(1, sizeof(wb_t));
    b->entry = WOB_NONE;
    return b;
}

uint32_t wb_const_int(wb_t *b, int64_t v) {
    put_u8(&b->consts, WOB_K_INT);
    put_u64(&b->consts, (uint64_t)v);
    return b->const_cnt++;
}

uint32_t wb_const_text(wb_t *b, const char *s) {
    uint32_t len = (uint32_t)strlen(s);
    put_u8(&b->consts, WOB_K_TEXT);
    put_u32(&b->consts, len);
    put(&b->consts, s, len);
    return b->const_cnt++;
}

uint32_t wb_class(wb_t *b, uint32_t name_const, uint32_t flags,
                  const uint8_t *kinds, uint32_t field_cnt) {
    put_u32(&b->classes, name_const);
    put_u32(&b->classes, flags);
    put_u32(&b->classes, field_cnt);
    put(&b->classes, kinds, field_cnt);
    for (uint32_t pad = field_cnt; pad % 4; pad++) put_u8(&b->classes, 0);
    /* v2 per-field metadata (wob.h): a hand-built image records no field
       names and no referenced classes — WOB_NONE reads as "not recorded",
       which every consumer but json.encode/decode ignores. */
    for (uint32_t j = 0; j < field_cnt; j++) put_u32(&b->classes, WOB_NONE);
    for (uint32_t j = 0; j < field_cnt; j++) put_u32(&b->classes, WOB_NONE);
    for (uint32_t j = 0; j < field_cnt; j++) put_u32(&b->classes, 0);
    put_u32(&b->classes, 0); /* v3: no secondary indexes in hand-built images */
    return b->class_cnt++;
}

uint32_t wb_iface(wb_t *b, uint32_t name_const, uint32_t method_cnt) {
    put_u32(&b->ifaces, name_const);
    put_u32(&b->ifaces, method_cnt);
    b->next_slot += method_cnt;
    return b->iface_cnt++;
}

void wb_vtab(wb_t *b, uint32_t class_id, uint32_t iface_id,
             const uint32_t *methods, uint32_t method_cnt) {
    put_u32(&b->vtabs, class_id);
    put_u32(&b->vtabs, iface_id);
    for (uint32_t i = 0; i < method_cnt; i++) put_u32(&b->vtabs, methods[i]);
    b->vtab_cnt++;
}

uint32_t wb_method(wb_t *b, uint32_t name_const, uint32_t class_id,
                   uint8_t argc, uint8_t regc, const uint32_t *code,
                   uint32_t ninstr, const uint32_t *lines, uint32_t nlines,
                   const wb_drop *drops, uint32_t ndrops) {
    buf_t *m = &b->methods;
    put_u32(m, name_const);
    put_u32(m, class_id);
    put_u8(m, argc);
    put_u8(m, regc);
    put_u16(m, 0); /* reserved */
    put_u32(m, ninstr * 4u);
    for (uint32_t i = 0; i < ninstr; i++) put_u32(m, code[i]);
    put_u32(m, nlines);
    for (uint32_t i = 0; i < 2 * nlines; i++) put_u32(m, lines[i]);
    put_u32(m, ndrops);
    for (uint32_t i = 0; i < ndrops; i++) {
        put_u32(m, drops[i].pc);
        put_u64(m, drops[i].owned);
        put_u64(m, drops[i].gc);
    }
    return b->method_cnt++;
}

void wb_entry(wb_t *b, uint32_t method_idx) { b->entry = method_idx; }

uint8_t *wb_finish(wb_t *b, size_t *len) {
    /* interface section = interface entries, then vtab count, then rows */
    buf_t iface_all = {0};
    put(&iface_all, b->ifaces.p ? (void *)b->ifaces.p : (void *)"", b->ifaces.len);
    put_u32(&iface_all, b->vtab_cnt);
    if (b->vtabs.len) put(&iface_all, b->vtabs.p, b->vtabs.len);

    buf_t out = {0};
    uint32_t off = WOB_HDR_SIZE;
    put_u32(&out, WOB_MAGIC);
    put_u32(&out, WOB_VERSION);
    put_u32(&out, off);
    put_u32(&out, b->const_cnt);
    off += (uint32_t)b->consts.len;
    put_u32(&out, off);
    put_u32(&out, b->class_cnt);
    off += (uint32_t)b->classes.len;
    put_u32(&out, off);
    put_u32(&out, b->iface_cnt);
    off += (uint32_t)iface_all.len;
    put_u32(&out, off);
    put_u32(&out, b->method_cnt);
    put_u32(&out, b->entry);
    if (b->consts.len) put(&out, b->consts.p, b->consts.len);
    if (b->classes.len) put(&out, b->classes.p, b->classes.len);
    put(&out, iface_all.p, iface_all.len);
    if (b->methods.len) put(&out, b->methods.p, b->methods.len);

    free(b->consts.p);
    free(b->classes.p);
    free(b->ifaces.p);
    free(b->vtabs.p);
    free(b->methods.p);
    free(iface_all.p);
    uint8_t *img = out.p;
    *len = out.len;
    free(b);
    return img;
}

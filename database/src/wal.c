/* pread/pwrite/fdatasync/posix_fallocate under -std=c11 */
#define _POSIX_C_SOURCE 200809L

#include "wal.h"

#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- crc32 (poly 0xEDB88320) — ported from runtime/wo-rt.c ------------- */

static uint32_t crc_table[256];
static int crc_ready;

static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    crc_ready = 1;
}

static uint32_t crc32(const void *buf, size_t len) {
    if (!crc_ready) crc32_init();
    const uint8_t *p = buf;
    uint32_t c = 0xFFFFFFFFu;
    while (len--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---- byte buffer -------------------------------------------------------- */

typedef struct {
    uint8_t *b;
    size_t len, cap;
    int oom;
} wbuf;

static void wput(wbuf *w, const void *p, size_t n) {
    if (w->oom) return;
    if (w->len + n > w->cap) {
        size_t nc = w->cap ? w->cap * 2 : 256;
        while (nc < w->len + n) nc *= 2;
        uint8_t *nb = realloc(w->b, nc);
        if (!nb) {
            w->oom = 1;
            return;
        }
        w->b = nb;
        w->cap = nc;
    }
    memcpy(w->b + w->len, p, n);
    w->len += n;
}

static void wput_u8(wbuf *w, uint8_t v) { wput(w, &v, 1); }
static void wput_u32(wbuf *w, uint32_t v) { wput(w, &v, 4); }
static void wput_u64(wbuf *w, uint64_t v) { wput(w, &v, 8); }

/* bounds-checked reader */
typedef struct {
    const uint8_t *p, *end;
    int bad;
} rbuf;

static int rtake(rbuf *r, void *out, size_t n) {
    if (r->bad || (size_t)(r->end - r->p) < n) {
        r->bad = 1;
        return -1;
    }
    memcpy(out, r->p, n);
    r->p += n;
    return 0;
}

static uint8_t rd_u8(rbuf *r) {
    uint8_t v = 0;
    rtake(r, &v, 1);
    return v;
}
static uint32_t rd_u32(rbuf *r) {
    uint32_t v = 0;
    rtake(r, &v, 4);
    return v;
}
static uint64_t rd_u64(rbuf *r) {
    uint64_t v = 0;
    rtake(r, &v, 8);
    return v;
}

#define WAL_NIL_TEXT 0xFFFFFFFFu

/* ---- engine-value <-> bytes (kind-driven, mirrors table.c's encoding) --- */

static void enc_val(wbuf *w, const wo_classdesc *classes, uint8_t kind, uint64_t v) {
    switch (kind) {
    /* iteration 19: a Float is one word on the wire, its raw IEEE bits — no
       decimal rendering anywhere in the durability path, so replay is
       bit-exact for NaN, the infinities, and -0.0 alike. A Bytes is the same
       length-prefixed blob a Text is; the class table's kind byte is what
       says which one comes back out. */
    case WO_K_SCALAR:
    case WO_K_FLOAT: wput_u64(w, v); return;
    case WO_K_TEXT:
    case WO_K_BYTES: {
        if (!v) {
            wput_u32(w, WAL_NIL_TEXT);
            return;
        }
        const db_text *t = (const db_text *)(uintptr_t)v;
        wput_u32(w, t->len);
        wput(w, t->bytes, t->len);
        return;
    }
    case WO_K_OWNED: {
        if (!v) {
            wput_u8(w, 0);
            return;
        }
        const db_rec *r = (const db_rec *)(uintptr_t)v;
        wput_u8(w, 1);
        wput_u32(w, r->class_id);
        const wo_classdesc *c = &classes[r->class_id];
        for (uint32_t i = 0; i < c->field_cnt; i++)
            enc_val(w, classes, c->kinds[i], r->slots[i]);
        return;
    }
    case WO_K_MULTI: {
        if (!v) {
            wput_u8(w, 0);
            return;
        }
        const db_multi *m = (const db_multi *)(uintptr_t)v;
        wput_u8(w, 1);
        wput_u8(w, m->elem_kind);
        wput_u32(w, m->len);
        for (uint32_t i = 0; i < m->len; i++) enc_val(w, classes, m->elem_kind, m->items[i]);
        return;
    }
    case WO_K_MAP: {
        if (!v) {
            wput_u8(w, 0);
            return;
        }
        const db_map *m = (const db_map *)(uintptr_t)v;
        wput_u8(w, 1);
        wput_u8(w, m->key_kind);
        wput_u8(w, m->val_kind);
        wput_u32(w, m->len);
        for (uint32_t i = 0; i < m->len; i++) {
            enc_val(w, classes, m->key_kind, m->kv[2 * i]);
            enc_val(w, classes, m->val_kind, m->kv[2 * i + 1]);
        }
        return;
    }
    default: return; /* GCREF never stored, so never logged */
    }
}

/* Decode one value into an engine-owned allocation. Returns 0 on success
 * with *out set (0 = genuine nil); -1 on truncation/corruption/OOM — the
 * caller frees what it already built. */
static int dec_val(rbuf *r, wo_db *db, uint8_t kind, uint64_t *out) {
    *out = 0;
    switch (kind) {
    case WO_K_SCALAR:
    case WO_K_FLOAT: { /* iteration 19: the same word back, uninterpreted */
        uint64_t v = rd_u64(r);
        if (r->bad) return -1;
        *out = v;
        return 0;
    }
    case WO_K_TEXT:
    case WO_K_BYTES: {
        uint32_t len = rd_u32(r);
        if (r->bad) return -1;
        if (len == WAL_NIL_TEXT) return 0;
        if ((size_t)(r->end - r->p) < len) return -1;
        db_text *t = malloc(sizeof(db_text) + len);
        if (!t) return -1;
        t->len = len;
        memcpy(t->bytes, r->p, len);
        r->p += len;
        *out = (uint64_t)(uintptr_t)t;
        return 0;
    }
    case WO_K_OWNED: {
        uint8_t tag = rd_u8(r);
        if (r->bad) return -1;
        if (!tag) return 0;
        uint32_t cid = rd_u32(r);
        if (r->bad || cid >= db->class_cnt) return -1;
        const wo_classdesc *c = &db->classes[cid];
        db_rec *rec = malloc(sizeof(db_rec) + (size_t)c->field_cnt * 8u);
        if (!rec) return -1;
        rec->class_id = cid;
        rec->_pad = 0;
        for (uint32_t i = 0; i < c->field_cnt; i++) {
            if (dec_val(r, db, c->kinds[i], &rec->slots[i]) != 0) {
                for (uint32_t j = 0; j < i; j++) wo_db_val_free(db, c->kinds[j], rec->slots[j]);
                free(rec);
                return -1;
            }
        }
        *out = (uint64_t)(uintptr_t)rec;
        return 0;
    }
    case WO_K_MULTI: {
        uint8_t tag = rd_u8(r);
        if (r->bad) return -1;
        if (!tag) return 0;
        uint8_t ek = rd_u8(r);
        uint32_t len = rd_u32(r);
        if (r->bad || ek > WO_K_MAX) return -1;
        if (len > (size_t)(r->end - r->p)) return -1; /* each elem >= 1 byte */
        db_multi *m = malloc(sizeof(db_multi) + (size_t)len * 8u);
        if (!m) return -1;
        m->elem_kind = ek;
        m->len = len;
        for (uint32_t i = 0; i < len; i++) {
            if (dec_val(r, db, ek, &m->items[i]) != 0) {
                for (uint32_t j = 0; j < i; j++) wo_db_val_free(db, ek, m->items[j]);
                free(m);
                return -1;
            }
        }
        *out = (uint64_t)(uintptr_t)m;
        return 0;
    }
    case WO_K_MAP: {
        uint8_t tag = rd_u8(r);
        if (r->bad) return -1;
        if (!tag) return 0;
        uint8_t kk = rd_u8(r), vk = rd_u8(r);
        uint32_t len = rd_u32(r);
        if (r->bad || kk > WO_K_MAX || vk > WO_K_MAX) return -1;
        if (len > (size_t)(r->end - r->p)) return -1;
        db_map *m = malloc(sizeof(db_map) + (size_t)len * 16u);
        if (!m) return -1;
        m->key_kind = kk;
        m->val_kind = vk;
        m->len = len;
        for (uint32_t i = 0; i < len; i++) {
            if (dec_val(r, db, kk, &m->kv[2 * i]) != 0 ||
                dec_val(r, db, vk, &m->kv[2 * i + 1]) != 0) {
                m->len = i; /* free only the fully-built pairs plus a possible key */
                for (uint32_t j = 0; j < i; j++) {
                    wo_db_val_free(db, kk, m->kv[2 * j]);
                    wo_db_val_free(db, vk, m->kv[2 * j + 1]);
                }
                wo_db_val_free(db, kk, m->kv[2 * i]); /* 0 if the key failed */
                free(m);
                return -1;
            }
        }
        *out = (uint64_t)(uintptr_t)m;
        return 0;
    }
    default: return -1;
    }
}

/* ---- record scan (shared by open, replay, check) ------------------------ */

/* Read the record at [off]. 0 = intact (*len_out = payload length, payload
 * malloc'd into *payload_out if non-NULL); 1 = end of intact prefix (zero
 * length, short read, bad crc, missing mark). */
static int scan_record(int fd, uint64_t off, uint32_t *len_out, uint8_t **payload_out) {
    uint8_t hdr[8];
    ssize_t n = pread(fd, hdr, 8, (off_t)off);
    if (n != 8) return 1;
    uint32_t len, crc;
    memcpy(&len, hdr, 4);
    memcpy(&crc, hdr + 4, 4);
    if (len == 0 || len > (64u << 20)) return 1; /* preallocated tail or garbage */
    uint8_t *payload = malloc(len + 4);
    if (!payload) return 1;
    n = pread(fd, payload, len + 4, (off_t)(off + 8));
    if (n != (ssize_t)(len + 4)) {
        free(payload);
        return 1;
    }
    uint32_t mark;
    memcpy(&mark, payload + len, 4);
    if (mark != WO_WAL_MARK || crc32(payload, len) != crc) {
        free(payload);
        return 1;
    }
    *len_out = len;
    if (payload_out) *payload_out = payload;
    else free(payload);
    return 0;
}

/* Task 4 follow-up (review finding): scan_record's exact framing, but read
 * from the STAGED bytes in [w->off, w->off + w->len) instead of the file.
 * A record staged behind this batch's own not-yet-run barrier is real,
 * fully-framed data sitting in `w->buf` — preading its file offset would
 * see whatever was on disk BEFORE this batch (the pre-allocated zero
 * tail, ordinarily), which scan_record correctly, but unhelpfully, reads
 * as "no record here yet". Used only by wo_wal_fold_row_at, and only for
 * an offset a caller got from wo_wal_repoint_offset1 (a PENDING re-point,
 * not yet flushed) — never for a durable offset, which stays on the
 * scan_record/pread path unchanged. 0 = intact, 1 = short/bad, same as
 * scan_record. */
static int scan_record_staged(const wo_wal *w, uint64_t off, uint32_t *len_out,
                              uint8_t **payload_out) {
    if (off < w->off) return 1;
    uint64_t rel = off - w->off;
    if (rel + 8 > w->len) return 1;
    uint32_t len, crc;
    memcpy(&len, w->buf + rel, 4);
    memcpy(&crc, w->buf + rel + 4, 4);
    if (len == 0 || len > (64u << 20)) return 1; /* garbage: not a real frame */
    if (rel + 8 + (uint64_t)len + 4 > w->len) return 1; /* short: not fully staged */
    const uint8_t *payload = w->buf + rel + 8;
    uint32_t mark;
    memcpy(&mark, payload + len, 4);
    if (mark != WO_WAL_MARK || crc32(payload, len) != crc) return 1;
    if (payload_out) {
        uint8_t *cp = malloc((size_t)len + 4);
        if (!cp) return 1;
        memcpy(cp, payload, (size_t)len + 4);
        *payload_out = cp;
    }
    *len_out = len;
    return 0;
}

/* ---- public API ---------------------------------------------------------- */

int wo_wal_open(wo_wal *w, const char *path, uint64_t prealloc) {
    memset(w, 0, sizeof(*w));
    w->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (w->fd < 0) return -1;
    w->path = strdup(path); /* NULL is tolerated: the diagnostic degrades */
    /* databasev2 3: remove a stale compaction temp before doing anything else.
     * The only way one exists is a crash before the rename, which means its
     * records were never authoritative — the live log below is the truth. It is
     * deleted rather than ignored because a file full of well-formed records
     * sitting beside the log is exactly the thing a future reader mistakes for
     * data. */
    {
        char tmp[4096];
        if ((size_t)snprintf(tmp, sizeof tmp, "%s%s", path, WO_WAL_TMP_SUFFIX) < sizeof tmp)
            (void)unlink(tmp);
    }
    w->prealloc = prealloc;
    if (prealloc) {
        /* best-effort: a filesystem without fallocate still works */
        (void)posix_fallocate(w->fd, 0, (off_t)prealloc);
    }
    /* position after the intact prefix: a torn tail is OVERWRITTEN by the
     * next append, never appended after */
    uint64_t off = 0;
    uint32_t len;
    while (scan_record(w->fd, off, &len, NULL) == 0) off += 8u + len + 4u;
    w->off = off;
    return 0;
}

int wo_wal_pend_drop(wo_wal *w, uint32_t cid, uint64_t id, uint64_t off) {
    if (w->pend_len == w->pend_cap) {
        size_t nc = w->pend_cap ? w->pend_cap * 2 : 16;
        struct wo_wal_pend *np = realloc(w->pend, nc * sizeof *np);
        if (!np) return -1; /* the row stays resident: safe, just not dropped */
        w->pend = np;
        w->pend_cap = nc;
    }
    w->pend[w->pend_len].cid = cid;
    w->pend[w->pend_len].id = id;
    w->pend[w->pend_len].off = off;
    w->pend_len++;
    return 0;
}

/* Task 4 (keys-resident delta updates): pend_drop's counterpart for a
 * re-point, own list, same reason (see the `repoint` field in wal.h). */
int wo_wal_pend_repoint(wo_wal *w, uint32_t cid, uint64_t id, uint64_t off) {
    if (w->repoint_len == w->repoint_cap) {
        size_t nc = w->repoint_cap ? w->repoint_cap * 2 : 16;
        struct wo_wal_pend *np = realloc(w->repoint, nc * sizeof *np);
        if (!np) return -1; /* the map stays where it was: see the header */
        w->repoint = np;
        w->repoint_cap = nc;
    }
    w->repoint[w->repoint_len].cid = cid;
    w->repoint[w->repoint_len].id = id;
    w->repoint[w->repoint_len].off = off;
    w->repoint_len++;
    return 0;
}

uint64_t wo_wal_repoint_offset1(const wo_wal *w, uint32_t cid, uint64_t id) {
    /* backward: the LATEST entry for (cid, id) is the one still current if
       this row was updated more than once behind the same barrier */
    for (size_t i = w->repoint_len; i > 0; i--) {
        if (w->repoint[i - 1].cid == cid && w->repoint[i - 1].id == id)
            return w->repoint[i - 1].off + 1;
    }
    return 0;
}

void wo_db_flush_drops(wo_db *db, wo_wal *w) {
    for (size_t i = 0; i < w->pend_len; i++)
        (void)wo_row_drop_payload(db, w->pend[i].cid, w->pend[i].id, w->pend[i].off);
    w->pend_len = 0;
    /* Task 4: the request path's deferred update re-points, held behind the
       same barrier as inserts' drops for the same reason — before the
       commit above, these offsets pread zeros. */
    for (size_t i = 0; i < w->repoint_len; i++)
        (void)wo_row_set_offset(db, w->repoint[i].cid, w->repoint[i].id, w->repoint[i].off);
    w->repoint_len = 0;
}

void wo_wal_close(wo_wal *w) {
    free(w->schema);
    w->schema = NULL;
    w->schema_len = 0;
    free(w->pend);
    free(w->repoint);
    if (w->fd >= 0) close(w->fd);
    free(w->path);
    free(w->buf);
    memset(w, 0, sizeof(*w));
    w->fd = -1;
}

/* frame one payload into the staged batch */
static int stage(wo_wal *w, const wbuf *payload) {
    if (payload->oom) return -1;
    wbuf rec = {0};
    wput_u32(&rec, (uint32_t)payload->len);
    wput_u32(&rec, crc32(payload->b, payload->len));
    wput(&rec, payload->b, payload->len);
    wput_u32(&rec, WO_WAL_MARK);
    if (rec.oom) {
        free(rec.b);
        return -1;
    }
    if (w->len + rec.len > w->cap) {
        size_t nc = w->cap ? w->cap * 2 : 4096;
        while (nc < w->len + rec.len) nc *= 2;
        uint8_t *nb = realloc(w->buf, nc);
        if (!nb) {
            free(rec.b);
            return -1;
        }
        w->buf = nb;
        w->cap = nc;
    }
    memcpy(w->buf + w->len, rec.b, rec.len);
    w->len += rec.len;
    free(rec.b);
    return 0;
}

/* ---- databasev2 12: the schema record -------------------------------- */

int wo_schema_encode(const wo_schema *sc, uint8_t **payload_out, uint32_t *len_out) {
    wbuf p = {0};
    wput_u8(&p, WO_WAL_SCHEMA);
    wput_u32(&p, sc->class_cnt);
    for (uint32_t c = 0; c < sc->class_cnt; c++) {
        const wo_schema_class *k = &sc->classes[c];
        wput_u32(&p, k->name_len);
        wput(&p, k->name, k->name_len);
        wput_u32(&p, k->flags);
        wput_u32(&p, k->field_cnt);
        for (uint32_t f = 0; f < k->field_cnt; f++) {
            const wo_schema_field *fl = &k->fields[f];
            wput_u32(&p, fl->name_len);
            wput(&p, fl->name, fl->name_len);
            wput_u8(&p, fl->kind);
            wput_u32(&p, fl->fclass);
            wput_u32(&p, fl->felem);
        }
    }
    if (p.oom) {
        free(p.b);
        return -1;
    }
    *payload_out = p.b;
    *len_out = (uint32_t)p.len;
    return 0;
}

wo_schema *wo_schema_decode(const uint8_t *payload, uint32_t len) {
    if (len < 5 || payload[0] != WO_WAL_SCHEMA) return NULL;
    /* names point into one private copy of the bytes, so the schema outlives
     * whatever buffer the caller hands in */
    wo_schema *sc = calloc(1, sizeof *sc);
    if (!sc) return NULL;
    sc->owned = malloc(len);
    if (!sc->owned) {
        free(sc);
        return NULL;
    }
    memcpy(sc->owned, payload, len);
    rbuf r = {sc->owned + 1, sc->owned + len, 0};
    sc->class_cnt = rd_u32(&r);
    if (r.bad || sc->class_cnt > 65536) goto bad;
    sc->classes = calloc(sc->class_cnt ? sc->class_cnt : 1, sizeof *sc->classes);
    if (!sc->classes) goto bad;
    for (uint32_t c = 0; c < sc->class_cnt; c++) {
        wo_schema_class *k = &sc->classes[c];
        k->name_len = rd_u32(&r);
        if (r.bad || (size_t)(r.end - r.p) < k->name_len) goto bad;
        k->name = r.p;
        r.p += k->name_len;
        k->flags = rd_u32(&r);
        k->field_cnt = rd_u32(&r);
        if (r.bad || k->field_cnt > 65536) goto bad;
        k->fields = calloc(k->field_cnt ? k->field_cnt : 1, sizeof *k->fields);
        if (!k->fields) goto bad;
        for (uint32_t f = 0; f < k->field_cnt; f++) {
            wo_schema_field *fl = &k->fields[f];
            fl->name_len = rd_u32(&r);
            if (r.bad || (size_t)(r.end - r.p) < fl->name_len) goto bad;
            fl->name = r.p;
            r.p += fl->name_len;
            fl->kind = rd_u8(&r);
            fl->fclass = rd_u32(&r);
            fl->felem = rd_u32(&r);
            if (r.bad) goto bad;
        }
    }
    if (r.p != r.end) goto bad; /* trailing bytes = malformed */
    return sc;
bad:
    wo_schema_free(sc);
    return NULL;
}

void wo_schema_free(wo_schema *sc) {
    if (!sc) return;
    if (sc->classes)
        for (uint32_t c = 0; c < sc->class_cnt; c++) free(sc->classes[c].fields);
    free(sc->classes);
    free(sc->owned);
    free(sc);
}

/* ---- databasev2 12: the boot diff ------------------------------------- */

static int sc_name_eq(const uint8_t *a, uint32_t al, const uint8_t *b, uint32_t bl) {
    return al == bl && (al == 0 || memcmp(a, b, al) == 0);
}
static uint32_t sc_find_class(const wo_schema *sc, const uint8_t *name, uint32_t len) {
    for (uint32_t c = 0; c < sc->class_cnt; c++)
        if (sc_name_eq(sc->classes[c].name, sc->classes[c].name_len, name, len)) return c;
    return WO_SCHEMA_NONE;
}
static uint32_t sc_find_field(const wo_schema_class *k, const uint8_t *name, uint32_t len) {
    for (uint32_t f = 0; f < k->field_cnt; f++)
        if (sc_name_eq(k->fields[f].name, k->fields[f].name_len, name, len)) return f;
    return WO_SCHEMA_NONE;
}
/* fclass words number classes in their OWN schema, so under reordering the
 * same referenced class carries different numbers — equality is by the NAME
 * the number resolves to, never the number itself */
static int sc_ref_eq(const wo_schema *osc, uint32_t ofc, const wo_schema *nsc, uint32_t nfc) {
    if (ofc == WO_SCHEMA_NONE || nfc == WO_SCHEMA_NONE) return ofc == nfc;
    if (ofc >= osc->class_cnt || nfc >= nsc->class_cnt) return 0;
    return sc_name_eq(osc->classes[ofc].name, osc->classes[ofc].name_len,
                      nsc->classes[nfc].name, nsc->classes[nfc].name_len);
}
static int sc_field_shape_eq(const wo_schema *osc, const wo_schema_field *of,
                             const wo_schema *nsc, const wo_schema_field *nf) {
    return of->kind == nf->kind && of->felem == nf->felem &&
           sc_ref_eq(osc, of->fclass, nsc, nf->fclass);
}
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
static char *sc_poisonf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return strdup(buf);
}

void wo_mig_plan_free(wo_mig_plan *plan) {
    if (!plan->classes) return;
    for (uint32_t c = 0; c < plan->old_class_cnt; c++) {
        free(plan->classes[c].poison);
        free(plan->classes[c].fmap);
    }
    free(plan->classes);
    plan->classes = NULL;
}

int wo_schema_diff(const wo_schema *osc, const wo_schema *nsc, wo_mig_plan *plan) {
    memset(plan, 0, sizeof *plan);
    plan->old_class_cnt = osc->class_cnt;
    plan->classes = calloc(osc->class_cnt ? osc->class_cnt : 1, sizeof *plan->classes);
    if (!plan->classes) return -1;

    for (uint32_t c = 0; c < osc->class_cnt; c++) {
        const wo_schema_class *ok = &osc->classes[c];
        wo_mig_class *mc = &plan->classes[c];
        mc->old_field_cnt = ok->field_cnt;
        mc->new_cid = sc_find_class(nsc, ok->name, ok->name_len);
        if (mc->new_cid == WO_SCHEMA_NONE) {
            mc->poison = sc_poisonf("class `%.*s` has stored rows this binary no "
                                    "longer declares — restore the class, or delete "
                                    "WO_DATA if the rows are expendable",
                                    (int)ok->name_len, (const char *)ok->name);
            continue;
        }
        const wo_schema_class *nk = &nsc->classes[mc->new_cid];
        if (ok->flags != nk->flags) {
            mc->new_cid = WO_SCHEMA_NONE;
            mc->poison = sc_poisonf("class `%.*s` changed its storage declaration "
                                    "(durable/resident) with rows in the log — v1 "
                                    "migrates fields, not storage modes",
                                    (int)ok->name_len, (const char *)ok->name);
            continue;
        }
        mc->fmap = malloc((ok->field_cnt ? ok->field_cnt : 1) * sizeof *mc->fmap);
        if (!mc->fmap) return -1;
        uint32_t deleted = 0;
        for (uint32_t f = 0; f < ok->field_cnt; f++) {
            const wo_schema_field *of = &ok->fields[f];
            uint32_t nf = sc_find_field(nk, of->name, of->name_len);
            if (nf == WO_SCHEMA_NONE) {
                mc->fmap[f] = -1;
                deleted++;
                continue;
            }
            if (!sc_field_shape_eq(osc, of, nsc, &nk->fields[nf])) {
                free(mc->fmap);
                mc->fmap = NULL;
                mc->new_cid = WO_SCHEMA_NONE;
                mc->poison = sc_poisonf("class `%.*s`: field `%.*s` changed its type "
                                        "— v1 has no conversions; add a new field "
                                        "and backfill instead",
                                        (int)ok->name_len, (const char *)ok->name,
                                        (int)of->name_len, (const char *)of->name);
                break;
            }
            mc->fmap[f] = (int32_t)nf;
        }
        if (!mc->fmap) continue; /* poisoned above */
        uint32_t added = 0;
        for (uint32_t f = 0; f < nk->field_cnt; f++)
            if (sc_find_field(ok, nk->fields[f].name, nk->fields[f].name_len) ==
                WO_SCHEMA_NONE)
                added++;
        mc->changed = (deleted > 0 || added > 0);
        /* a deleted and an added field of the SAME shape in one step is
         * byte-for-byte indistinguishable from a rename, and the two
         * readings differ by exactly one column of destroyed data */
        if (deleted && added) {
            for (uint32_t f = 0; f < ok->field_cnt && mc->fmap; f++) {
                if (mc->fmap[f] != -1) continue;
                for (uint32_t g = 0; g < nk->field_cnt; g++) {
                    if (sc_find_field(ok, nk->fields[g].name, nk->fields[g].name_len) !=
                        WO_SCHEMA_NONE)
                        continue;
                    if (sc_field_shape_eq(osc, &ok->fields[f], nsc, &nk->fields[g])) {
                        free(mc->fmap);
                        mc->fmap = NULL;
                        mc->new_cid = WO_SCHEMA_NONE;
                        mc->poison = sc_poisonf(
                            "class `%.*s`: `%.*s` was deleted and a field of the "
                            "same type added — a rename and a delete+add are "
                            "indistinguishable here and one of them destroys data. "
                            "Deploy the delete and the add as two separate steps",
                            (int)ok->name_len, (const char *)ok->name,
                            (int)ok->fields[f].name_len,
                            (const char *)ok->fields[f].name);
                        break;
                    }
                }
                if (!mc->fmap) break;
            }
        }
    }

    /* the embed closure: a class whose old records EMBED (owned or container
     * values of) a class whose shape changed cannot be transcoded — the
     * nested bytes are in the OLD sub-shape and v1 does not rewrite value
     * trees recursively. Iterate to a fixpoint so chains of embedding
     * poison through. */
    for (int again = 1; again;) {
        again = 0;
        for (uint32_t c = 0; c < osc->class_cnt; c++) {
            wo_mig_class *mc = &plan->classes[c];
            if (mc->poison) continue;
            for (uint32_t f = 0; f < osc->classes[c].field_cnt; f++) {
                const wo_schema_field *of = &osc->classes[c].fields[f];
                if (of->kind != WO_K_OWNED && of->kind != WO_K_MULTI &&
                    of->kind != WO_K_MAP)
                    continue;
                int tainted = 0;
                if (of->fclass != WO_SCHEMA_NONE && of->fclass < osc->class_cnt) {
                    const wo_mig_class *ref = &plan->classes[of->fclass];
                    tainted = ref->poison != NULL || ref->changed;
                } else if (of->kind == WO_K_OWNED) {
                    /* an owned field with no recorded target class: assume the
                     * worst whenever anything at all changed */
                    for (uint32_t x = 0; x < osc->class_cnt && !tainted; x++)
                        tainted = plan->classes[x].poison != NULL ||
                                  plan->classes[x].changed;
                }
                if (tainted) {
                    free(mc->fmap);
                    mc->fmap = NULL;
                    mc->new_cid = WO_SCHEMA_NONE;
                    mc->poison = sc_poisonf(
                        "class `%.*s`: field `%.*s` embeds a class whose shape "
                        "changed — its stored values carry the old sub-shape, "
                        "which v1 does not rewrite. Migrate the embedded class "
                        "on its own first",
                        (int)osc->classes[c].name_len,
                        (const char *)osc->classes[c].name, (int)of->name_len,
                        (const char *)of->name);
                    again = 1;
                    break;
                }
            }
        }
    }

    plan->identity = 1;
    for (uint32_t c = 0; c < osc->class_cnt; c++) {
        const wo_mig_class *mc = &plan->classes[c];
        if (mc->poison) continue; /* a poison alone never forces a rewrite */
        if (mc->new_cid != c || mc->changed) {
            plan->identity = 0;
            break;
        }
    }
    return 0;
}

int wo_wal_set_schema(wo_wal *w, const wo_schema *sc) {
    uint8_t *p;
    uint32_t len;
    if (wo_schema_encode(sc, &p, &len) != 0) return -1;
    free(w->schema);
    w->schema = p;
    w->schema_len = len;
    return 0;
}

int wo_wal_ensure_schema(wo_wal *w) {
    if (!w->schema) return 0;             /* never set: legacy behaviour */
    if (w->off != 0 || w->len != 0) return 0; /* records exist or staged */
    wbuf p = {0};
    wput(&p, w->schema, w->schema_len);
    int rc = stage(w, &p);
    free(p.b);
    if (rc != 0) return -1;
    return wo_wal_commit(w);
}

int wo_wal_read_schema(const char *path, uint8_t **payload_out, uint32_t *len_out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? 1 : -1;
    uint32_t len;
    uint8_t *payload;
    int rc = scan_record(fd, 0, &len, &payload);
    close(fd);
    if (rc != 0) return 1; /* empty or torn head: a legacy log */
    if (len < 1 || payload[0] != WO_WAL_SCHEMA) {
        free(payload);
        return 1;
    }
    if (payload_out) *payload_out = payload;
    else free(payload);
    if (len_out) *len_out = len;
    return 0;
}

int wo_wal_append_insert(wo_wal *w, wo_db *db, uint32_t class_id, uint64_t id) {
    /* wo_row_ptr, NOT wo_row_borrow: at append time a keys-resident row is
     * still in its slab and the id map still holds a SLOT, not an offset —
     * the drop happens after the commit. Borrowing here would read the log at
     * a byte position that is really a slot number. (Compaction, which does
     * face rows that live only in the log, moves their bytes instead — see
     * copy_record.) */
    db_row *r = wo_row_ptr(db, class_id, id);
    if (!r) return -1; /* commit order: RAM apply comes FIRST */
    wbuf p = {0};
    wput_u8(&p, WO_WAL_INSERT);
    wput_u32(&p, class_id);
    wput_u64(&p, id);
    const wo_classdesc *c = &db->classes[class_id];
    for (uint32_t i = 0; i < c->field_cnt; i++) enc_val(&p, db->classes, c->kinds[i], r->slots[i]);
    int rc = stage(w, &p);
    free(p.b);
    return rc;
}

int wo_wal_append_update(wo_wal *w, wo_db *db, uint32_t class_id, uint64_t id) {
    /* wo_row_ptr is right here for the same reason as append_insert. A
     * keys-resident update CAN succeed now (Task 3's row_apply_field_keys),
     * but this function never runs for one: db.c routes a keys-resident
     * update to a staged DELTA record instead (row_apply_field_keys), and
     * never re-logs the whole row. This function stays reachable only for
     * resident: all, guarded at both db.c call sites. */
    db_row *r = wo_row_ptr(db, class_id, id);
    if (!r) return -1;
    wbuf p = {0};
    wput_u8(&p, WO_WAL_UPDATE);
    wput_u32(&p, class_id);
    wput_u64(&p, id);
    const wo_classdesc *c = &db->classes[class_id];
    for (uint32_t i = 0; i < c->field_cnt; i++) enc_val(&p, db->classes, c->kinds[i], r->slots[i]);
    int rc = stage(w, &p);
    free(p.b);
    return rc;
}

/* databasev2 11: the chain-terminating write. Same encode as append_insert,
 * but from a row the caller already holds — a keys-resident row whose payload
 * has been dropped has no slab image for wo_row_ptr to find, and the update
 * path is the one caller that legitimately has the full, post-update values in
 * hand because it folded them to maintain indexes.
 *
 * WO_WAL_UPDATE, not WO_WAL_INSERT, and the distinction is not cosmetic.
 * Compaction's own flattening (stage_flattened_row) writes INSERT because it
 * builds a FRESH log in which each row appears exactly once. This function
 * appends into a LIVE log that already carries the row's original insert, so
 * an INSERT here is a duplicate id, and replay correctly refuses a duplicate as
 * corruption — caught by test_delta_chain_flatten_replays, which is the only
 * way this could have been caught: the record reads back perfectly in-process
 * and only fails on the next boot.
 *
 * UPDATE is exactly right anyway: replay applies it as remove-then-recreate,
 * which is what replacing a row wholesale means, and the fold terminates on
 * either full-row kind. */
int wo_wal_append_row_image(wo_wal *w, wo_db *db, uint32_t class_id, uint64_t id,
                            const db_row *r) {
    if (!r) return -1;
    wbuf p = {0};
    wput_u8(&p, WO_WAL_UPDATE);
    wput_u32(&p, class_id);
    wput_u64(&p, id);
    const wo_classdesc *c = &db->classes[class_id];
    for (uint32_t i = 0; i < c->field_cnt; i++) enc_val(&p, db->classes, c->kinds[i], r->slots[i]);
    int rc = stage(w, &p);
    free(p.b);
    return rc;
}

int wo_wal_append_delta(wo_wal *w, wo_db *db, uint32_t class_id, uint64_t id,
                         uint32_t field_idx, uint64_t back_off, uint64_t value) {
    wbuf p = {0};
    wput_u8(&p, WO_WAL_DELTA);
    wput_u32(&p, class_id);
    wput_u64(&p, id);
    wput_u32(&p, field_idx);
    wput_u64(&p, back_off);
    const wo_classdesc *c = &db->classes[class_id];
    enc_val(&p, db->classes, c->kinds[field_idx], value);
    int rc = stage(w, &p);
    free(p.b);
    return rc;
}

int wo_wal_append_remove(wo_wal *w, uint32_t class_id, uint64_t id) {
    wbuf p = {0};
    wput_u8(&p, WO_WAL_REMOVE);
    wput_u32(&p, class_id);
    wput_u64(&p, id);
    int rc = stage(w, &p);
    free(p.b);
    return rc;
}

int wo_wal_commit(wo_wal *w) {
    if (!w->len) return 0; /* empty commits are not batches; do not count them */
    if (w->len > w->stat_peak_staged) w->stat_peak_staged = w->len;
    size_t at = 0;
    while (at < w->len) {
        ssize_t n = pwrite(w->fd, w->buf + at, w->len - at, (off_t)(w->off + at));
        if (n < 0) {
            if (errno == EINTR) continue;
            return WO_WAL_ERR_WRITE;
        }
        at += (size_t)n;
    }
    if (fdatasync(w->fd) != 0) return WO_WAL_ERR_SYNC;
    w->off += w->len;
    w->len = 0; /* acked: the batch is durable */
    return 0;
}

/* Nothing at either fatal point is recoverable: RAM holds changes the log
 * does not, and this process can no longer serve reads that would survive a
 * restart. Name what failed precisely enough to act on, then stop. */
static void wal_die(const wo_wal *w, const char *op, uint32_t nrec) {
    fprintf(stderr,
            "writeonce: DURABILITY FAILURE — %s failed on %s: %s\n"
            "  %u record(s) were NOT made durable and are not acknowledged.\n"
            "  The process is stopping: replay restores the last durable state.\n",
            op, w->path ? w->path : "(the write-ahead log)", strerror(errno),
            nrec);
    exit(WO_EXIT_DURABILITY);
}

void wo_wal_stage_fatal(const wo_wal *w) { wal_die(w, "staging a record", 1); }

/* IMPORTANT 1 (review finding): unlike wo_wal_pend_drop's failure, a lost
 * pend_repoint is not safe to shrug off — the delta it was meant to
 * re-point to is already staged, RAM has already moved (table.c's own
 * fatal-below-this-line rule), and a second same-drain update to the same
 * row would otherwise chain its delta past the lost one, permanently. */
void wo_wal_repoint_fatal(const wo_wal *w) { wal_die(w, "recording a pending re-point", 1); }

void wo_wal_commit_fatal(wo_wal *w, uint32_t nrec) {
    int staged = w->len != 0;
    int rc = wo_wal_commit(w);
    if (rc == 0) {
        if (staged) { /* count the barrier that actually happened */
            w->stat_batches++;
            w->stat_records += nrec;
            if (nrec > w->stat_peak_batch) w->stat_peak_batch = nrec;
        }
        return;
    }
    wal_die(w, rc == WO_WAL_ERR_SYNC ? "fdatasync" : "pwrite", nrec);
}

uint64_t wo_wal_ckpt_floor = 4u << 20; /* 4 MiB: below this there is nothing worth reclaiming */
uint32_t wo_wal_ckpt_ratio = 3u;      /* 3x the live-set's own size is enough history */

int wo_wal_should_compact(uint64_t used, uint64_t last, uint64_t floor, uint32_t ratio) {
    if (used < floor) return 0;   /* a small log has nothing to reclaim */
    if (last == 0) return 1;      /* past the floor and never compacted: do it once
                                   * to establish the denominator */
    if (ratio == 0) return 0;     /* a zero ratio disables the policy rather than
                                   * dividing by nothing */

    /* databasev2 11: an ABSOLUTE garbage term, and a ceiling on the
     * proportional one.
     *
     * NOTE THE VOCABULARY TRAP this fixes. `floor` above SUPPRESSES compaction
     * on a small log — the opposite of what the same word means in PostgreSQL,
     * where autovacuum's `vac_base_thresh` (default 50) TRIGGERS cleanup on a
     * small absolute problem that the proportional term would hide. We had the
     * proportion and the suppressor and neither of the two guards that keep a
     * size-based policy honest:
     *
     *   - without a triggering term, garbage that is large in bytes but small
     *     relative to a big live set is never reclaimed;
     *   - a very large live set would otherwise defer compaction forever,
     *     which is what autovacuum_vacuum_max_threshold exists to stop.
     *
     * ONE term does both jobs here, and a separate ceiling was tried and
     * removed as dead code. PostgreSQL needs two because its threshold counts
     * TUPLES and its two constants sit at opposite ends (base 50, max 1e8).
     * Ours counts BYTES, so "compact once garbage exceeds X" already caps
     * deferral: any ceiling above X is unreachable, and any ceiling below it
     * would be the trigger. Caught by trying to write a test that exercised
     * the ceiling and finding none could. */
    uint64_t garbage = used > last ? used - last : 0;
    if (garbage >= WO_CKPT_ABS_BYTES) return 1;
    return used > last * (uint64_t)ratio;
}

/* databasev2 3: how many records the dump stages before flushing.
 *
 * NOT unbounded: stage() grows the staging buffer by doubling and never
 * shrinks it, so appending a whole store through one buffer would hold the
 * entire store in RAM on top of the store itself — the unbounded growth
 * databasev2 1 identified as how this engine dies. 256 records is a few tens
 * of KiB per flush, which is large enough that the syscall cost is amortised
 * and small enough that the buffer never matters. */
#define WO_WAL_COMPACT_FLUSH 256u

/* rename(2)'s atomicity is in-kernel: the new directory ENTRY is not durable
 * until the parent directory is synced. Postgres does the same thing for the
 * same reason. Best-effort — a filesystem that refuses to sync a directory
 * still leaves a correct log, just one whose swap might not survive a power
 * cut. */
static void sync_parent_dir(const char *path) {
    char dir[4096];
    size_t n = strlen(path);
    if (n >= sizeof dir) return;
    memcpy(dir, path, n + 1);
    char *slash = strrchr(dir, '/');
    if (slash == dir) dir[1] = '\0';
    else if (slash) *slash = '\0';
    else memcpy(dir, ".", 2);
    int fd = open(dir, O_RDONLY);
    if (fd < 0) return;
    (void)fsync(fd);
    close(fd);
}

/* databasev2 3: write the staged bytes WITHOUT a durability barrier.
 *
 * Only compaction's dump uses this. Intermediate durability there is worthless:
 * the temp file is not authoritative until the rename, and it is fsynced once
 * immediately before that. Using wo_wal_commit for the dump instead cost one
 * fdatasync per 256 records — measured, that was most of the stop-the-world
 * pause (~22 MB/s, where the fixed cost plus ~150 redundant syncs dominated a
 * 2 MB dump). */
static int wal_write_nosync(wo_wal *w) {
    size_t at = 0;
    while (at < w->len) {
        ssize_t n = pwrite(w->fd, w->buf + at, w->len - at, (off_t)(w->off + at));
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        at += (size_t)n;
    }
    w->off += w->len;
    w->len = 0;
    return 0;
}

static uint64_t mono_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* ============================================================================
 * OBLIGATION FOR WHOEVER IMPLEMENTS `resident: keys` (databasev2 2, tasks
 * 5c/5d) — READ THIS BEFORE STORING WAL OFFSETS.
 *
 * Compaction rewrites the log and MOVES EVERY RECORD. Any WAL byte offset
 * captured from the old file is meaningless afterwards — not stale-but-
 * readable, but pointing at an arbitrary byte of a different file.
 *
 * `resident: keys` stores exactly such an offset per row and reads rows back
 * through it. So the loop below, which knows each record's NEW position as it
 * writes it, MUST also rebuild that map. It is the cheap direction and the only
 * one that keeps both features usable together; the alternative is forbidding
 * compaction whenever such a table is live, which would mean the feature for
 * huge tables is incompatible with the feature that stops their log growing.
 *
 * Nothing fails today because that storage half does not exist yet. It will
 * fail later, and it will look like data corruption rather than a design gap.
 * ==========================================================================*/
/* databasev2 2 (5d): move one already-durable record from the old log into
 * the new one, VERBATIM.
 *
 * Compaction cannot re-encode a keys-resident row the way it re-encodes a
 * resident one. enc_val expects the ENGINE representation a slab row holds
 * (db_text: len + bytes), while a row read back out of the log arrives in the
 * VM representation (wo_str: len + data). The two are not the same struct, so
 * feeding a borrowed row to enc_val reads the length out of the wrong field —
 * ASan caught exactly that as a 4294967292-byte memcpy.
 *
 * Copying the bytes sidesteps the whole question, and is strictly better than
 * decode-then-encode anyway: no allocation per row, no arena pressure, and the
 * record that lands is bit-identical to the one that was acked. scan_record
 * verifies the CRC, so a torn record is refused rather than propagated.
 *
 * The kind byte is normalised to INSERT: a live row's newest image may have
 * been logged as an UPDATE, and the compacted log is supposed to read as one
 * INSERT per live row. */
static int copy_record(wo_wal *nw, wo_wal *ow, uint64_t off, uint32_t want_cid,
                       uint64_t want_id, const char **why) {
    uint32_t len = 0;
    uint8_t *payload = NULL;
    if (scan_record(ow->fd, off, &len, &payload) != 0) {
        *why = "no intact record at a row's recorded offset";
        return -1;
    }
    rbuf r = {payload, payload + len, 0};
    uint8_t kind = rd_u8(&r);
    uint32_t cid = rd_u32(&r);
    uint64_t id = rd_u64(&r);
    if (r.bad || cid != want_cid || id != want_id ||
        (kind != WO_WAL_INSERT && kind != WO_WAL_UPDATE)) {
        free(payload);
        *why = "a row's recorded offset does not hold that row";
        return -1;
    }
    payload[0] = WO_WAL_INSERT;
    wbuf p = {0};
    wput(&p, payload, len);
    int rc = stage(nw, &p);
    free(p.b);
    free(payload);
    if (rc != 0) *why = "staging a moved record failed";
    return rc;
}

/* Task 5 (replay and compaction fold the same way): copy_record's
 * counterpart for a row whose chain is NOT empty — a delta cannot be moved
 * byte-for-byte the way copy_record moves a base row, because the whole
 * point of compaction is to reset every chain to length zero. Fold the row
 * (THE fold, same one reads and replay use) and re-encode it as a fresh
 * INSERT via enc_val, exactly the representation the fold's own contract
 * promises (ENGINE values, not the VM's) — so this, unlike copy_record,
 * never touches wo_row_ptr/a slab at all. */
static int stage_flattened_row(wo_wal *nw, wo_wal *ow, wo_db *db, uint64_t off,
                               uint32_t class_id, uint64_t id, const char **why) {
    const wo_classdesc *c = &db->classes[class_id];
    uint64_t *vals = c->field_cnt ? calloc(c->field_cnt, sizeof *vals) : NULL;
    if (c->field_cnt && !vals) {
        *why = "out of memory flattening a delta chain";
        return -1;
    }
    uint32_t got_cid = 0;
    uint64_t got_id = 0;
    const char *fmsg = "";
    if (wo_wal_fold_row_at(ow, db, off, &got_cid, &got_id, vals, NULL, &fmsg) != 0 ||
        got_cid != class_id || got_id != id) {
        for (uint32_t i = 0; i < c->field_cnt; i++) wo_db_val_free(db, c->kinds[i], vals[i]);
        free(vals);
        *why = "a live row's delta chain does not fold cleanly";
        return -1;
    }
    wbuf p = {0};
    wput_u8(&p, WO_WAL_INSERT);
    wput_u32(&p, class_id);
    wput_u64(&p, id);
    for (uint32_t i = 0; i < c->field_cnt; i++) enc_val(&p, db->classes, c->kinds[i], vals[i]);
    int rc = stage(nw, &p);
    free(p.b);
    for (uint32_t i = 0; i < c->field_cnt; i++) wo_db_val_free(db, c->kinds[i], vals[i]);
    free(vals);
    if (rc != 0) *why = "staging a flattened row failed";
    return rc;
}

int wo_wal_compact(wo_wal *w, wo_db *db) {
    /* staged records would be written into a file about to be replaced */
    if (!w->path || w->len != 0) return -1;
    uint64_t t0 = mono_us();
    int repointed = 0; /* has any keys-resident row been moved to the new log? */
    const char *why = NULL; /* the reason a fail arm was taken, when it is known */

    char tmp[4096];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s%s", w->path, WO_WAL_TMP_SUFFIX) >= sizeof tmp)
        return -1;
    (void)unlink(tmp); /* a stale one would otherwise be appended to */

    wo_wal nw;
    /* THE REPLACEMENT MUST BE PREALLOCATED LIKE THE ORIGINAL. The WAL is
     * preallocated so appends never extend the file, which is precisely what
     * makes fdatasync sufficient as the ack barrier — no file-size metadata
     * has to reach disk for an acked record to be readable. Opening the
     * replacement with prealloc 0 silently removed that property, and the
     * crash battery caught it: records acked shortly before a kill went
     * missing, with the log otherwise intact and self-consistent. */
    if (wo_wal_open(&nw, tmp, w->prealloc) != 0) return -1;

    /* databasev2 12: the replacement log's first record is the schema, so a
     * compacted log is always self-describing — including the first
     * compaction of a legacy log, which is how existing WO_DATA dirs become
     * diffable without any migration step of their own. */
    if (w->schema) {
        wbuf sp = {0};
        wput(&sp, w->schema, w->schema_len);
        int src = stage(&nw, &sp);
        free(sp.b);
        if (src != 0) goto fail;
    }

    /* one INSERT per live row, in the existing grammar, through the existing
     * append path — so replay needs no second decoder and ids are preserved
     * exactly (wo_wal_append_insert takes the id and reads the row) */
    uint32_t pending = 0;
    /* databasev2 2 (5d): the walk is wo_row_next_id, not the bitmap. A
     * keys-resident row has NO bitmap bit — its slot was returned to the free
     * list when the payload was dropped — so a bitmap walk would omit every
     * such row from the new log and call it compaction. That is silent data
     * loss, and it is the failure the obligation note above was written for. */
    for (uint32_t cid = 0; cid < db->class_cnt; cid++) {
        db_table *t = &db->tables[cid];
        if (!t->row_size) continue; /* tables are created lazily */
        int keys = wo_table_is_keys_resident(db, cid);
        size_t cur = 0;
        uint64_t id;
        while (wo_row_next_id(db, cid, &cur, &id)) {
            if (!keys) {
                if (wo_wal_append_insert(&nw, db, cid, id) != 0) goto fail;
            } else {
                /* read from the OLD log (still open, still the live file at
                 * this point), write into the new one, and re-point the map
                 * to where it landed. The offset is captured BEFORE staging:
                 * off is what has reached the file, len what is staged behind
                 * it, so their sum is the position of the next record. */
                uint64_t o1 = wo_row_offset1(db, cid, id);
                if (!o1) {
                    why = "a live keys-resident row has no recorded offset";
                    goto fail;
                }
                uint64_t at = nw.off + (uint64_t)nw.len;
                /* Task 5: a chain flattens to ONE full row on every
                 * checkpoint — the bound the whole design relies on
                 * (without it, chains grow without limit). Peek the kind
                 * byte at the row's current record (position fixed by the
                 * frame header, valid whatever the record turns out to be)
                 * to choose: an empty chain keeps the existing byte-for-byte
                 * copy, faster and unchanged; a delta chain folds instead of
                 * being copied — copying it would just move the chain, not
                 * flatten it. */
                uint8_t kind_byte = 0;
                if (pread(w->fd, &kind_byte, 1, (off_t)(o1 - 1 + 8)) != 1) {
                    why = "cannot read a row's recorded offset";
                    goto fail;
                }
                if (kind_byte == WO_WAL_DELTA) {
                    if (stage_flattened_row(&nw, w, db, o1 - 1, cid, id, &why) != 0) goto fail;
                } else if (copy_record(&nw, w, o1 - 1, cid, id, &why) != 0) {
                    goto fail;
                }
                /* value-only update: cannot rehash, so `cur` stays valid */
                if (wo_row_set_offset(db, cid, id, at) != 0) {
                    why = "row vanished from the id map mid-compaction";
                    goto fail;
                }
                repointed = 1;
            }
            if (++pending >= WO_WAL_COMPACT_FLUSH) {
                if (wal_write_nosync(&nw) != 0) goto fail;
                pending = 0;
            }
        }
    }
    if (wal_write_nosync(&nw) != 0) goto fail; /* the tail batch */
    /* THE dump's one and only barrier: everything above is just bytes in the
     * page cache until this, and nothing reads the temp before the rename. */
    if (fsync(nw.fd) != 0) goto fail;

    uint64_t new_bytes = nw.off;
    wo_wal_close(&nw);

    /* THE SWITCH. Every crash point either side of this is safe. */
    if (rename(tmp, w->path) != 0) {
        (void)unlink(tmp);
        return -1;
    }
    sync_parent_dir(w->path);

    /* the old descriptor now refers to an unlinked inode */
    if (w->fd >= 0) close(w->fd);
    w->fd = open(w->path, O_RDWR);
    if (w->fd < 0) return -1; /* the log is correct on disk; this process cannot go on */
    w->off = new_bytes;
    w->len = 0;
    w->compacted_bytes = new_bytes;
    {   /* the stop-the-world pause: nothing was served while this ran */
        uint64_t el = mono_us() - t0;
        w->stat_compactions++;
        w->stat_compact_us_total += el;
        if (el > w->stat_compact_us_max) w->stat_compact_us_max = el;
    }
    return 0;

fail:
    wo_wal_close(&nw);
    (void)unlink(tmp);
    if (repointed) {
        /* databasev2 2 (5d): rows already re-pointed name offsets inside the
         * temp file just unlinked, so the id map now describes a file that no
         * longer exists — reads would return another row's bytes or nothing.
         * The log ON DISK is still the intact original, so replay rebuilds the
         * map correctly; carrying on in this process cannot. Same doctrine as
         * a failed commit barrier: stop rather than serve wrong rows. */
        fprintf(stderr,
                "writeonce: DURABILITY FAILURE — compaction of %s failed after "
                "moving rows: %s\n"
                "  No data was lost: the original log is intact on disk.\n"
                "  The process is stopping: replay rebuilds the row offsets.\n",
                w->path, why ? why : "write or sync error");
        exit(WO_EXIT_DURABILITY);
    }
    return -1; /* the live log is untouched and still usable */
}

/* Task 5 (replay and compaction fold the same way): the DELTA replay arm.
 *
 * A delta cannot be applied like an insert/update — its body is one field,
 * not the row — so it borrows WO_WAL_UPDATE's remove-then-recreate SHAPE
 * instead: fold the row's PRE-delta state (THE fold, the same one reads and
 * compaction use) via [back_off], overlay this delta's field onto it, then
 * remove-and-recreate through the ordinary choke points. Recreating, rather
 * than patching an index in place, is what re-indexes a changed INDEXED
 * column for free — wo_row_remove takes the OLD value's entries out,
 * wo_row_raw_commit puts the NEW value's entries in, exactly as a live
 * update would have. The caller (wo_wal_replay_ex) still owns dropping the
 * recreated row's payload back to the log, the same way it does for
 * INSERT/UPDATE. */
static int apply_delta(wo_db *db, uint32_t cid, uint64_t id, rbuf *r) {
    uint32_t field_idx = rd_u32(r);
    uint64_t back_off = rd_u64(r);
    const wo_classdesc *c = &db->classes[cid];
    if (r->bad || field_idx >= c->field_cnt) return -1;
    /* IMPORTANT 2 (review finding): a DELTA can only be folded through
     * db->rt->wal (wo_wal_replay_ex's lent view, at replay, or a live one
     * otherwise) — refuse cleanly instead of dereferencing a NULL rt. */
    if (!db->rt) return -1;
    uint64_t nv;
    if (dec_val(r, db, c->kinds[field_idx], &nv) != 0 || (size_t)(r->end - r->p) != 0)
        return -1;

    uint32_t field_cnt = c->field_cnt;
    uint64_t *vals = field_cnt ? calloc(field_cnt, sizeof *vals) : NULL;
    if (field_cnt && !vals) {
        wo_db_val_free(db, c->kinds[field_idx], nv);
        return -1;
    }
    uint32_t got_cid = 0;
    uint64_t got_id = 0;
    const char *fmsg = "";
    if (wo_wal_fold_row_at((wo_wal *)db->rt->wal, db, back_off, &got_cid, &got_id, vals,
                           NULL, &fmsg) != 0 ||
        got_cid != cid || got_id != id) {
        for (uint32_t i = 0; i < field_cnt; i++) wo_db_val_free(db, c->kinds[i], vals[i]);
        free(vals);
        wo_db_val_free(db, c->kinds[field_idx], nv);
        return -1;
    }
    wo_db_val_free(db, c->kinds[field_idx], vals[field_idx]);
    vals[field_idx] = nv;

    /* the row must exist (its base record precedes its deltas in a correct
       log); anything else is corruption */
    if (wo_row_remove(db, cid, id) != 0) {
        for (uint32_t i = 0; i < field_cnt; i++) wo_db_val_free(db, c->kinds[i], vals[i]);
        free(vals);
        return -1;
    }
    db_row *row = wo_row_create_raw(db, cid, id);
    if (!row) {
        for (uint32_t i = 0; i < field_cnt; i++) wo_db_val_free(db, c->kinds[i], vals[i]);
        free(vals);
        return -1;
    }
    memcpy(row->slots, vals, (size_t)field_cnt * sizeof *vals);
    free(vals);
    /* a unique violation here is corruption, same reasoning as INSERT/UPDATE
       below: the live update that produced this delta would have refused it */
    if (wo_row_raw_commit(db, cid, row) != 0) {
        wo_row_remove(db, cid, id);
        return -1;
    }
    return 0;
}

static int apply_record(wo_db *db, const uint8_t *payload, uint32_t len) {
    rbuf r = {payload, payload + len, 0};
    uint8_t kind = rd_u8(&r);
    /* databasev2 12: a schema record is descriptive, not a row — and it has
     * no cid/id fields, so it must be skipped BEFORE those are read (its
     * class count would be misread as a cid and bounds-refused). */
    if (kind == WO_WAL_SCHEMA) return 0;
    uint32_t cid = rd_u32(&r);
    uint64_t id = rd_u64(&r);
    if (r.bad || cid >= db->class_cnt) return -1;
    /* databasev2 2: this log holds records for a class the CURRENT source
     * declares `durable: false`. Not corruption — a real migration case (the
     * table used to be durable). Refuse rather than convert, and refuse
     * rather than silently resurrect rows into a table declared not to have
     * any. -2 so the caller can say which of the two it is. */
    if (db->classes[cid].flags & WO_CLASSF_VOLATILE) return -2;
    if (kind == WO_WAL_REMOVE) return wo_row_remove(db, cid, id);
    if (kind == WO_WAL_DELTA) return apply_delta(db, cid, id, &r);
    if (kind != WO_WAL_INSERT && kind != WO_WAL_UPDATE) return -1;
    if (kind == WO_WAL_UPDATE) {
        /* replace: the row must exist (its insert precedes its update in a
           correct log); anything else is corruption */
        if (wo_row_remove(db, cid, id) != 0) return -1;
    }
    db_row *row = wo_row_create_raw(db, cid, id);
    if (!row) return -1;
    const wo_classdesc *c = &db->classes[cid];
    for (uint32_t i = 0; i < c->field_cnt; i++) {
        if (dec_val(&r, db, c->kinds[i], &row->slots[i]) != 0) {
            /* a record that CRC-passed but does not decode is corruption,
             * not a tear: fail loudly (the row's built slots are freed by
             * wo_row_remove, which also unregisters the id) */
            wo_row_remove(db, cid, id);
            return -1;
        }
    }
    if ((size_t)(r.end - r.p) != 0) { /* trailing bytes = corrupt */
        wo_row_remove(db, cid, id);
        return -1;
    }
    /* slots are real now: re-index (Task 4). A unique violation during
     * replay is corruption — the live insert would have refused it. */
    if (wo_row_raw_commit(db, cid, row) != 0) {
        wo_row_remove(db, cid, id);
        return -1;
    }
    return 0;
}

/* databasev2 2: materialise a row straight from a log offset.
 *
 * The offset twin of wo_row_read (table.c): same out-gate contract — every
 * value handed back is a FRESH VM allocation, never a pointer into anything
 * the engine owns — but resolved from a file position instead of the id hash.
 * This is what a `resident: keys` table's read path will call once 5c gives
 * it an id->offset map; nothing calls it yet, deliberately.
 *
 * Two decode stages, because the record and the VM speak different dialects:
 * dec_val yields ENGINE-owned slots (db_text and friends, exactly what a slab
 * row holds), then wo_val_decode_vm copies each into the VM. The engine slots
 * are scratch and are always freed before returning, on every path. */
int wo_wal_read_row_at(wo_wal *w, wo_db *db, wo_rt *rt, uint64_t off,
                       uint32_t *class_out, uint64_t *id_out, uint64_t *out_vals,
                       const char **msg) {
    uint32_t len = 0;
    uint8_t *payload = NULL;
    if (scan_record(w->fd, off, &len, &payload) != 0) {
        *msg = "no intact record at that offset";
        return -1;
    }
    rbuf r = {payload, payload + len, 0};
    uint8_t kind = rd_u8(&r);
    uint32_t cid = rd_u32(&r);
    uint64_t id = rd_u64(&r);
    if (r.bad || cid >= db->class_cnt) {
        free(payload);
        *msg = "record header is malformed";
        return -1;
    }
    /* A REMOVE tombstone carries no field payload. Handing one back as a row
     * would be the worst failure available here — the caller would read a
     * deleted row as live — so it is refused explicitly, not decoded. */
    if (kind != WO_WAL_INSERT && kind != WO_WAL_UPDATE) {
        free(payload);
        *msg = "record at that offset is a tombstone, not a row";
        return -1;
    }
    const wo_classdesc *c = &db->classes[cid];
    uint64_t *slots = c->field_cnt ? calloc(c->field_cnt, sizeof *slots) : NULL;
    if (c->field_cnt && !slots) {
        free(payload);
        *msg = "out of memory reading a row";
        return -2;
    }
    int rc = 0;
    uint32_t done = 0;
    for (; done < c->field_cnt; done++) {
        if (dec_val(&r, db, c->kinds[done], &slots[done]) != 0) {
            *msg = "record at that offset does not decode";
            rc = -1;
            break;
        }
    }
    if (rc == 0 && (size_t)(r.end - r.p) != 0) { /* trailing bytes = corrupt */
        *msg = "record at that offset has trailing bytes";
        rc = -1;
    }
    if (rc == 0) {
        int ok = 1;
        for (uint32_t i = 0; i < c->field_cnt; i++) {
            out_vals[i] = wo_val_decode_vm(db, rt, c->kinds[i], slots[i], &ok, msg);
            if (!ok) {
                rc = -2;
                break;
            }
        }
    }
    /* engine slots are scratch: free every one that was built, on EVERY path */
    for (uint32_t i = 0; i < done; i++) wo_db_val_free(db, c->kinds[i], slots[i]);
    free(slots);
    free(payload);
    if (rc == 0) {
        if (class_out) *class_out = cid;
        if (id_out) *id_out = id;
    }
    return rc;
}

/* keys-resident delta updates, Task 2: THE fold. See wal.h. Reads, replay,
 * and compaction all call this one function — never a second copy. */
int wo_wal_fold_row_at(wo_wal *w, wo_db *db, uint64_t off, uint32_t *class_out,
                       uint64_t *id_out, uint64_t *out_vals, uint32_t *hops_out,
                       const char **msg) {
    uint32_t hops = 0;             /* databasev2 11: DELTA records crossed */
    if (hops_out) *hops_out = 0;
    uint32_t cid = 0;
    uint64_t id = 0;
    uint32_t field_cnt = 0;
    uint8_t *resolved = NULL;      /* field idx -> a delta already claimed it */
    uint64_t *resolved_val = NULL; /* field idx -> its remembered engine value */
    uint64_t cur = off;
    int rc = 0;
    int first = 1;

    for (;;) {
        uint32_t len;
        uint8_t *payload;
        /* Task 4 follow-up: a hop into the currently-staged (not yet
           durable) region reads from `w->buf`, not the file — see
           scan_record_staged. Every other hop (the durable majority of
           any real chain) is the original file read, unchanged. */
        int src_rc = (cur >= w->off && cur < w->off + w->len)
                         ? scan_record_staged(w, cur, &len, &payload)
                         : scan_record(w->fd, cur, &len, &payload);
        if (src_rc != 0) {
            *msg = "no intact record at that offset";
            rc = -1;
            break;
        }
        rbuf r = {payload, payload + len, 0};
        uint8_t kind = rd_u8(&r);
        uint32_t rec_cid = rd_u32(&r);
        uint64_t rec_id = rd_u64(&r);
        if (r.bad || rec_cid >= db->class_cnt) {
            free(payload);
            *msg = "record header is malformed";
            rc = -1;
            break;
        }
        if (first) {
            first = 0;
            cid = rec_cid;
            id = rec_id;
            field_cnt = db->classes[cid].field_cnt;
            resolved = field_cnt ? calloc(field_cnt, 1) : NULL;
            resolved_val = field_cnt ? calloc(field_cnt, sizeof *resolved_val) : NULL;
            if (field_cnt && (!resolved || !resolved_val)) {
                free(payload);
                *msg = "out of memory folding a row";
                rc = -2;
                break;
            }
        } else if (rec_cid != cid || rec_id != id) {
            free(payload);
            *msg = "delta chain does not agree on row identity";
            rc = -1;
            break;
        }

        if (kind == WO_WAL_DELTA) {
            uint32_t field_idx = rd_u32(&r);
            uint64_t back_off = rd_u64(&r);
            if (r.bad || field_idx >= field_cnt) {
                free(payload);
                *msg = "delta record is malformed";
                rc = -1;
                break;
            }
            /* THE cycle/forgery guard: a back-pointer names the row's
             * PREVIOUS record, which by construction is earlier in the
             * (append-only) log than the delta naming it. Anything else —
             * a self-pointer, a forward pointer, a pointer that only forms
             * a cycle several hops later — is corruption or forgery, and
             * this is what actually rules all of those out: not a bound on
             * how many records could exist, which a forward pointer to a
             * genuine record satisfies trivially and a bound would then let
             * straight through. */
            if (back_off >= cur) {
                free(payload);
                *msg = "delta back-pointer does not point earlier in the log";
                rc = -1;
                break;
            }
            const wo_classdesc *c = &db->classes[cid];
            uint64_t v;
            if (dec_val(&r, db, c->kinds[field_idx], &v) != 0 ||
                (size_t)(r.end - r.p) != 0) {
                free(payload);
                *msg = "delta record does not decode";
                rc = -1;
                break;
            }
            if (!resolved[field_idx]) {
                resolved[field_idx] = 1;
                resolved_val[field_idx] = v;
            } else {
                /* shadowed by a newer delta already seen: still validated
                   above, but its value loses, exactly like the base row's */
                wo_db_val_free(db, c->kinds[field_idx], v);
            }
            free(payload);
            hops++;
            if (hops_out) *hops_out = hops;
            cur = back_off;
            continue;
        }

        if (kind != WO_WAL_INSERT && kind != WO_WAL_UPDATE) {
            free(payload);
            *msg = "record in a delta chain is a tombstone, not a row";
            rc = -1;
            break;
        }

        /* base row: decode every field positionally (a delta-shadowed
           field's bytes are still there — dec_val must still walk past
           them to keep the fields after it aligned), then overlay whatever
           the walk resolved. */
        const wo_classdesc *c = &db->classes[cid];
        uint64_t *slots = field_cnt ? calloc(field_cnt, sizeof *slots) : NULL;
        if (field_cnt && !slots) {
            free(payload);
            *msg = "out of memory folding a row";
            rc = -2;
            break;
        }
        uint32_t done = 0;
        for (; done < field_cnt; done++) {
            if (dec_val(&r, db, c->kinds[done], &slots[done]) != 0) {
                *msg = "record at that offset does not decode";
                rc = -1;
                break;
            }
        }
        if (rc == 0 && done == field_cnt && (size_t)(r.end - r.p) != 0) {
            *msg = "record at that offset has trailing bytes";
            rc = -1;
        }
        if (rc == 0 && done == field_cnt) {
            for (uint32_t i = 0; i < field_cnt; i++) {
                if (resolved[i]) {
                    wo_db_val_free(db, c->kinds[i], slots[i]);
                    slots[i] = resolved_val[i];
                }
            }
            memcpy(out_vals, slots, (size_t)field_cnt * sizeof *out_vals);
        } else {
            for (uint32_t i = 0; i < done; i++) wo_db_val_free(db, c->kinds[i], slots[i]);
        }
        free(slots);
        free(payload);
        break;
    }

    if (rc != 0 && resolved && resolved_val) {
        /* the walk resolved some fields but never reached a base row to
           install them into: free what it was holding */
        const wo_classdesc *c = &db->classes[cid];
        for (uint32_t i = 0; i < field_cnt; i++)
            if (resolved[i]) wo_db_val_free(db, c->kinds[i], resolved_val[i]);
    }
    free(resolved);
    free(resolved_val);

    if (rc == 0) {
        if (class_out) *class_out = cid;
        if (id_out) *id_out = id;
    }
    return rc;
}

int64_t wo_wal_replay_ex(const char *path, wo_db *db, uint32_t *volatile_cid) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? 0 : -1; /* no WAL yet = fresh boot */
    uint64_t off = 0;
    int64_t applied = 0;

    /* databasev2 2: replay must be able to READ rows back, not only write them.
     * A REMOVE (or an UPDATE or a DELTA, both of which replay as
     * remove-then-recreate — see apply_delta, Task 5) on a keys-resident
     * table reaches wo_row_remove, whose keys arm borrows the row out of the
     * log to find its index entries — and a borrow reads through
     * db->rt->wal. At boot that pointer is not wired yet: main.c replays first
     * and assigns rt.wal afterwards, so the borrow found no log, the remove
     * failed, and replay reported a perfectly good tombstone as CORRUPTION.
     *
     * Lend the runtime a read-only view over the fd already open here, for the
     * duration of the replay only, and restore whatever was there. Nothing in
     * this window appends, so a view carrying just the descriptor is enough.
     *
     * IMPORTANT 2 (review finding): this used to lend the view only when
     * db->rt->wal was NOT already set — leaving a rt whose wal WAS already
     * live to fold deltas against that LIVE wal's (possibly non-empty)
     * staging buffer instead, which the design never allows. Install the
     * throwaway view unconditionally whenever there is an rt to hold it;
     * apply_delta refuses outright when there is no rt at all to install
     * one into. */
    wo_wal view;
    memset(&view, 0, sizeof view);
    view.fd = fd;
    const void *saved_wal = NULL;
    int lent = 0;
    if (db->rt) {
        saved_wal = db->rt->wal;
        db->rt->wal = &view;
        lent = 1;
    }
#define REPLAY_RETURN(v)                                                       \
    do {                                                                       \
        if (lent) db->rt->wal = (void *)saved_wal;                             \
        return (v);                                                            \
    } while (0)
    for (;;) {
        uint32_t len;
        uint8_t *payload;
        if (scan_record(fd, off, &len, &payload) != 0) break; /* intact prefix ends */
        /* peek the class id before applying, so a -2 can name it */
        uint32_t rec_cid = len >= 5u ? (uint32_t)payload[1] | ((uint32_t)payload[2] << 8)
                                           | ((uint32_t)payload[3] << 16)
                                           | ((uint32_t)payload[4] << 24)
                                     : 0u;
        /* databasev2 2 (5c): a keys-resident row must end boot pointing at the
         * LOG, not at a slab. The record is applied normally (so indexes and
         * uniqueness are built exactly as for any other table) and then its
         * payload is dropped, leaving the id map holding THIS record's offset.
         * For an update the later record wins, because each apply overwrites
         * the map in order — which is the same rule replay already follows. */
        uint8_t rec_kind = len >= 1u ? payload[0] : 0u;
        uint64_t rec_id = 0;
        if (len >= 13u)
            for (int b = 0; b < 8; b++) rec_id |= (uint64_t)payload[5 + b] << (8 * b);
        int rc = apply_record(db, payload, len);
        free(payload);
        if (rc != 0) {
            close(fd);
            if (rc == -2 && volatile_cid) *volatile_cid = rec_cid;
            REPLAY_RETURN(rc == -2 ? -2 : -1);
        }
        if ((rec_kind == WO_WAL_INSERT || rec_kind == WO_WAL_UPDATE ||
             rec_kind == WO_WAL_DELTA) &&
            rec_id && wo_table_is_keys_resident(db, rec_cid))
            (void)wo_row_drop_payload(db, rec_cid, rec_id, off);
        off += 8u + len + 4u;
        if (rec_kind != WO_WAL_SCHEMA) applied++;
    }
    close(fd);
    REPLAY_RETURN(applied);
#undef REPLAY_RETURN
}

int64_t wo_wal_replay(const char *path, wo_db *db) {
    return wo_wal_replay_ex(path, db, NULL);
}

int64_t wo_wal_check(const char *path, uint64_t *intact_bytes) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    uint64_t off = 0;
    int64_t records = 0;
    for (;;) {
        uint32_t len;
        if (scan_record(fd, off, &len, NULL) != 0) break;
        off += 8u + len + 4u;
        records++;
    }
    if (intact_bytes) *intact_bytes = off;
    close(fd);
    return records;
}

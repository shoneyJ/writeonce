/* pread/pwrite/fdatasync/posix_fallocate under -std=c11 */
#define _POSIX_C_SOURCE 200809L

#include "wal.h"

#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <stdio.h>
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

void wo_wal_close(wo_wal *w) {
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

int wo_wal_append_insert(wo_wal *w, wo_db *db, uint32_t class_id, uint64_t id) {
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

int wo_wal_compact(wo_wal *w, wo_db *db) {
    /* staged records would be written into a file about to be replaced */
    if (!w->path || w->len != 0) return -1;
    uint64_t t0 = mono_us();

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

    /* one INSERT per live row, in the existing grammar, through the existing
     * append path — so replay needs no second decoder and ids are preserved
     * exactly (wo_wal_append_insert takes the id and reads the row) */
    uint32_t pending = 0;
    for (uint32_t cid = 0; cid < db->class_cnt; cid++) {
        db_table *t = &db->tables[cid];
        if (!t->slabs) continue; /* tables are created lazily */
        uint32_t total = t->slab_cnt * DB_SLAB_ROWS;
        for (uint32_t g = 0; g < total; g++) {
            if (!(t->bitmap[g >> 6] & (1ull << (g & 63)))) continue;
            db_row *r = (db_row *)(t->slabs[g / DB_SLAB_ROWS] +
                                   (size_t)(g % DB_SLAB_ROWS) * t->row_size);
            if (wo_wal_append_insert(&nw, db, cid, r->id) != 0) goto fail;
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
    return -1; /* the live log is untouched and still usable */
}

static int apply_record(wo_db *db, const uint8_t *payload, uint32_t len) {
    rbuf r = {payload, payload + len, 0};
    uint8_t kind = rd_u8(&r);
    uint32_t cid = rd_u32(&r);
    uint64_t id = rd_u64(&r);
    if (r.bad || cid >= db->class_cnt) return -1;
    if (kind == WO_WAL_REMOVE) return wo_row_remove(db, cid, id);
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

int64_t wo_wal_replay(const char *path, wo_db *db) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? 0 : -1; /* no WAL yet = fresh boot */
    uint64_t off = 0;
    int64_t applied = 0;
    for (;;) {
        uint32_t len;
        uint8_t *payload;
        if (scan_record(fd, off, &len, &payload) != 0) break; /* intact prefix ends */
        int rc = apply_record(db, payload, len);
        free(payload);
        if (rc != 0) {
            close(fd);
            return -1;
        }
        off += 8u + len + 4u;
        applied++;
    }
    close(fd);
    return applied;
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

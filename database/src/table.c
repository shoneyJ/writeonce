#include "table.h"

#include <stdlib.h>
#include <string.h>

#include "cont.h"
#include "gc.h"   /* databasev2 2 (5c): VM-side drops for materialised rows */
#include "wal.h" /* databasev2 2 (5c): a keys-resident borrow reads the log */

/* ---- engine-owned value encode / free / decode ------------------------- */

/* Free one encoded slot value of [kind]. Recursion mirrors encoding. */
static void db_val_free(uint8_t kind, uint64_t v);

static void db_rec_free(db_rec *r, const wo_classdesc *classes) {
    const wo_classdesc *c = &classes[r->class_id];
    for (uint32_t i = 0; i < c->field_cnt; i++) db_val_free(c->kinds[i], r->slots[i]);
    free(r);
}

/* db_val_free needs the class table for nested records; a file-static is
 * the honest signature here — one engine per process today (N=1), and the
 * pointer is set once at init. Revisit when iteration 8 brings N>1 shards
 * (each shard's wo_db shares the same immutable class table anyway). */
static const wo_classdesc *g_classes;

static void db_val_free(uint8_t kind, uint64_t v) {
    if (!v) return;
    switch (kind) {
    /* iteration 19: FLOAT is a word in the slot, nothing to free. BYTES is
       stored in the same db_text blob a Text is, so the same free serves. */
    case WO_K_SCALAR:
    case WO_K_FLOAT: return;
    case WO_K_TEXT:
    case WO_K_BYTES: free((db_text *)(uintptr_t)v); return;
    case WO_K_OWNED: db_rec_free((db_rec *)(uintptr_t)v, g_classes); return;
    case WO_K_MULTI: {
        db_multi *m = (db_multi *)(uintptr_t)v;
        for (uint32_t i = 0; i < m->len; i++) db_val_free(m->elem_kind, m->items[i]);
        free(m);
        return;
    }
    case WO_K_MAP: {
        db_map *m = (db_map *)(uintptr_t)v;
        for (uint32_t i = 0; i < m->len; i++) {
            db_val_free(m->key_kind, m->kv[2 * i]);
            db_val_free(m->val_kind, m->kv[2 * i + 1]);
        }
        free(m);
        return;
    }
    default: return; /* GCREF never stored */
    }
}

/* Encode one VM value into an engine-owned slot value. 0-with-*ok=0 means
 * failure (OOM or a GCREF); a genuine nil encodes as 0 with *ok=1. */
static uint64_t db_val_encode(const wo_classdesc *classes, uint8_t kind, uint64_t v,
                              int *ok, const char **msg) {
    *ok = 1;
    switch (kind) {
    /* iteration 19: the f64's bits go in the slot unexamined. NaN, the
       infinities, and -0.0 all store and read back bit-exact because nothing
       here interprets the word — the kind byte is what tells json and the WAL
       how to read it later. */
    case WO_K_SCALAR:
    case WO_K_FLOAT: return v;
    case WO_K_TEXT:
    case WO_K_BYTES: {
        if (!v) return 0;
        const wo_str *s = (const wo_str *)(uintptr_t)v;
        db_text *t = malloc(sizeof(db_text) + s->len);
        if (!t) goto oom;
        t->len = s->len;
        memcpy(t->bytes, s->data, s->len);
        return (uint64_t)(uintptr_t)t;
    }
    case WO_K_OWNED: {
        if (!v) return 0;
        const wo_hdr *o = (const wo_hdr *)(uintptr_t)v;
        const wo_classdesc *c = &classes[o->class_id];
        db_rec *r = malloc(sizeof(db_rec) + (size_t)c->field_cnt * 8u);
        if (!r) goto oom;
        r->class_id = o->class_id;
        r->_pad = 0;
        const uint64_t *f = (const uint64_t *)(const void *)(o + 1);
        for (uint32_t i = 0; i < c->field_cnt; i++) {
            r->slots[i] = db_val_encode(classes, c->kinds[i], f[i], ok, msg);
            if (!*ok) { /* free what we built so far, then fail upward */
                for (uint32_t j = 0; j < i; j++) db_val_free(c->kinds[j], r->slots[j]);
                free(r);
                return 0;
            }
        }
        return (uint64_t)(uintptr_t)r;
    }
    case WO_K_MULTI: {
        if (!v) return 0;
        const wo_multi *m = (const wo_multi *)(uintptr_t)v;
        db_multi *d = malloc(sizeof(db_multi) + (size_t)m->len * 8u);
        if (!d) goto oom;
        d->elem_kind = m->elem_kind;
        d->len = m->len;
        for (uint32_t i = 0; i < m->len; i++) {
            d->items[i] = db_val_encode(classes, m->elem_kind, m->items[i], ok, msg);
            if (!*ok) {
                for (uint32_t j = 0; j < i; j++) db_val_free(d->elem_kind, d->items[j]);
                free(d);
                return 0;
            }
        }
        return (uint64_t)(uintptr_t)d;
    }
    case WO_K_MAP: {
        if (!v) return 0;
        const wo_map *m = (const wo_map *)(uintptr_t)v;
        db_map *d = malloc(sizeof(db_map) + (size_t)m->len * 16u);
        if (!d) goto oom;
        d->key_kind = m->key_kind;
        d->val_kind = m->val_kind;
        d->len = m->len;
        for (uint32_t i = 0; i < m->len; i++) {
            d->kv[2 * i] = db_val_encode(classes, m->key_kind, m->keys[i], ok, msg);
            uint64_t dv = 0;
            if (*ok) dv = db_val_encode(classes, m->val_kind, m->vals[i], ok, msg);
            d->kv[2 * i + 1] = dv;
            if (!*ok) {
                for (uint32_t j = 0; j <= i; j++) {
                    db_val_free(d->key_kind, d->kv[2 * j]);
                    db_val_free(d->val_kind, d->kv[2 * j + 1]);
                }
                free(d);
                return 0;
            }
        }
        return (uint64_t)(uintptr_t)d;
    }
    default:
        *ok = 0;
        *msg = "a garbage-collected value cannot be stored in a table field";
        return 0;
    }
oom:
    *ok = 0;
    *msg = "out of memory encoding a row";
    return 0;
}

/* Decode one engine slot back into a fresh VM value (the out-gate: always
 * a copy). 0-with-*ok=0 = OOM; nil decodes as 0 with *ok=1. */
static uint64_t db_val_decode(wo_rt *rt, uint8_t kind, uint64_t v, int *ok,
                              const char **msg) {
    *ok = 1;
    switch (kind) {
    case WO_K_SCALAR:
    case WO_K_FLOAT: return v; /* iteration 19: bits back out unchanged */
    case WO_K_TEXT:
    case WO_K_BYTES: {
        if (!v) return 0;
        const db_text *t = (const db_text *)(uintptr_t)v;
        /* the out-gate decides the KIND: a Bytes column must hand back a
           Bytes, or a Text builtin would happily accept the row's value and
           the distinct type would be a fiction at the storage boundary */
        wo_str *s = kind == WO_K_BYTES ? wo_bytes_new(rt, t->bytes, t->len)
                                       : wo_str_new(rt, t->bytes, t->len);
        if (!s) goto oom;
        return (uint64_t)(uintptr_t)s;
    }
    case WO_K_OWNED: {
        if (!v) return 0;
        const db_rec *r = (const db_rec *)(uintptr_t)v;
        wo_hdr *o = wo_obj_new(rt, r->class_id);
        if (!o) goto oom;
        const wo_classdesc *c = &rt->classes[r->class_id];
        uint64_t *f = wo_fields(o);
        for (uint32_t i = 0; i < c->field_cnt; i++) {
            f[i] = db_val_decode(rt, c->kinds[i], r->slots[i], ok, msg);
            if (!*ok) return 0; /* partial object: rt teardown reclaims (test scope) */
        }
        return (uint64_t)(uintptr_t)o;
    }
    case WO_K_MULTI: {
        if (!v) return 0;
        const db_multi *d = (const db_multi *)(uintptr_t)v;
        wo_multi *m = wo_multi_new(rt, d->elem_kind);
        if (!m) goto oom;
        for (uint32_t i = 0; i < d->len; i++) {
            uint64_t ev = db_val_decode(rt, d->elem_kind, d->items[i], ok, msg);
            if (!*ok || wo_multi_push(m, ev) != 0) goto oom;
        }
        return (uint64_t)(uintptr_t)m;
    }
    case WO_K_MAP: {
        if (!v) return 0;
        const db_map *d = (const db_map *)(uintptr_t)v;
        wo_map *m = wo_map_new(rt, d->key_kind, d->val_kind);
        if (!m) goto oom;
        for (uint32_t i = 0; i < d->len; i++) {
            uint64_t kv = db_val_decode(rt, d->key_kind, d->kv[2 * i], ok, msg);
            uint64_t vv = 0;
            if (*ok) vv = db_val_decode(rt, d->val_kind, d->kv[2 * i + 1], ok, msg);
            uint64_t old;
            if (!*ok || wo_map_set(m, kv, vv, &old) < 0) goto oom;
        }
        return (uint64_t)(uintptr_t)m;
    }
    default: return 0; /* GCREF never stored, so never decoded */
    }
oom:
    *ok = 0;
    *msg = "out of memory decoding a row";
    return 0;
}

/* ---- id hash (open addressing, pow2, id -> global slot + 1) ----------- */

static uint64_t hmix(uint64_t x) { /* splitmix64 finalizer */
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

/* Ids are never 0 (0 spells "empty bucket") and never reused, so all-ones
 * can never collide with a live id — it marks a deleted bucket that probes
 * walk straight past. */
#define H_DELETED ((uint64_t)-1)

static int hgrow(db_table *t) {
    size_t ncap = t->hcap ? t->hcap * 2 : 64;
    uint64_t *nk = calloc(ncap, 8), *nv = calloc(ncap, 8);
    if (!nk || !nv) {
        free(nk);
        free(nv);
        return -1;
    }
    for (size_t i = 0; i < t->hcap; i++) {
        if (!t->hkeys[i] || t->hkeys[i] == H_DELETED) continue;
        size_t j = hmix(t->hkeys[i]) & (ncap - 1);
        while (nk[j]) j = (j + 1) & (ncap - 1);
        nk[j] = t->hkeys[i];
        nv[j] = t->hvals[i];
    }
    free(t->hkeys);
    free(t->hvals);
    t->hkeys = nk;
    t->hvals = nv;
    t->hcap = ncap;
    return 0;
}

static int hput(db_table *t, uint64_t id, uint64_t slot1) {
    if (t->hlen * 10 >= t->hcap * 7 && hgrow(t) != 0) return -1;
    size_t j = hmix(id) & (t->hcap - 1);
    while (t->hkeys[j] && t->hkeys[j] != id) j = (j + 1) & (t->hcap - 1);
    if (!t->hkeys[j]) t->hlen++;
    t->hkeys[j] = id;
    t->hvals[j] = slot1;
    return 0;
}

static uint64_t hget(const db_table *t, uint64_t id) {
    if (!t->hcap) return 0;
    size_t j = hmix(id) & (t->hcap - 1);
    while (t->hkeys[j]) {
        if (t->hkeys[j] == id) return t->hvals[j];
        j = (j + 1) & (t->hcap - 1);
    }
    return 0;
}

static void hdel(db_table *t, uint64_t id) {
    if (!t->hcap) return;
    size_t j = hmix(id) & (t->hcap - 1);
    while (t->hkeys[j]) {
        if (t->hkeys[j] == id) {
            t->hkeys[j] = H_DELETED;
            t->hvals[j] = 0;
            return;
        }
        j = (j + 1) & (t->hcap - 1);
    }
}

/* ---- tables and rows ---------------------------------------------------- */

/* ---- secondary indexes (Task 4) ---------------------------------------- */

/* iteration 19: an index key for a FLOAT column is the value's CANONICAL
 * bits, not its raw bits. Two values that the total order calls equal must
 * hash and compare equal, and raw bits break that twice: -0.0 and +0.0 are
 * equal but differ in the sign bit, and two NaNs with different payloads are
 * equal (both "last") but differ everywhere. Without this a `unique` Float
 * column would accept both -0.0 and 0.0, and a probe for one would miss a row
 * stored as the other. */
static uint64_t idx_float_key(uint64_t bits) {
    double d = wo_f64(bits);
    if (d != d) return 0x7FF8000000000000ull; /* every NaN -> the canonical one */
    if (d == 0.0) return 0;                   /* -0.0 -> +0.0 */
    return bits;
}

/* hash of one row's index columns: kind-driven, never trusted for equality */
static uint64_t idx_hash(const wo_classdesc *c, const db_index *ix, const db_row *r) {
    uint64_t h = 0x9e3779b97f4a7c15ull;
    for (uint32_t i = 0; i < ix->col_cnt; i++) {
        uint32_t col = ix->cols[i];
        uint64_t v = r->slots[col];
        if (c->kinds[col] == WO_K_TEXT) {
            const db_text *t = (const db_text *)(uintptr_t)v;
            uint64_t th = 1469598103934665603ull; /* FNV-1a over bytes; nil = 0 */
            if (t)
                for (uint32_t b = 0; b < t->len; b++) th = (th ^ (uint8_t)t->bytes[b]) * 1099511628211ull;
            else th = 0;
            v = th;
        }
        else if (c->kinds[col] == WO_K_FLOAT)
            v = idx_float_key(v); /* iteration 19 */
        h ^= hmix(v + i);
    }
    return h ? h : 1; /* 0 marks an empty bucket */
}

static db_ibucket *idx_bucket(db_index *ix, uint64_t h, int create);

/* One KEY's bucket hash — must reproduce idx_hash's result for a
 * single-column index bit for bit (same FNV, same float folding, same
 * position mix at i == 0), or probes and maintenance disagree on the
 * bucket and rows silently vanish from reads. */
static uint64_t idx_hash_key1(uint8_t kind, uint64_t key_scalar, const void *key_bytes,
                              uint32_t key_len) {
    uint64_t v;
    if (kind == WO_K_TEXT) {
        if (key_bytes) {
            uint64_t th = 1469598103934665603ull;
            const uint8_t *p = (const uint8_t *)key_bytes;
            for (uint32_t b = 0; b < key_len; b++) th = (th ^ p[b]) * 1099511628211ull;
            v = th;
        } else
            v = 0; /* nil text, exactly as idx_hash spells it */
    } else if (kind == WO_K_FLOAT)
        v = idx_float_key(key_scalar);
    else
        v = key_scalar;
    uint64_t h = 0x9e3779b97f4a7c15ull;
    h ^= hmix(v + 0);
    return h ? h : 1;
}

int wo_idx_probe(wo_db *db, uint32_t class_id, uint32_t index, uint64_t key_scalar,
                 const void *key_bytes, uint32_t key_len, uint64_t **out_ids,
                 uint32_t *out_cnt) {
    *out_ids = NULL;
    *out_cnt = 0;
    if (class_id >= db->class_cnt) return 0;
    db_table *t = &db->tables[class_id];
    if (!t->row_size || index >= t->index_cnt) return 0;
    db_index *ix = &t->indexes[index];
    if (ix->col_cnt != 1) return 0; /* composite: the caller keeps its scan */
    uint32_t col = ix->cols[0];
    uint8_t kind = db->classes[class_id].kinds[col];
    db_ibucket *b = idx_bucket(ix, idx_hash_key1(kind, key_scalar, key_bytes, key_len), 0);
    if (!b || !b->len) return 1; /* probed: genuinely empty */
    uint64_t *ids = malloc((size_t)b->len * 8u);
    if (!ids) return -1;
    uint32_t n = 0;
    for (uint32_t i = 0; i < b->len; i++) {
        /* databasev2 2 (5d): THE unique shadow — the site the plan called the
         * real coupling, because it needs a row it cannot get from a slab. For
         * a keys-resident table each candidate costs a pread and a
         * materialisation: the disclosed price of `@unique` there, bounded by
         * the bucket rather than the table. */
        db_row *r = wo_row_borrow(db, class_id, b->ids[i], NULL);
        if (!r) continue;
        int eq;
        if (kind == WO_K_TEXT) {
            const db_text *have = (const db_text *)(uintptr_t)r->slots[col];
            eq = (!key_bytes && !have) ||
                 (key_bytes && have && have->len == key_len &&
                  memcmp(have->bytes, key_bytes, key_len) == 0);
        } else
            /* raw-word equality for scalars AND floats — the slab walk's
             * exact comparison, so probe results never differ from scan
             * results (the hash canonicalized only to FIND the bucket) */
            eq = r->slots[col] == key_scalar;
        wo_row_release(db, class_id, r); /* before any use of the result */
        if (eq) ids[n++] = b->ids[i];
    }
    if (!n) {
        free(ids);
        return 1;
    }
    *out_ids = ids;
    *out_cnt = n;
    return 1;
}

static int idx_cols_equal(const wo_classdesc *c, const db_index *ix, const db_row *a,
                          const db_row *b) {
    for (uint32_t i = 0; i < ix->col_cnt; i++) {
        uint32_t col = ix->cols[i];
        if (c->kinds[col] == WO_K_TEXT) {
            const db_text *x = (const db_text *)(uintptr_t)a->slots[col];
            const db_text *y = (const db_text *)(uintptr_t)b->slots[col];
            if (!x || !y) {
                if (x != y) return 0;
            } else if (x->len != y->len || memcmp(x->bytes, y->bytes, x->len) != 0)
                return 0;
        } else if (c->kinds[col] == WO_K_FLOAT) {
            /* iteration 19: compare canonicalized, matching idx_hash */
            if (idx_float_key(a->slots[col]) != idx_float_key(b->slots[col])) return 0;
        } else if (a->slots[col] != b->slots[col])
            return 0;
    }
    return 1;
}

static db_ibucket *idx_bucket(db_index *ix, uint64_t h, int create) {
    if (ix->bcap == 0) {
        if (!create) return NULL;
        ix->buckets = calloc(64, sizeof(db_ibucket));
        if (!ix->buckets) return NULL;
        ix->bcap = 64;
    }
    if (create && ix->blen * 10 >= ix->bcap * 7) {
        size_t ncap = ix->bcap * 2;
        db_ibucket *nb = calloc(ncap, sizeof(db_ibucket));
        if (!nb) return NULL;
        for (size_t i = 0; i < ix->bcap; i++) {
            if (!ix->buckets[i].hash) continue;
            size_t j = ix->buckets[i].hash & (ncap - 1);
            while (nb[j].hash) j = (j + 1) & (ncap - 1);
            nb[j] = ix->buckets[i];
        }
        free(ix->buckets);
        ix->buckets = nb;
        ix->bcap = ncap;
    }
    size_t j = h & (ix->bcap - 1);
    while (ix->buckets[j].hash) {
        if (ix->buckets[j].hash == h) return &ix->buckets[j];
        j = (j + 1) & (ix->bcap - 1);
    }
    if (!create) return NULL;
    ix->buckets[j].hash = h;
    ix->blen++;
    return &ix->buckets[j];
}

/* Add [r] to every index; unique violation reports which without mutating
 * anything (checks run before any add). 0 ok, DB_ERR_* otherwise. */
static int idx_add_row(wo_db *db, db_table *t, db_row *r) {
    const wo_classdesc *c = &db->classes[t->class_id];
    for (uint32_t x = 0; x < t->index_cnt; x++) {
        db_index *ix = &t->indexes[x];
        if (!(ix->flags & 1u)) continue;
        db_ibucket *b = idx_bucket(ix, idx_hash(c, ix, r), 0);
        if (!b) continue;
        for (uint32_t i = 0; i < b->len; i++) {
            /* databasev2 2: borrow, never peek at a slab. For a keys-table the
             * conflicting row may not be resident, and a unique check that
             * silently skipped non-resident rows would be a correctness hole,
             * not a limitation. */
            const char *bmsg = "";
            db_row *other = wo_row_borrow(db, t->class_id, b->ids[i], &bmsg);
            int clash = other && idx_cols_equal(c, ix, r, other);
            wo_row_release(db, t->class_id, other);
            if (clash) return DB_ERR_UNIQUE;
        }
    }
    for (uint32_t x = 0; x < t->index_cnt; x++) {
        db_index *ix = &t->indexes[x];
        db_ibucket *b = idx_bucket(ix, idx_hash(c, ix, r), 1);
        if (!b) return DB_ERR_OOM;
        if (b->len == b->cap) {
            uint32_t ncap = b->cap ? b->cap * 2 : 4;
            uint64_t *ni = realloc(b->ids, (size_t)ncap * 8u);
            if (!ni) return DB_ERR_OOM;
            b->ids = ni;
            b->cap = ncap;
        }
        b->ids[b->len++] = r->id;
    }
    return 0;
}

static void idx_remove_row(wo_db *db, db_table *t, db_row *r) {
    const wo_classdesc *c = &db->classes[t->class_id];
    for (uint32_t x = 0; x < t->index_cnt; x++) {
        db_index *ix = &t->indexes[x];
        db_ibucket *b = idx_bucket(ix, idx_hash(c, ix, r), 0);
        if (!b) continue;
        for (uint32_t i = 0; i < b->len; i++)
            if (b->ids[i] == r->id) {
                b->ids[i] = b->ids[--b->len];
                break;
            }
    }
}

int wo_db_init(wo_db *db, const wo_classdesc *classes, uint32_t class_cnt,
               uint32_t shard, uint32_t nshards) {
    if (!nshards || shard >= nshards) return -1;
    memset(db, 0, sizeof(*db));
    db->classes = classes;
    db->class_cnt = class_cnt;
    db->shard = shard;
    db->nshards = nshards;
    db->tables = calloc(class_cnt ? class_cnt : 1, sizeof(db_table));
    if (!db->tables) return -1;
    g_classes = classes;
    return 0;
}

static void table_destroy(wo_db *db, db_table *t) {
    free(t->scratch); /* databasev2 2 */
    t->scratch = NULL;
    t->scratch_cap = 0;
    /* free every live row's engine-owned values, then the slabs */
    const wo_classdesc *c = &db->classes[t->class_id];
    for (uint32_t s = 0; s < t->slab_cnt; s++) {
        for (uint32_t i = 0; i < DB_SLAB_ROWS; i++) {
            uint32_t g = s * DB_SLAB_ROWS + i;
            if (!(t->bitmap[g >> 6] & (1ull << (g & 63)))) continue;
            db_row *r = (db_row *)(t->slabs[s] + (size_t)i * t->row_size);
            for (uint32_t f = 0; f < c->field_cnt; f++)
                db_val_free(c->kinds[f], r->slots[f]);
        }
        free(t->slabs[s]);
    }
    free(t->slabs);
    free(t->bitmap);
    free(t->free_slots);
    free(t->hkeys);
    free(t->hvals);
    for (uint32_t x = 0; x < t->index_cnt; x++) {
        for (size_t b = 0; b < t->indexes[x].bcap; b++) free(t->indexes[x].buckets[b].ids);
        free(t->indexes[x].buckets);
    }
    free(t->indexes);
}

void wo_db_destroy(wo_db *db) {
    if (!db->tables) return;
    for (uint32_t i = 0; i < db->class_cnt; i++)
        if (db->tables[i].slab_cnt || db->tables[i].hkeys) table_destroy(db, &db->tables[i]);
    free(db->tables);
    db->tables = NULL;
}

static db_table *table_of(wo_db *db, uint32_t class_id) {
    if (class_id >= db->class_cnt) return NULL;
    db_table *t = &db->tables[class_id];
    if (!t->row_size) { /* lazy init on first touch */
        const wo_classdesc *c = &db->classes[class_id];
        t->class_id = class_id;
        t->row_size = sizeof(db_row) + (size_t)c->field_cnt * 8u;
        t->next_id = db->shard + 1; /* S+1, then += N: interleaved, local-only */
        if (c->idx_cnt) {
            t->indexes = calloc(c->idx_cnt, sizeof(db_index));
            if (!t->indexes) return NULL;
            const uint32_t *im = c->idx_meta;
            for (uint32_t x = 0; x < c->idx_cnt; x++) {
                t->indexes[x].flags = im[0];
                t->indexes[x].col_cnt = im[1];
                t->indexes[x].cols = im + 2;
                im += 2 + im[1];
            }
            t->index_cnt = c->idx_cnt;
        }
    }
    return t;
}

static db_row *slot_row(db_table *t, uint32_t g) {
    return (db_row *)(t->slabs[g / DB_SLAB_ROWS] + (size_t)(g % DB_SLAB_ROWS) * t->row_size);
}

/* Pick the slot a new row lands in: recycled first, else the next free bit,
 * else grow a slab. Returns the global slot or UINT32_MAX on OOM. */
static uint32_t slot_alloc(db_table *t) {
    if (t->free_cnt) return t->free_slots[--t->free_cnt];
    uint32_t total = t->slab_cnt * DB_SLAB_ROWS;
    for (uint32_t g = 0; g < total; g++) /* cheap at slab granularity: only
        reached when free list is empty, and the bitmap scan is bounded by
        one word test per 64 slots */
        if (!(t->bitmap[g >> 6] & (1ull << (g & 63)))) return g;
    /* grow */
    if (t->slab_cnt == t->slab_cap) {
        uint32_t ncap = t->slab_cap ? t->slab_cap * 2 : 4;
        uint8_t **ns = realloc(t->slabs, (size_t)ncap * sizeof(uint8_t *));
        if (!ns) return UINT32_MAX;
        t->slabs = ns;
        t->slab_cap = ncap;
    }
    uint8_t *slab = malloc((size_t)DB_SLAB_ROWS * t->row_size);
    if (!slab) return UINT32_MAX;
    size_t nwords = ((size_t)(t->slab_cnt + 1) * DB_SLAB_ROWS + 63) / 64;
    uint64_t *nb = realloc(t->bitmap, nwords * 8);
    if (!nb) {
        free(slab);
        return UINT32_MAX;
    }
    memset(nb + ((size_t)t->slab_cnt * DB_SLAB_ROWS) / 64, 0,
           (nwords - ((size_t)t->slab_cnt * DB_SLAB_ROWS) / 64) * 8);
    t->bitmap = nb;
    t->slabs[t->slab_cnt] = slab;
    return t->slab_cnt++ * DB_SLAB_ROWS;
}

uint64_t wo_row_insert(wo_db *db, uint32_t class_id, const uint64_t *vals,
                       const char **msg, int *err_kind) {
    if (err_kind) *err_kind = DB_ERR_MISC;
    db_table *t = table_of(db, class_id);
    if (!t) {
        *msg = "no such class";
        return 0;
    }
    const wo_classdesc *c = &db->classes[class_id];
    uint32_t g = slot_alloc(t);
    if (g == UINT32_MAX) {
        *msg = "out of memory growing a table";
        return 0;
    }
    db_row *r = slot_row(t, g);
    r->class_id = class_id;
    r->flags = 0;
    int ok = 1;
    uint32_t i = 0;
    for (; i < c->field_cnt; i++) {
        r->slots[i] = db_val_encode(db->classes, c->kinds[i], vals[i], &ok, msg);
        if (!ok) {
            if (err_kind) *err_kind = DB_ERR_BADKIND;
            break;
        }
    }
    if (!ok) {
        for (uint32_t j = 0; j < i; j++) db_val_free(c->kinds[j], r->slots[j]);
        /* slot never became live: recycle it (bitmap bit was never set) */
        if (t->free_cnt == t->free_cap) {
            uint32_t ncap = t->free_cap ? t->free_cap * 2 : 16;
            uint32_t *nf = realloc(t->free_slots, (size_t)ncap * 4);
            if (nf) {
                t->free_slots = nf;
                t->free_cap = ncap;
            }
        }
        if (t->free_cnt < t->free_cap) t->free_slots[t->free_cnt++] = g;
        return 0;
    }
    r->id = t->next_id;
    t->next_id += db->nshards;
    if (hput(t, r->id, (uint64_t)g + 1) != 0) {
        for (uint32_t j = 0; j < c->field_cnt; j++) db_val_free(c->kinds[j], r->slots[j]);
        if (err_kind) *err_kind = DB_ERR_OOM;
        *msg = "out of memory indexing a row";
        return 0;
    }
    t->bitmap[g >> 6] |= 1ull << (g & 63);
    t->count++;
    /* THE index hook (Task 4): inside the choke point, never anywhere else.
       A unique violation un-applies the row entirely — id never handed out
       twice matters less than the row never having existed. */
    int irc = idx_add_row(db, t, r);
    if (irc != 0) {
        t->bitmap[g >> 6] &= ~(1ull << (g & 63));
        hdel(t, r->id);
        t->count--;
        t->next_id -= db->nshards; /* the id was never observable: reclaim it */
        for (uint32_t j = 0; j < c->field_cnt; j++) db_val_free(c->kinds[j], r->slots[j]);
        if (t->free_cnt < t->free_cap) t->free_slots[t->free_cnt++] = g;
        if (err_kind) *err_kind = irc;
        *msg = irc == DB_ERR_UNIQUE ? "unique index violation" : "out of memory indexing a row";
        return 0;
    }
    if (err_kind) *err_kind = DB_ERR_NONE;
    return r->id;
}

uint64_t wo_row_insert_slots(wo_db *db, uint32_t class_id, const uint64_t *slots,
                             const char **msg, int *err_kind) {
    if (err_kind) *err_kind = DB_ERR_MISC;
    db_table *t = table_of(db, class_id);
    const wo_classdesc *c = class_id < db->class_cnt ? &db->classes[class_id] : NULL;
    if (!t || !c) {
        /* slot kinds unknowable without the class: the values leak rather
           than die by the wrong kind (defensive; the requester validated) */
        *msg = "no such class";
        return 0;
    }
    uint32_t g = slot_alloc(t);
    if (g == UINT32_MAX) {
        for (uint32_t j = 0; j < c->field_cnt; j++) db_val_free(c->kinds[j], slots[j]);
        if (err_kind) *err_kind = DB_ERR_OOM;
        *msg = "out of memory growing a table";
        return 0;
    }
    db_row *r = slot_row(t, g);
    r->class_id = class_id;
    r->flags = 0;
    memcpy(r->slots, slots, (size_t)c->field_cnt * 8u);
    r->id = t->next_id;
    t->next_id += db->nshards;
    if (hput(t, r->id, (uint64_t)g + 1) != 0) {
        for (uint32_t j = 0; j < c->field_cnt; j++) db_val_free(c->kinds[j], r->slots[j]);
        t->next_id -= db->nshards;
        if (t->free_cnt < t->free_cap) t->free_slots[t->free_cnt++] = g;
        if (err_kind) *err_kind = DB_ERR_OOM;
        *msg = "out of memory indexing a row";
        return 0;
    }
    t->bitmap[g >> 6] |= 1ull << (g & 63);
    t->count++;
    int irc = idx_add_row(db, t, r);
    if (irc != 0) {
        t->bitmap[g >> 6] &= ~(1ull << (g & 63));
        hdel(t, r->id);
        t->count--;
        t->next_id -= db->nshards; /* the id was never observable: reclaim it */
        for (uint32_t j = 0; j < c->field_cnt; j++) db_val_free(c->kinds[j], r->slots[j]);
        if (t->free_cnt < t->free_cap) t->free_slots[t->free_cnt++] = g;
        if (err_kind) *err_kind = irc;
        *msg = irc == DB_ERR_UNIQUE ? "unique index violation" : "out of memory indexing a row";
        return 0;
    }
    if (err_kind) *err_kind = DB_ERR_NONE;
    return r->id;
}

db_row *wo_row_ptr(wo_db *db, uint32_t class_id, uint64_t id) {
    if (class_id >= db->class_cnt) return NULL;
    db_table *t = &db->tables[class_id];
    if (!t->row_size) return NULL;
    uint64_t s1 = hget(t, id);
    if (!s1) return NULL;
    return slot_row(t, (uint32_t)(s1 - 1));
}

db_row *wo_row_borrow(wo_db *db, uint32_t class_id, uint64_t id, const char **msg) {
    /* Fully-resident tables: exactly today's lookup, and releasing is a no-op.
     * The hot path pays one predicate. */
    if (!wo_table_is_keys_resident(db, class_id)) return wo_row_ptr(db, class_id, id);

    /* Keys-resident: the id map holds the record's LOG OFFSET (off + 1), not a
     * slot, so the row is materialised into the table's scratch. */
    db_table *t = &db->tables[class_id];
    if (!t->row_size) return NULL;
    uint64_t o1 = hget(t, id);
    if (!o1) return NULL;
    if (!db->rt || !db->rt->wal) {
        /* a keys-resident table cannot exist without a log to read from; the
         * loader refuses the annotation outright, so this is a defensive arm */
        if (msg) *msg = "resident: keys table without a write-ahead log";
        return NULL;
    }
    if (t->scratch_busy) {
        /* One scratch per TABLE, so two live borrows on the same table would
         * hand back the same buffer. The unique shadow borrows one candidate
         * at a time, which is why per-table is enough — but say so rather than
         * corrupting the first borrow silently. */
        if (msg) *msg = "nested borrow on one table";
        return NULL;
    }
    if (t->scratch_cap < t->row_size) {
        uint8_t *nb = realloc(t->scratch, t->row_size);
        if (!nb) {
            if (msg) *msg = "out of memory";
            return NULL;
        }
        t->scratch = nb;
        t->scratch_cap = t->row_size;
    }
    db_row *r = (db_row *)t->scratch;
    uint32_t got_cid = 0;
    uint64_t got_id = 0;
    if (wo_wal_read_row_at((wo_wal *)db->rt->wal, db, db->rt, o1 - 1, &got_cid, &got_id,
                           r->slots, msg) != 0)
        return NULL;
    if (got_cid != class_id || got_id != id) {
        /* the offset pointed at someone else's record — a compaction that
         * moved records without rebuilding this map would land here, which is
         * exactly the obligation recorded at wo_wal_compact */
        if (msg) *msg = "log offset does not hold the expected row";
        return NULL;
    }
    r->id = id;
    r->class_id = class_id;
    r->flags = 0;
    t->scratch_busy = 1;
    return r;
}

void wo_row_release(wo_db *db, uint32_t class_id, db_row *r) {
    if (!r || class_id >= db->class_cnt) return;
    db_table *t = &db->tables[class_id];
    if (!t->scratch_busy || (uint8_t *)r != t->scratch) return; /* slab-backed */
    const wo_classdesc *c = &db->classes[class_id];
    /* These are VM values, not engine values. wo_wal_read_row_at is the
     * out-gate — it always COPIES, producing fresh runtime allocations — so
     * they must be dropped through the runtime. Freeing them with the engine's
     * allocator (as this did while the materialising path was still a stub)
     * is a bad-free the moment a keys-resident row is actually read back. */
    if (db->rt)
        for (uint32_t i = 0; i < c->field_cnt; i++) wo_drop_kind(db->rt, c->kinds[i], r->slots[i]);
    t->scratch_busy = 0;
}

int wo_row_read(wo_db *db, wo_rt *rt, uint32_t class_id, uint64_t id,
                uint64_t *out_vals, const char **msg) {
    db_row *r = wo_row_borrow(db, class_id, id, msg);
    if (!r) return -1;
    const wo_classdesc *c = &db->classes[class_id];
    int ok = 1;
    for (uint32_t i = 0; i < c->field_cnt; i++) {
        /* decode out of the row BEFORE releasing: a keys-resident row's slots
         * point into the scratch that release frees */
        out_vals[i] = db_val_decode(rt, c->kinds[i], r->slots[i], &ok, msg);
        if (!ok) {
            wo_row_release(db, class_id, r);
            return -2;
        }
    }
    wo_row_release(db, class_id, r);
    return 0;
}

db_row *wo_row_create_raw(wo_db *db, uint32_t class_id, uint64_t id) {
    db_table *t = table_of(db, class_id);
    if (!t || !id) return NULL;
    if (hget(t, id)) return NULL; /* duplicate id: corruption, not a tear */
    uint32_t g = slot_alloc(t);
    if (g == UINT32_MAX) return NULL;
    db_row *r = slot_row(t, g);
    r->id = id;
    r->class_id = class_id;
    r->flags = 0;
    memset(r->slots, 0, t->row_size - sizeof(db_row));
    if (hput(t, id, (uint64_t)g + 1) != 0) return NULL;
    t->bitmap[g >> 6] |= 1ull << (g & 63);
    t->count++;
    /* keep the interleave: only ids this shard owns move its counter */
    if ((id - 1) % db->nshards == db->shard && id >= t->next_id)
        t->next_id = id + db->nshards;
    /* indexes: NOT here — the slots are still zero. wal.c fills them and
       then calls wo_row_raw_commit, which is where replayed rows re-index. */
    return r;
}

int wo_row_raw_commit(wo_db *db, uint32_t class_id, db_row *r) {
    db_table *t = &db->tables[class_id];
    return idx_add_row(db, t, r) == 0 ? 0 : -1;
}

void wo_db_val_free(wo_db *db, uint8_t kind, uint64_t v) {
    (void)db;
    db_val_free(kind, v);
}

uint64_t wo_db_val_encode(const wo_classdesc *classes, uint8_t kind, uint64_t vm_val,
                          int *ok, const char **msg) {
    return db_val_encode(classes, kind, vm_val, ok, msg);
}

/* Deep engine-to-engine copy; shapes mirror db_val_free's recursion. */
uint64_t wo_db_val_clone(const wo_classdesc *classes, uint8_t kind, uint64_t v, int *ok) {
    *ok = 1;
    if (!v) return 0;
    switch (kind) {
    case WO_K_SCALAR:
    case WO_K_FLOAT: return v;
    case WO_K_TEXT:
    case WO_K_BYTES: {
        const db_text *s = (const db_text *)(uintptr_t)v;
        db_text *t = malloc(sizeof(db_text) + s->len);
        if (!t) goto oom;
        t->len = s->len;
        memcpy(t->bytes, s->bytes, s->len);
        return (uint64_t)(uintptr_t)t;
    }
    case WO_K_OWNED: {
        const db_rec *s = (const db_rec *)(uintptr_t)v;
        const wo_classdesc *c = &classes[s->class_id];
        db_rec *r = malloc(sizeof(db_rec) + (size_t)c->field_cnt * 8u);
        if (!r) goto oom;
        r->class_id = s->class_id;
        r->_pad = 0;
        for (uint32_t i = 0; i < c->field_cnt; i++) {
            r->slots[i] = wo_db_val_clone(classes, c->kinds[i], s->slots[i], ok);
            if (!*ok) {
                for (uint32_t j = 0; j < i; j++) db_val_free(c->kinds[j], r->slots[j]);
                free(r);
                return 0;
            }
        }
        return (uint64_t)(uintptr_t)r;
    }
    case WO_K_MULTI: {
        const db_multi *s = (const db_multi *)(uintptr_t)v;
        db_multi *d = malloc(sizeof(db_multi) + (size_t)s->len * 8u);
        if (!d) goto oom;
        d->elem_kind = s->elem_kind;
        d->len = s->len;
        for (uint32_t i = 0; i < s->len; i++) {
            d->items[i] = wo_db_val_clone(classes, s->elem_kind, s->items[i], ok);
            if (!*ok) {
                for (uint32_t j = 0; j < i; j++) db_val_free(d->elem_kind, d->items[j]);
                free(d);
                return 0;
            }
        }
        return (uint64_t)(uintptr_t)d;
    }
    case WO_K_MAP: {
        const db_map *s = (const db_map *)(uintptr_t)v;
        db_map *d = malloc(sizeof(db_map) + (size_t)s->len * 16u);
        if (!d) goto oom;
        d->key_kind = s->key_kind;
        d->val_kind = s->val_kind;
        d->len = s->len;
        for (uint32_t i = 0; i < s->len; i++) {
            d->kv[2 * i] = wo_db_val_clone(classes, s->key_kind, s->kv[2 * i], ok);
            uint64_t dv = 0;
            if (*ok) dv = wo_db_val_clone(classes, s->val_kind, s->kv[2 * i + 1], ok);
            d->kv[2 * i + 1] = dv;
            if (!*ok) {
                for (uint32_t j = 0; j <= i; j++) {
                    db_val_free(d->key_kind, d->kv[2 * j]);
                    db_val_free(d->val_kind, d->kv[2 * j + 1]);
                }
                free(d);
                return 0;
            }
        }
        return (uint64_t)(uintptr_t)d;
    }
    default: return v; /* GCREF never stored; nothing to clone */
    }
oom:
    *ok = 0;
    return 0;
}

uint64_t wo_val_decode_vm(wo_db *db, wo_rt *rt, uint8_t kind, uint64_t engine_val,
                          int *ok, const char **msg) {
    (void)db;
    return db_val_decode(rt, kind, engine_val, ok, msg);
}

static int row_apply_field_slot(wo_db *db, db_table *t, const wo_classdesc *c,
                                db_row *r, uint32_t class_id, uint64_t id,
                                uint32_t field, uint64_t nv, const char **msg,
                                int *err_kind);

int wo_row_update_field(wo_db *db, uint32_t class_id, uint64_t id, uint32_t field,
                        uint64_t vm_val, const char **msg, int *err_kind) {
    if (err_kind) *err_kind = DB_ERR_MISC;
    /* databasev2 2 (5d): a keys-resident row lives in the LOG, so there is no
     * slab slot to mutate — writing into the borrow's scratch would discard
     * the update silently, which is the one failure mode this iteration must
     * not ship. Updating such a row means read-modify-APPEND (a new record,
     * then re-point the offset), and that is not built yet. Refuse loudly.
     * The loader refuses `resident: keys` outright, so this is defence in
     * depth and a marker for the next implementer. */
    if (wo_table_is_keys_resident(db, class_id)) {
        *msg = "update on a `resident: keys` table is not implemented";
        return -1;
    }
    db_row *r = wo_row_ptr(db, class_id, id);
    if (!r) {
        *msg = "no such row";
        return -1;
    }
    const wo_classdesc *c = &db->classes[class_id];
    if (field >= c->field_cnt) {
        *msg = "no such field";
        return -1;
    }
    db_table *t = &db->tables[class_id];
    int ok = 1;
    uint64_t nv = db_val_encode(db->classes, c->kinds[field], vm_val, &ok, msg);
    if (!ok) {
        if (err_kind) *err_kind = DB_ERR_BADKIND;
        return -1;
    }
    return row_apply_field_slot(db, t, c, r, class_id, id, field, nv, msg, err_kind);
}

/* The post-encode half of an update: unique shadow-check, index fix-up,
 * slot swap. Consumes [nv] (installed on success, freed on failure) —
 * shared by the VM-value wrapper above and the RPC slot path. */
static int row_apply_field_slot(wo_db *db, db_table *t, const wo_classdesc *c,
                                db_row *r, uint32_t class_id, uint64_t id,
                                uint32_t field, uint64_t nv, const char **msg,
                                int *err_kind) {
    /* indexes containing this column: unique checks against the NEW value
       run first, against a shadow of the row, before anything mutates */
    uint64_t old = r->slots[field];
    r->slots[field] = nv;
    for (uint32_t x = 0; x < t->index_cnt; x++) {
        db_index *ix = &t->indexes[x];
        if (!(ix->flags & 1u)) continue;
        int touches = 0;
        for (uint32_t i = 0; i < ix->col_cnt; i++)
            if (ix->cols[i] == field) touches = 1;
        if (!touches) continue;
        db_ibucket *b = idx_bucket(ix, idx_hash(c, ix, r), 0);
        if (!b) continue;
        for (uint32_t i = 0; i < b->len; i++) {
            if (b->ids[i] == id) continue;
            const char *bmsg = "";
            db_row *other = wo_row_borrow(db, class_id, b->ids[i], &bmsg);
            int clash = other && idx_cols_equal(c, ix, r, other);
            wo_row_release(db, class_id, other);
            if (clash) {
                r->slots[field] = old; /* untouched, promised */
                db_val_free(c->kinds[field], nv);
                if (err_kind) *err_kind = DB_ERR_UNIQUE;
                *msg = "unique index violation";
                return -1;
            }
        }
    }
    /* commit: fix every index containing the column (old entry out under
       the OLD value's hash, new entry in), then free the old value */
    r->slots[field] = old;
    for (uint32_t x = 0; x < t->index_cnt; x++) {
        db_index *ix = &t->indexes[x];
        int touches = 0;
        for (uint32_t i = 0; i < ix->col_cnt; i++)
            if (ix->cols[i] == field) touches = 1;
        if (!touches) continue;
        db_ibucket *b = idx_bucket(ix, idx_hash(c, ix, r), 0);
        if (b)
            for (uint32_t i = 0; i < b->len; i++)
                if (b->ids[i] == id) {
                    b->ids[i] = b->ids[--b->len];
                    break;
                }
    }
    r->slots[field] = nv;
    for (uint32_t x = 0; x < t->index_cnt; x++) {
        db_index *ix = &t->indexes[x];
        int touches = 0;
        for (uint32_t i = 0; i < ix->col_cnt; i++)
            if (ix->cols[i] == field) touches = 1;
        if (!touches) continue;
        db_ibucket *b = idx_bucket(ix, idx_hash(c, ix, r), 1);
        if (b) {
            if (b->len == b->cap) {
                uint32_t ncap = b->cap ? b->cap * 2 : 4;
                uint64_t *ni = realloc(b->ids, (size_t)ncap * 8u);
                if (ni) {
                    b->ids = ni;
                    b->cap = ncap;
                }
            }
            if (b->len < b->cap) b->ids[b->len++] = id;
        }
    }
    db_val_free(c->kinds[field], old);
    if (err_kind) *err_kind = DB_ERR_NONE;
    return 0;
}

int wo_row_update_field_slot(wo_db *db, uint32_t class_id, uint64_t id, uint32_t field,
                             uint64_t slot, const char **msg, int *err_kind) {
    if (err_kind) *err_kind = DB_ERR_MISC;
    /* databasev2 2 (5d): same reason as wo_row_update_field — a keys-resident
     * row has no slab slot to mutate, and writing into the borrow's scratch
     * would discard the update silently. Read-modify-APPEND is the shape that
     * works, and it is not built yet. */
    if (wo_table_is_keys_resident(db, class_id)) {
        *msg = "update on a `resident: keys` table is not implemented";
        return -1;
    }
    /* bounds first: the RPC requester validated cid/field to encode at all,
       so these are defensive; the slot's kind is unknowable on a class
       violation and the value leaks rather than dies by the wrong kind */
    if (class_id >= db->class_cnt) {
        *msg = "no such class";
        return -1;
    }
    const wo_classdesc *c = &db->classes[class_id];
    if (field >= c->field_cnt) {
        *msg = "no such field";
        return -1;
    }
    db_row *r = wo_row_ptr(db, class_id, id);
    if (!r) {
        db_val_free(c->kinds[field], slot);
        *msg = "no such row";
        return -1;
    }
    return row_apply_field_slot(db, &db->tables[class_id], c, r, class_id, id,
                                field, slot, msg, err_kind);
}

int wo_row_has_referrers(wo_db *db, uint32_t class_id, uint64_t id) {
    if (!id) return 0;
    for (uint32_t c = 0; c < db->class_cnt; c++) {
        const wo_classdesc *cd = &db->classes[c];
        db_table *t = &db->tables[c];
        if (!t->row_size || !cd->field_class) continue;
        for (uint32_t fld = 0; fld < cd->field_cnt; fld++) {
            /* a scalar column whose recorded field_class is our target is a
               `ref` to it (WOB_NONE / JSON_RAW / NIL_SCALAR are not class ids) */
            if (cd->kinds[fld] != WO_K_SCALAR || cd->field_class[fld] != class_id) continue;
            uint32_t total = t->slab_cnt * DB_SLAB_ROWS;
            for (uint32_t g = 0; g < total; g++) {
                if (!(t->bitmap[g >> 6] & (1ull << (g & 63)))) continue;
                db_row *r = (db_row *)(t->slabs[g / DB_SLAB_ROWS] +
                                       (size_t)(g % DB_SLAB_ROWS) * t->row_size);
                if (r->slots[fld] == id) return 1;
            }
        }
    }
    return 0;
}

int wo_row_next_id(const wo_db *db, uint32_t class_id, size_t *cursor, uint64_t *id_out) {
    if (class_id >= db->class_cnt) return 0;
    const db_table *t = &db->tables[class_id];
    if (!t->row_size) return 0;
    if (wo_table_is_keys_resident(db, class_id)) {
        /* the id map IS the live set here: hkeys non-zero, hvals holding an
         * offset + 1 */
        for (size_t j = *cursor; j < t->hcap; j++) {
            if (t->hkeys[j] && t->hvals[j]) {
                *id_out = t->hkeys[j];
                *cursor = j + 1;
                return 1;
            }
        }
        *cursor = t->hcap;
        return 0;
    }
    {   /* resident: the bitmap, in slab order, exactly as before */
        uint32_t total = t->slab_cnt * DB_SLAB_ROWS;
        for (size_t g = *cursor; g < total; g++) {
            if (!(t->bitmap[g >> 6] & (1ull << (g & 63)))) continue;
            *id_out = slot_row((db_table *)t, (uint32_t)g)->id;
            *cursor = g + 1;
            return 1;
        }
        *cursor = total;
        return 0;
    }
}

int wo_table_is_keys_resident(const wo_db *db, uint32_t class_id) {
    if (class_id >= db->class_cnt) return 0;
    return (db->classes[class_id].flags & WO_CLASSF_RESIDENT_KEYS) != 0u;
}

int wo_row_drop_payload(wo_db *db, uint32_t class_id, uint64_t id, uint64_t wal_off) {
    if (class_id >= db->class_cnt) return -1;
    db_table *t = &db->tables[class_id];
    if (!t->row_size) return -1;
    uint64_t s1 = hget(t, id);
    if (!s1) return -1;
    uint32_t g = (uint32_t)(s1 - 1);
    db_row *r = slot_row(t, g);
    /* the values are engine-owned; the log holds their bytes now */
    const wo_classdesc *c = &db->classes[class_id];
    for (uint32_t i = 0; i < c->field_cnt; i++) db_val_free(c->kinds[i], r->slots[i]);
    t->bitmap[g >> 6] &= ~(1ull << (g & 63));
    /* the id STAYS, now pointing at the log rather than at a slab. No
     * idx_remove_row and no count change: the row is live, only its backing
     * moved. */
    if (hput(t, id, wal_off + 1) != 0) return -1;
    if (t->free_cnt == t->free_cap) {
        uint32_t ncap = t->free_cap ? t->free_cap * 2 : 16;
        uint32_t *nf = realloc(t->free_slots, (size_t)ncap * 4);
        if (!nf) return 0; /* slot simply not recycled; the bitmap still frees it */
        t->free_slots = nf;
        t->free_cap = ncap;
    }
    t->free_slots[t->free_cnt++] = g;
    return 0;
}

int wo_row_remove(wo_db *db, uint32_t class_id, uint64_t id) {
    if (class_id >= db->class_cnt) return -1;
    db_table *t = &db->tables[class_id];
    if (!t->row_size) return -1;
    uint64_t s1 = hget(t, id);
    if (!s1) return -1;
    uint32_t g = (uint32_t)(s1 - 1);
    db_row *r = slot_row(t, g);
    /* the index hook's remove side: before the row's values die, while the
       columns are still comparable */
    idx_remove_row(db, t, r);
    const wo_classdesc *c = &db->classes[class_id];
    for (uint32_t i = 0; i < c->field_cnt; i++) db_val_free(c->kinds[i], r->slots[i]);
    t->bitmap[g >> 6] &= ~(1ull << (g & 63));
    hdel(t, id);
    t->count--;
    if (t->free_cnt == t->free_cap) {
        uint32_t ncap = t->free_cap ? t->free_cap * 2 : 16;
        uint32_t *nf = realloc(t->free_slots, (size_t)ncap * 4);
        if (!nf) return 0; /* slot simply not recycled; bitmap still frees it */
        t->free_slots = nf;
        t->free_cap = ncap;
    }
    t->free_slots[t->free_cnt++] = g;
    return 0;
}

/* databasev2 2 (5d): re-point a keys-resident row at a NEW log offset.
 *
 * Deliberately not hput(): hput runs the load-factor check and can rehash,
 * which would reorder hkeys/hvals underneath a wo_row_next_id cursor. This
 * only ever overwrites the value of a key that already exists, so the table's
 * shape cannot change and a walk in progress stays valid. That property is
 * what lets compaction re-point rows as it writes them instead of buffering
 * one (cid, id, offset) triple per live row. Returns -1 if the id is absent. */
int wo_row_set_offset(wo_db *db, uint32_t class_id, uint64_t id, uint64_t wal_off) {
    if (class_id >= db->class_cnt) return -1;
    db_table *t = &db->tables[class_id];
    if (!t->hcap) return -1;
    size_t j = hmix(id) & (t->hcap - 1);
    while (t->hkeys[j]) {
        if (t->hkeys[j] == id) {
            t->hvals[j] = wal_off + 1;
            return 0;
        }
        j = (j + 1) & (t->hcap - 1);
    }
    return -1;
}

/* databasev2 2 (5d): the log offset a keys-resident row currently reads from,
 * as stored (off + 1), so 0 means "no such row". Compaction needs the raw
 * offset to copy the record without materialising it. */
uint64_t wo_row_offset1(const wo_db *db, uint32_t class_id, uint64_t id) {
    if (class_id >= db->class_cnt) return 0;
    const db_table *t = &db->tables[class_id];
    if (!t->hcap) return 0;
    return hget(t, id);
}

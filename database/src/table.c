#include "table.h"

#include <stdlib.h>
#include <string.h>

#include "cont.h"

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
    case WO_K_SCALAR: return;
    case WO_K_TEXT: free((db_text *)(uintptr_t)v); return;
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
    case WO_K_SCALAR: return v;
    case WO_K_TEXT: {
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
    case WO_K_SCALAR: return v;
    case WO_K_TEXT: {
        if (!v) return 0;
        const db_text *t = (const db_text *)(uintptr_t)v;
        wo_str *s = wo_str_new(rt, t->bytes, t->len);
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
        t->class_id = class_id;
        t->row_size = sizeof(db_row) + (size_t)db->classes[class_id].field_cnt * 8u;
        t->next_id = db->shard + 1; /* S+1, then += N: interleaved, local-only */
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
                       const char **msg) {
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
        if (!ok) break;
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
        *msg = "out of memory indexing a row";
        return 0;
    }
    t->bitmap[g >> 6] |= 1ull << (g & 63);
    t->count++;
    /* INDEX HOOK (Task 4): secondary indexes update here, inside the choke
       point, never anywhere else. */
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

int wo_row_read(wo_db *db, wo_rt *rt, uint32_t class_id, uint64_t id,
                uint64_t *out_vals, const char **msg) {
    db_row *r = wo_row_ptr(db, class_id, id);
    if (!r) return -1;
    const wo_classdesc *c = &db->classes[class_id];
    int ok = 1;
    for (uint32_t i = 0; i < c->field_cnt; i++) {
        out_vals[i] = db_val_decode(rt, c->kinds[i], r->slots[i], &ok, msg);
        if (!ok) return -2;
    }
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
    /* INDEX HOOK (Task 4): replayed rows re-index here, same as inserts —
       the caller fills slots BEFORE indexes exist on them (Task 4 will move
       the hook to a post-fill call, recorded in the binding doc). */
    return r;
}

void wo_db_val_free(wo_db *db, uint8_t kind, uint64_t v) {
    (void)db;
    db_val_free(kind, v);
}

int wo_row_remove(wo_db *db, uint32_t class_id, uint64_t id) {
    if (class_id >= db->class_cnt) return -1;
    db_table *t = &db->tables[class_id];
    if (!t->row_size) return -1;
    uint64_t s1 = hget(t, id);
    if (!s1) return -1;
    uint32_t g = (uint32_t)(s1 - 1);
    db_row *r = slot_row(t, g);
    /* INDEX HOOK (Task 4): secondary indexes remove here, before the row's
       values die. */
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

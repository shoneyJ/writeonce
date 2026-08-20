#include "cont.h"

#include <stdlib.h>
#include <string.h>

wo_multi *wo_multi_new(wo_rt *rt, uint8_t elem_kind) {
    wo_multi *m = wo_arena_alloc(&rt->arena, sizeof(wo_multi));
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));
    m->h.class_id = WO_CLS_MULTI;
    m->h.shard_id = rt->shard_id;
    m->elem_kind = elem_kind;
    return m;
}

int wo_multi_push(wo_multi *m, uint64_t v) {
    if (m->len == m->cap) {
        uint32_t ncap = m->cap ? m->cap * 2 : 8;
        uint64_t *ni = realloc(m->items, (size_t)ncap * sizeof(uint64_t));
        if (!ni) return -1;
        m->items = ni;
        m->cap = ncap;
    }
    m->items[m->len++] = v;
    return 0;
}

int wo_multi_get(const wo_multi *m, uint64_t idx, uint64_t *out) {
    if (idx >= m->len) return -1;
    *out = m->items[idx];
    return 0;
}

wo_map *wo_map_new(wo_rt *rt, uint8_t key_kind, uint8_t val_kind) {
    wo_map *m = wo_arena_alloc(&rt->arena, sizeof(wo_map));
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));
    m->h.class_id = WO_CLS_MAP;
    m->h.shard_id = rt->shard_id;
    m->key_kind = key_kind;
    m->val_kind = val_kind;
    return m;
}

static int key_eq(const wo_map *m, uint64_t a, uint64_t b) {
    if (m->key_kind == WO_K_TEXT)
        return wo_str_eq((const wo_str *)(uintptr_t)a,
                         (const wo_str *)(uintptr_t)b);
    return a == b;
}

static int map_find(const wo_map *m, uint64_t k) {
    for (uint32_t i = 0; i < m->len; i++)
        if (key_eq(m, m->keys[i], k)) return (int)i;
    return -1;
}

int wo_map_set(wo_map *m, uint64_t k, uint64_t v, uint64_t *old) {
    int i = map_find(m, k);
    if (i >= 0) {
        *old = m->vals[i];
        m->vals[i] = v;
        return 1;
    }
    if (m->len == m->cap) {
        uint32_t ncap = m->cap ? m->cap * 2 : 8;
        uint64_t *nk = realloc(m->keys, (size_t)ncap * sizeof(uint64_t));
        if (!nk) return -1;
        m->keys = nk;
        uint64_t *nv = realloc(m->vals, (size_t)ncap * sizeof(uint64_t));
        if (!nv) return -1;
        m->vals = nv;
        m->cap = ncap;
    }
    m->keys[m->len] = k;
    m->vals[m->len] = v;
    m->len++;
    return 0;
}

int wo_map_get(const wo_map *m, uint64_t k, uint64_t *out) {
    int i = map_find(m, k);
    if (i < 0) return -1;
    *out = m->vals[i];
    return 0;
}

int wo_map_has(const wo_map *m, uint64_t k) { return map_find(m, k) >= 0; }

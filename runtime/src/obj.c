#include "obj.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t round16(size_t n) { return (n + 15u) & ~(size_t)15u; }

int wo_arena_init(wo_arena *a, size_t cap) {
    memset(a, 0, sizeof(*a));
    a->base = malloc(cap ? cap : 1);
    if (!a->base) return -1;
    a->cap = cap;
    return 0;
}

void wo_arena_destroy(wo_arena *a) {
    free(a->base);
    memset(a, 0, sizeof(*a));
}

void *wo_arena_alloc(wo_arena *a, size_t size) {
    size = round16(size ? size : 1);
    if (size > WO_ARENA_MAX_CLASS) return malloc(size);
    size_t cls = size / 16u - 1u;
    if (a->freelist[cls]) {
        void *p = a->freelist[cls];
        memcpy(&a->freelist[cls], p, sizeof(void *));
        return p;
    }
    if (a->used + size > a->cap) return NULL; /* region OOM -> trap upstream */
    void *p = a->base + a->used;
    a->used += size;
    return p;
}

void wo_arena_free(wo_arena *a, void *p, size_t size) {
    if (!p) return;
    size = round16(size ? size : 1);
    if (size > WO_ARENA_MAX_CLASS) {
        free(p);
        return;
    }
    size_t cls = size / 16u - 1u;
    memcpy(p, &a->freelist[cls], sizeof(void *));
    a->freelist[cls] = p;
}

int wo_rt_init(wo_rt *rt, size_t heap_cap, const wo_classdesc *classes,
               uint32_t class_cnt) {
    memset(rt, 0, sizeof(*rt));
    if (wo_arena_init(&rt->arena, heap_cap) != 0) return -1;
    rt->classes = classes;
    rt->class_cnt = class_cnt;
    rt->out = stdout;
    return 0;
}

void wo_rt_destroy(wo_rt *rt) {
    free(rt->cycbuf.items);
    wo_arena_destroy(&rt->arena);
    memset(rt, 0, sizeof(*rt));
}

wo_hdr *wo_obj_new(wo_rt *rt, uint32_t class_id) {
    const wo_classdesc *c = &rt->classes[class_id];
    size_t sz = wo_obj_size(c);
    wo_hdr *o = wo_arena_alloc(&rt->arena, sz);
    if (!o) return NULL;
    memset(o, 0, sz);
    o->class_id = class_id;
    if (c->flags & WO_CLASSF_GC) {
        o->flags = WO_F_GC;
        o->rc = 1; /* the creating reference */
    }
    return o;
}

static wo_str *str_alloc(wo_rt *rt, uint32_t len) {
    wo_str *s = wo_arena_alloc(&rt->arena, sizeof(wo_str) + len);
    if (!s) return NULL;
    memset(&s->h, 0, sizeof(s->h));
    s->h.class_id = WO_CLS_STR;
    s->len = len;
    return s;
}

wo_str *wo_str_new(wo_rt *rt, const char *bytes, uint32_t len) {
    wo_str *s = str_alloc(rt, len);
    if (!s) return NULL;
    memcpy(s->data, bytes, len);
    return s;
}

wo_str *wo_str_concat(wo_rt *rt, const wo_str *a, const wo_str *b) {
    wo_str *s = str_alloc(rt, a->len + b->len);
    if (!s) return NULL;
    memcpy(s->data, a->data, a->len);
    memcpy(s->data + a->len, b->data, b->len);
    return s;
}

int wo_str_eq(const wo_str *a, const wo_str *b) {
    return a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
}

void wo_str_free(wo_rt *rt, wo_str *s) {
    if (!s || (s->h.flags & WO_F_CONST)) return; /* interned: outlives all */
    wo_arena_free(&rt->arena, s, sizeof(wo_str) + s->len);
}

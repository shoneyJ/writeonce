/* test_arena — size-class reuse, region OOM, malloc fallback above 1024. */
#include "obj.h"
#include "t.h"

static void test_size_class_reuse(void) {
    wo_arena a;
    T_EQ(wo_arena_init(&a, 4096), 0);
    void *p = wo_arena_alloc(&a, 48);
    T_CHECK(p != NULL);
    wo_arena_free(&a, p, 48);
    /* same size class: the freed block must come straight back */
    void *q = wo_arena_alloc(&a, 48);
    T_CHECK(q == p);
    wo_arena_free(&a, q, 48);
    wo_arena_destroy(&a);
}

static void test_region_oom_returns_null(void) {
    wo_arena a;
    T_EQ(wo_arena_init(&a, 256), 0);
    void *p1 = wo_arena_alloc(&a, 128);
    void *p2 = wo_arena_alloc(&a, 128);
    T_CHECK(p1 != NULL && p2 != NULL);
    /* region exhausted and nothing on the free lists: null, never abort */
    T_CHECK(wo_arena_alloc(&a, 128) == NULL);
    /* freeing one block makes its class allocatable again */
    wo_arena_free(&a, p1, 128);
    T_CHECK(wo_arena_alloc(&a, 128) == p1);
    wo_arena_destroy(&a);
}

static void test_large_bypasses_region(void) {
    wo_arena a;
    T_EQ(wo_arena_init(&a, 64), 0); /* tiny region on purpose */
    /* >1024 goes to plain malloc/free regardless of region capacity */
    void *big = wo_arena_alloc(&a, 4096);
    T_CHECK(big != NULL);
    wo_arena_free(&a, big, 4096);
    wo_arena_destroy(&a);
}

/* language 44: a freed block's header is poisoned (class WO_CLS_FREED, shard
 * 0xFFFF) so it can never pass for a live object; the freelist link lives at
 * offset 8 and must still chain in LIFO order. */
static void test_poison_on_free(void) {
    wo_arena a;
    T_EQ(wo_arena_init(&a, 4096), 0);
    wo_hdr *p = wo_arena_alloc(&a, 48);
    wo_hdr *q = wo_arena_alloc(&a, 48);
    T_CHECK(p != NULL && q != NULL && p != q);
    p->class_id = 7; p->shard_id = 3; p->flags = 0; p->pad = 0;   /* "live" */
    wo_arena_free(&a, p, 48);
    T_CHECK(p->class_id == WO_CLS_FREED);        /* poisoned on free */
    T_CHECK(p->shard_id == 0xFFFFu);
    wo_arena_free(&a, q, 48);                    /* q now heads the list, links to p */
    T_CHECK(q->class_id == WO_CLS_FREED);
    /* LIFO through the relocated (offset-8) link: q first, then p */
    T_CHECK(wo_arena_alloc(&a, 48) == q);
    T_CHECK(wo_arena_alloc(&a, 48) == p);
    /* the class is drained: the next allocation is a fresh bump, not a stale block */
    void *r = wo_arena_alloc(&a, 48);
    T_CHECK(r != NULL && r != p && r != q);
    wo_arena_destroy(&a);
}

int main(void) {
    test_size_class_reuse();
    test_region_oom_returns_null();
    test_large_bypasses_region();
    test_poison_on_free();
    return t_report("test_arena");
}

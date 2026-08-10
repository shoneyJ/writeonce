/* test_cont — native containers: multi growth/bounds, map int + text keys.
 * Element drops land in the gc task; here backing stores are freed by hand. */
#include <stdlib.h>

#include "cont.h"
#include "t.h"

static void free_multi_raw(wo_rt *rt, wo_multi *m) {
    free(m->items);
    wo_arena_free(&rt->arena, m, sizeof(wo_multi));
}

static void free_map_raw(wo_rt *rt, wo_map *m) {
    free(m->keys);
    free(m->vals);
    wo_arena_free(&rt->arena, m, sizeof(wo_map));
}

static void test_multi_push_get_growth(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 16, NULL, 0), 0);
    wo_multi *m = wo_multi_new(&rt, WO_K_SCALAR);
    T_CHECK(m != NULL);
    T_EQ(m->h.class_id, WO_CLS_MULTI);
    for (uint64_t i = 0; i < 100; i++) T_EQ(wo_multi_push(m, i * 7), 0);
    T_EQ(m->len, 100);
    uint64_t v = 0;
    T_EQ(wo_multi_get(m, 0, &v), 0);
    T_EQ(v, 0);
    T_EQ(wo_multi_get(m, 99, &v), 0);
    T_EQ(v, 99 * 7);
    /* bounds miss */
    T_EQ(wo_multi_get(m, 100, &v), -1);
    free_multi_raw(&rt, m);
    wo_rt_destroy(&rt);
}

static void test_map_int_keys(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 16, NULL, 0), 0);
    wo_map *m = wo_map_new(&rt, WO_K_SCALAR, WO_K_SCALAR);
    T_CHECK(m != NULL);
    T_EQ(m->h.class_id, WO_CLS_MAP);
    uint64_t old = 0;
    T_EQ(wo_map_set(m, 1, 10, &old), 0); /* insert */
    T_EQ(wo_map_set(m, 2, 20, &old), 0);
    T_EQ(wo_map_set(m, 1, 11, &old), 1); /* replace hands old back */
    T_EQ(old, 10);
    uint64_t v = 0;
    T_EQ(wo_map_get(m, 1, &v), 0);
    T_EQ(v, 11);
    T_EQ(wo_map_get(m, 2, &v), 0);
    T_EQ(v, 20);
    T_EQ(wo_map_get(m, 3, &v), -1); /* miss */
    T_CHECK(wo_map_has(m, 2));
    T_CHECK(!wo_map_has(m, 3));
    free_map_raw(&rt, m);
    wo_rt_destroy(&rt);
}

static void test_map_text_keys_by_content(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 16, NULL, 0), 0);
    wo_map *m = wo_map_new(&rt, WO_K_TEXT, WO_K_SCALAR);
    wo_str *k1 = wo_str_new(&rt, "sku-1", 5);
    uint64_t old = 0;
    T_EQ(wo_map_set(m, (uint64_t)(uintptr_t)k1, 999, &old), 0);
    /* lookup through a DIFFERENT pointer with equal content must hit */
    wo_str *k1b = wo_str_new(&rt, "sku-1", 5);
    T_CHECK(k1 != k1b);
    uint64_t v = 0;
    T_EQ(wo_map_get(m, (uint64_t)(uintptr_t)k1b, &v), 0);
    T_EQ(v, 999);
    T_CHECK(wo_map_has(m, (uint64_t)(uintptr_t)k1b));
    wo_str *k2 = wo_str_new(&rt, "sku-2", 5);
    T_CHECK(!wo_map_has(m, (uint64_t)(uintptr_t)k2));
    wo_str_free(&rt, k1);
    wo_str_free(&rt, k1b);
    wo_str_free(&rt, k2);
    free_map_raw(&rt, m);
    wo_rt_destroy(&rt);
}

int main(void) {
    test_multi_push_get_growth();
    test_map_int_keys();
    test_map_text_keys_by_content();
    return t_report("test_cont");
}

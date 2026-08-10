/* test_obj — object creation (owned vs @gc), strings, const interning. */
#include "obj.h"
#include "t.h"

/* two test classes: 0 = owned Point{x,y}, 1 = @gc Cache{hits} */
static const uint8_t point_kinds[] = {WO_K_SCALAR, WO_K_SCALAR};
static const uint8_t cache_kinds[] = {WO_K_SCALAR};
static const wo_classdesc CLASSES[] = {
    {.name = 0, .flags = 0, .field_cnt = 2, .kinds = point_kinds},
    {.name = 0, .flags = WO_CLASSF_GC, .field_cnt = 1, .kinds = cache_kinds},
};

static void test_owned_object_zeroed(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 16, CLASSES, 2), 0);
    wo_hdr *o = wo_obj_new(&rt, 0);
    T_CHECK(o != NULL);
    T_EQ(o->class_id, 0);
    T_EQ(o->flags, 0);
    T_EQ(o->borrow, WO_BORROW_FREE);
    T_EQ(o->rc, 0);
    T_EQ(wo_fields(o)[0], 0);
    T_EQ(wo_fields(o)[1], 0);
    wo_arena_free(&rt.arena, o, wo_obj_size(&CLASSES[0]));
    wo_rt_destroy(&rt);
}

static void test_gc_object_rc1(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 16, CLASSES, 2), 0);
    wo_hdr *o = wo_obj_new(&rt, 1);
    T_CHECK(o != NULL);
    T_CHECK(o->flags & WO_F_GC);
    T_EQ(o->rc, 1);
    wo_arena_free(&rt.arena, o, wo_obj_size(&CLASSES[1]));
    wo_rt_destroy(&rt);
}

static void test_strings(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 16, CLASSES, 2), 0);
    wo_str *a = wo_str_new(&rt, "hello ", 6);
    wo_str *b = wo_str_new(&rt, "world", 5);
    T_CHECK(a && b);
    T_EQ(a->h.class_id, WO_CLS_STR);
    T_EQ(a->len, 6);
    T_CHECK(memcmp(a->data, "hello ", 6) == 0);

    wo_str *c = wo_str_concat(&rt, a, b);
    T_CHECK(c != NULL);
    T_EQ(c->len, 11);
    T_CHECK(memcmp(c->data, "hello world", 11) == 0);

    wo_str *c2 = wo_str_new(&rt, "hello world", 11);
    T_CHECK(wo_str_eq(c, c2));      /* content equality, different pointers */
    T_CHECK(!wo_str_eq(a, b));

    wo_str_free(&rt, a);
    wo_str_free(&rt, b);
    wo_str_free(&rt, c);
    wo_str_free(&rt, c2);
    wo_rt_destroy(&rt);
}

static void test_const_string_survives_free(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 16, CLASSES, 2), 0);
    wo_str *s = wo_str_new(&rt, "interned", 8);
    T_CHECK(s != NULL);
    s->h.flags |= WO_F_CONST; /* as the loader will mark interned constants */
    wo_str_free(&rt, s);      /* must be a no-op */
    T_EQ(s->len, 8);          /* still readable: free didn't touch it */
    T_CHECK(memcmp(s->data, "interned", 8) == 0);
    s->h.flags &= (uint8_t)~WO_F_CONST;
    wo_str_free(&rt, s); /* real free so ASan sees no leak */
    wo_rt_destroy(&rt);
}

int main(void) {
    test_owned_object_zeroed();
    test_gc_object_rc1();
    test_strings();
    test_const_string_survives_free();
    return t_report("test_obj");
}

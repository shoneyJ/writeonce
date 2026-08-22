/* test_table — iteration 9 Task 1: class-shaped row storage.
 * Round-trips across kinds, nil encodings, id interleave across shards,
 * slab growth past one slab, slot reuse after removal, and the out-gate
 * invariant (a read hands back FRESH VM values, never slab pointers). */
#include <stdlib.h>
#include <string.h>

#include "cont.h"
#include "gc.h"
#include "obj.h"
#include "t.h"
#include "table.h"

/* class 0: Addr { city: Text }
 * class 1: Emp  { name: Text, salary: Int(scalar), addr: OWNED Addr,
 *                 tags: multi Text, meta: map<Text, scalar> }
 * class 2: Tiny { n: scalar }  (slab-growth workhorse) */
static const uint8_t addr_kinds[] = {WO_K_TEXT};
static const uint8_t emp_kinds[] = {WO_K_TEXT, WO_K_SCALAR, WO_K_OWNED, WO_K_MULTI,
                                    WO_K_MAP};
static const uint8_t tiny_kinds[] = {WO_K_SCALAR};
static const wo_classdesc CLASSES[] = {
    {.name = 0, .flags = 0, .field_cnt = 1, .kinds = addr_kinds},
    {.name = 0, .flags = 0, .field_cnt = 5, .kinds = emp_kinds},
    {.name = 0, .flags = 0, .field_cnt = 1, .kinds = tiny_kinds},
};

static void test_roundtrip_all_kinds(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, CLASSES, 3), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, CLASSES, 3, 0, 1), 0);
    const char *msg = "";

    /* build the VM-side value: Emp{"Asha", 9200000, Addr{"Pune"}, ["a","b"], {"k": 7}} */
    wo_str *name = wo_str_new(&rt, "Asha", 4);
    wo_hdr *addr = wo_obj_new(&rt, 0);
    wo_fields(addr)[0] = (uint64_t)(uintptr_t)wo_str_new(&rt, "Pune", 4);
    wo_multi *tags = wo_multi_new(&rt, WO_K_TEXT);
    wo_multi_push(tags, (uint64_t)(uintptr_t)wo_str_new(&rt, "a", 1));
    wo_multi_push(tags, (uint64_t)(uintptr_t)wo_str_new(&rt, "b", 1));
    wo_map *meta = wo_map_new(&rt, WO_K_TEXT, WO_K_SCALAR);
    uint64_t old;
    wo_map_set(meta, (uint64_t)(uintptr_t)wo_str_new(&rt, "k", 1), 7, &old);

    uint64_t vals[5] = {(uint64_t)(uintptr_t)name, 9200000,
                        (uint64_t)(uintptr_t)addr, (uint64_t)(uintptr_t)tags,
                        (uint64_t)(uintptr_t)meta};
    uint64_t id = wo_row_insert(&db, 1, vals, &msg, NULL);
    T_EQ(id, 1); /* shard 0 of 1: first id is 1 */

    /* the row stored COPIES: mutate the VM originals, then read back */
    name->data[0] = 'X';
    ((wo_str *)(uintptr_t)wo_fields(addr)[0])->data[0] = 'X';

    uint64_t out[5] = {0};
    T_EQ(wo_row_read(&db, &rt, 1, id, out, &msg), 0);
    wo_str *rname = (wo_str *)(uintptr_t)out[0];
    T_EQ(rname->len, 4);
    T_CHECK(memcmp(rname->data, "Asha", 4) == 0); /* not "Xsha" */
    T_CHECK(rname != name);                       /* fresh allocation */
    T_EQ(out[1], 9200000);
    wo_hdr *raddr = (wo_hdr *)(uintptr_t)out[2];
    T_CHECK(raddr != addr);
    wo_str *rcity = (wo_str *)(uintptr_t)wo_fields(raddr)[0];
    T_CHECK(memcmp(rcity->data, "Pune", 4) == 0); /* not "Xune" */
    wo_multi *rtags = (wo_multi *)(uintptr_t)out[3];
    T_EQ(rtags->len, 2);
    T_CHECK(memcmp(((wo_str *)(uintptr_t)rtags->items[1])->data, "b", 1) == 0);
    wo_map *rmeta = (wo_map *)(uintptr_t)out[4];
    uint64_t got = 0;
    wo_str *k = wo_str_new(&rt, "k", 1);
    T_EQ(wo_map_get(rmeta, (uint64_t)(uintptr_t)k, &got), 0);
    T_EQ(got, 7);

    /* nil TEXT / nil OWNED / WO_NIL_SCALAR round-trip */
    uint64_t nilvals[5] = {0, WO_NIL_SCALAR, 0, 0, 0};
    uint64_t id2 = wo_row_insert(&db, 1, nilvals, &msg, NULL);
    T_EQ(id2, 2);
    uint64_t out2[5] = {(uint64_t)-1, 0, (uint64_t)-1, (uint64_t)-1, (uint64_t)-1};
    T_EQ(wo_row_read(&db, &rt, 1, id2, out2, &msg), 0);
    T_EQ(out2[0], 0);
    T_EQ(out2[1], WO_NIL_SCALAR);
    T_EQ(out2[2], 0);
    T_EQ(out2[3], 0);

    /* the VM-side values are containers with malloc'd backing arrays:
       real drops, not arena teardown, are what frees them */
    wo_drop_obj(&rt, (wo_hdr *)name);
    wo_drop_obj(&rt, addr);
    wo_drop_obj(&rt, (wo_hdr *)tags);
    wo_drop_obj(&rt, (wo_hdr *)meta);
    wo_drop_obj(&rt, (wo_hdr *)k);
    for (int i = 0; i < 5; i++)
        if (i != 1 && out[i]) wo_drop_obj(&rt, (wo_hdr *)(uintptr_t)out[i]);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

static void test_id_interleave_across_shards(void) {
    const char *msg = "";
    wo_db a, b, c;
    T_EQ(wo_db_init(&a, CLASSES, 3, 0, 3), 0);
    T_EQ(wo_db_init(&b, CLASSES, 3, 1, 3), 0);
    T_EQ(wo_db_init(&c, CLASSES, 3, 2, 3), 0);
    uint64_t v[1] = {42};
    T_EQ(wo_row_insert(&a, 2, v, &msg, NULL), 1); /* shard 0: 1, 4, 7 */
    T_EQ(wo_row_insert(&a, 2, v, &msg, NULL), 4);
    T_EQ(wo_row_insert(&b, 2, v, &msg, NULL), 2); /* shard 1: 2, 5 */
    T_EQ(wo_row_insert(&b, 2, v, &msg, NULL), 5);
    T_EQ(wo_row_insert(&c, 2, v, &msg, NULL), 3); /* shard 2: 3, 6 */
    T_EQ(wo_row_insert(&c, 2, v, &msg, NULL), 6);
    /* owner-shard discipline: (id-1) % N names the shard */
    T_EQ((4 - 1) % 3, 0);
    T_EQ((5 - 1) % 3, 1);
    T_EQ((6 - 1) % 3, 2);
    /* shard/nshards misuse refused */
    wo_db bad;
    T_EQ(wo_db_init(&bad, CLASSES, 3, 3, 3), -1);
    T_EQ(wo_db_init(&bad, CLASSES, 3, 0, 0), -1);
    wo_db_destroy(&a);
    wo_db_destroy(&b);
    wo_db_destroy(&c);
}

static void test_slab_growth_and_reuse(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, CLASSES, 3), 0);
    const char *msg = "";
    wo_db db;
    T_EQ(wo_db_init(&db, CLASSES, 3, 0, 1), 0);
    /* three slabs' worth of Tiny rows */
    enum { N = 3 * DB_SLAB_ROWS + 5 };
    uint64_t ids[N];
    for (uint32_t i = 0; i < N; i++) {
        uint64_t v[1] = {i};
        ids[i] = wo_row_insert(&db, 2, v, &msg, NULL);
        T_CHECK(ids[i] == i + 1);
    }
    T_EQ(db.tables[2].slab_cnt, 4);
    T_EQ(db.tables[2].count, N);
    /* every row readable after growth (addresses were never moved) */
    uint64_t out[1];
    T_EQ(wo_row_read(&db, &rt, 2, ids[0], out, &msg), 0);
    T_EQ(out[0], 0);
    T_EQ(wo_row_read(&db, &rt, 2, ids[N - 1], out, &msg), 0);
    T_EQ(out[0], N - 1);

    /* remove a middle row: its slot is reused BEFORE any new slab grows */
    db_row *victim = wo_row_ptr(&db, 2, ids[100]);
    T_CHECK(victim != NULL);
    T_EQ(wo_row_remove(&db, 2, ids[100]), 0);
    T_EQ(wo_row_read(&db, &rt, 2, ids[100], out, &msg), -1); /* gone */
    T_EQ(wo_row_remove(&db, 2, ids[100]), -1);               /* twice = miss */
    uint64_t v[1] = {777};
    uint64_t fresh = wo_row_insert(&db, 2, v, &msg, NULL);
    T_CHECK(fresh > (uint64_t)N); /* ids never reused ... */
    db_row *fresh_row = wo_row_ptr(&db, 2, fresh);
    T_CHECK(fresh_row == victim); /* ... but the SLOT is */
    T_EQ(db.tables[2].slab_cnt, 4);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

/* Task 5: single-field update through the choke point — value swapped,
 * indexes moved, unique violations leave the row untouched. Class 3 in a
 * local table: User { email: Text @unique-ish } via idx metadata. */
static const uint8_t user_kinds[] = {WO_K_TEXT, WO_K_SCALAR};
static const uint32_t user_idx_meta[] = {1 /*unique*/, 1, 0 /*col: email*/,
                                         0 /*non-unique*/, 1, 1 /*col: n*/};
static const wo_classdesc UCLASSES[] = {
    {.name = 0, .flags = 0, .field_cnt = 2, .kinds = user_kinds, .idx_cnt = 2,
     .idx_meta = user_idx_meta},
};

static void test_update_field(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, UCLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, UCLASSES, 1, 0, 1), 0);
    const char *msg = "";
    int ek = 0;
    wo_str *e1 = wo_str_new(&rt, "a@x", 3);
    wo_str *e2 = wo_str_new(&rt, "b@x", 3);
    uint64_t v1[2] = {(uint64_t)(uintptr_t)e1, 10};
    uint64_t v2[2] = {(uint64_t)(uintptr_t)e2, 20};
    uint64_t a = wo_row_insert(&db, 0, v1, &msg, NULL);
    uint64_t b = wo_row_insert(&db, 0, v2, &msg, NULL);
    T_CHECK(a && b);

    /* scalar update: value moves, non-unique index follows */
    T_EQ(wo_row_update_field(&db, 0, a, 1, 99, &msg, &ek), 0);
    uint64_t out[2];
    T_EQ(wo_row_read(&db, &rt, 0, a, out, &msg), 0);
    T_EQ(out[1], 99);
    wo_str_free(&rt, (wo_str *)(uintptr_t)out[0]);

    /* unique violation: updating a's email to b's must refuse, row untouched */
    wo_str *dupe = wo_str_new(&rt, "b@x", 3);
    T_EQ(wo_row_update_field(&db, 0, a, 0, (uint64_t)(uintptr_t)dupe, &msg, &ek), -1);
    T_EQ(ek, DB_ERR_UNIQUE);
    T_EQ(wo_row_read(&db, &rt, 0, a, out, &msg), 0);
    wo_str *still = (wo_str *)(uintptr_t)out[0];
    T_CHECK(still->len == 3 && memcmp(still->data, "a@x", 3) == 0);
    wo_str_free(&rt, still);

    /* legal text update: old engine value freed (ASan), index moved — the
       old email becomes free for someone else */
    wo_str *fresh = wo_str_new(&rt, "c@x", 3);
    T_EQ(wo_row_update_field(&db, 0, a, 0, (uint64_t)(uintptr_t)fresh, &msg, &ek), 0);
    wo_str *take_a = wo_str_new(&rt, "a@x", 3);
    uint64_t v3[2] = {(uint64_t)(uintptr_t)take_a, 30};
    uint64_t cid = wo_row_insert(&db, 0, v3, &msg, &ek);
    T_CHECK(cid != 0); /* "a@x" released by the update */

    wo_drop_obj(&rt, (wo_hdr *)e1);
    wo_drop_obj(&rt, (wo_hdr *)e2);
    wo_drop_obj(&rt, (wo_hdr *)dupe);
    wo_drop_obj(&rt, (wo_hdr *)fresh);
    wo_drop_obj(&rt, (wo_hdr *)take_a);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

/* read-path index slice: wo_idx_probe answers a single-column equality
 * from the index buckets (expected O(1)) with the SAME id set the slab
 * walk yields — duplicates, nil text, and removed rows included; a
 * multi-column index refuses (0) so callers keep the scan fallback.
 * Class: Kv { k: Text, n: scalar } with a non-unique index on each,
 * plus one multi-column index over both. */
static const uint8_t kv_kinds[] = {WO_K_TEXT, WO_K_SCALAR};
static const uint32_t kv_idx_meta[] = {0, 1, 0,   /* [k]    */
                                       0, 1, 1,   /* [n]    */
                                       0, 2, 0, 1 /* [k, n] */};
static const wo_classdesc KVCLASSES[] = {
    {.name = 0, .flags = 0, .field_cnt = 2, .kinds = kv_kinds, .idx_cnt = 3,
     .idx_meta = kv_idx_meta},
};

static int ids_contain(const uint64_t *ids, uint32_t n, uint64_t id) {
    for (uint32_t i = 0; i < n; i++)
        if (ids[i] == id) return 1;
    return 0;
}

static void test_idx_probe(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 20, KVCLASSES, 1), 0);
    wo_db db;
    T_EQ(wo_db_init(&db, KVCLASSES, 1, 0, 1), 0);
    const char *msg = "";
    /* rows: ("a",1) ("a",2) (nil,1) ("b",2) ("a",3); then remove the
     * second "a" so the bucket's removal path is exercised */
    wo_str *sa = wo_str_new(&rt, "a", 1);
    wo_str *sb = wo_str_new(&rt, "b", 1);
    uint64_t r1[2] = {(uint64_t)(uintptr_t)sa, 1};
    uint64_t r2[2] = {(uint64_t)(uintptr_t)sa, 2};
    uint64_t r3[2] = {0, 1};
    uint64_t r4[2] = {(uint64_t)(uintptr_t)sb, 2};
    uint64_t r5[2] = {(uint64_t)(uintptr_t)sa, 3};
    uint64_t a1 = wo_row_insert(&db, 0, r1, &msg, NULL);
    uint64_t a2 = wo_row_insert(&db, 0, r2, &msg, NULL);
    uint64_t a3 = wo_row_insert(&db, 0, r3, &msg, NULL);
    uint64_t a4 = wo_row_insert(&db, 0, r4, &msg, NULL);
    uint64_t a5 = wo_row_insert(&db, 0, r5, &msg, NULL);
    T_CHECK(a1 && a2 && a3 && a4 && a5);
    T_EQ(wo_row_remove(&db, 0, a2), 0);

    uint64_t *ids = NULL;
    uint32_t n = 0;
    /* text key "a" on index 0 ([k]): exactly a1 and a5 */
    T_EQ(wo_idx_probe(&db, 0, 0, 0, "a", 1, &ids, &n), 1);
    T_EQ(n, 2);
    T_CHECK(ids_contain(ids, n, a1) && ids_contain(ids, n, a5));
    free(ids);
    /* nil text key (bytes == NULL): exactly a3 */
    T_EQ(wo_idx_probe(&db, 0, 0, 0, NULL, 0, &ids, &n), 1);
    T_EQ(n, 1);
    T_CHECK(ids_contain(ids, n, a3));
    free(ids);
    /* scalar key 2 on index 1 ([n]): a4 only (a2 removed) */
    T_EQ(wo_idx_probe(&db, 0, 1, 2, NULL, 0, &ids, &n), 1);
    T_EQ(n, 1);
    T_CHECK(ids_contain(ids, n, a4));
    free(ids);
    /* absent key: probed, empty */
    T_EQ(wo_idx_probe(&db, 0, 1, 77, NULL, 0, &ids, &n), 1);
    T_EQ(n, 0);
    free(ids);
    /* multi-column index 2 ([k, n]): refuses — caller falls back */
    T_EQ(wo_idx_probe(&db, 0, 2, 2, NULL, 0, &ids, &n), 0);
    /* out-of-range index: refuses, never traps */
    T_EQ(wo_idx_probe(&db, 0, 9, 2, NULL, 0, &ids, &n), 0);
    wo_db_destroy(&db);
    wo_rt_destroy(&rt);
}

static void test_misuse(void) {
    const char *msg = "";
    wo_db db;
    T_EQ(wo_db_init(&db, CLASSES, 3, 0, 1), 0);
    uint64_t v[1] = {1};
    T_EQ(wo_row_insert(&db, 99, v, &msg, NULL), 0); /* unknown class */
    T_CHECK(wo_row_ptr(&db, 99, 1) == NULL);
    T_CHECK(wo_row_ptr(&db, 2, 1) == NULL); /* table never touched */
    T_EQ(wo_row_remove(&db, 2, 1), -1);
    wo_db_destroy(&db);
}

int main(void) {
    test_roundtrip_all_kinds();
    test_id_interleave_across_shards();
    test_slab_growth_and_reuse();
    test_update_field();
    test_idx_probe();
    test_misuse();
    return t_report("test_table");
}

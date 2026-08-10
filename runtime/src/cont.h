/* cont.h — native containers (spec §3): `multi T` and `map<K,V>` are
 * runtime-provided native classes, not user generics. Struct heads live in
 * the arena; backing arrays are malloc/realloc'd. Map lookup is a linear
 * scan — deliberate milestone-1 KISS. Insert-or-replace hands the replaced
 * old value back to the caller (dropping needs gc.c; the builtin layer does
 * it). Element-dropping frees also live in gc.c. */
#ifndef WO_CONT_H
#define WO_CONT_H

#include "obj.h"

typedef struct wo_multi {
    wo_hdr h; /* class_id WO_CLS_MULTI */
    uint8_t elem_kind;
    uint32_t len, cap;
    uint64_t *items;
} wo_multi;

wo_multi *wo_multi_new(wo_rt *rt, uint8_t elem_kind); /* NULL = OOM */
int wo_multi_push(wo_multi *m, uint64_t v);           /* 0 ok, -1 OOM */
int wo_multi_get(const wo_multi *m, uint64_t idx, uint64_t *out); /* -1 oob */

typedef struct wo_map {
    wo_hdr h; /* class_id WO_CLS_MAP */
    uint8_t key_kind, val_kind;
    uint32_t len, cap;
    uint64_t *keys, *vals; /* parallel arrays */
} wo_map;

wo_map *wo_map_new(wo_rt *rt, uint8_t key_kind, uint8_t val_kind);
/* 0 = inserted, 1 = replaced (*old set to the displaced value), -1 = OOM.
 * Keys compare by content when key_kind is TEXT, by bits otherwise. */
int wo_map_set(wo_map *m, uint64_t k, uint64_t v, uint64_t *old);
int wo_map_get(const wo_map *m, uint64_t k, uint64_t *out); /* -1 = miss */
int wo_map_has(const wo_map *m, uint64_t k);

#endif /* WO_CONT_H */

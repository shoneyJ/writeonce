/* loader.h — .wob loading + the full static-validation contract.
 * The "no UB on any input" constraint lives here: everything is parsed
 * through a bounds-checked cursor and COPIED OUT into aligned, malloc'd
 * structures (misaligned files and input-buffer lifetimes are non-issues);
 * text constants intern as const-flagged strings. What the loader accepts,
 * the interpreter trusts forever — the validation list is in loader.c.
 * Field indexes on GETF/SETF stay runtime checks (registers are untyped):
 * the spec's residual-check doctrine. */
#ifndef WO_LOADER_H
#define WO_LOADER_H

#include "obj.h"

typedef struct wo_lineent {
    uint32_t pc, line;
} wo_lineent;

typedef struct wo_dropent {
    uint32_t pc;
    uint64_t owned; /* register bitmask: live owned values at pc */
    uint64_t gc;    /* register bitmask: live @gc references at pc */
} wo_dropent;

typedef struct wo_methodrec {
    uint32_t name;     /* constant index of a text constant */
    uint32_t class_id; /* WOB_NONE = free fn */
    uint8_t arg_cnt, reg_cnt;
    uint32_t *code; /* aligned owned copy */
    uint32_t ninstr;
    wo_lineent *lines;
    uint32_t line_cnt;
    wo_dropent *drops;
    uint32_t drop_cnt;
} wo_methodrec;

typedef struct wo_const {
    uint8_t tag; /* WOB_K_INT / WOB_K_TEXT */
    int64_t i;   /* tag INT */
    wo_str *s;   /* tag TEXT: interned, WO_F_CONST, module-owned */
} wo_const;

/* vtable rows expanded flat and sorted by (class_id, slot) for binary
 * search at ICALL time */
typedef struct wo_vtabent {
    uint32_t class_id, slot, method;
} wo_vtabent;

typedef struct wo_module {
    wo_const *consts;
    uint32_t const_cnt;
    wo_classdesc *classes;
    uint32_t class_cnt;
    uint8_t *kindpool; /* pooled field-kind bytes the classes point into */
    /* pooled per-field metadata (v2): names, referenced class ids, element
       kinds — see wob.h's "class-table field metadata" note */
    uint32_t *metapool;
    uint32_t slot_cnt; /* total interface slots across all interfaces */
    wo_vtabent *vtabs;
    uint32_t vtab_cnt;
    wo_methodrec *methods;
    uint32_t method_cnt;
    uint32_t entry; /* WOB_NONE = none */
} wo_module;

/* 0 ok; -1 reject with a human-readable reason in err (never empty) and
 * everything parsed so far freed. */
int wo_load_buf(wo_module *m, const uint8_t *buf, size_t len, char *err,
                size_t errlen);
/* mmap -> parse -> munmap */
int wo_load_file(wo_module *m, const char *path, char *err, size_t errlen);
void wo_module_free(wo_module *m);

#endif /* WO_LOADER_H */

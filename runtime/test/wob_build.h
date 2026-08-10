/* wob_build.h — in-memory .wob assembler for tests and fixture generation.
 * Deliberately a SECOND, independent encoding of the format (the loader is
 * the first): builder/loader disagreements surface as test failures,
 * cross-checking docs/plan/oop-vm/00-wob-format.md. Test-side only — never
 * linked into wovm itself. */
#ifndef WO_WOB_BUILD_H
#define WO_WOB_BUILD_H

#include <stddef.h>

#include "wob.h"

typedef struct wb_t wb_t;

typedef struct wb_drop {
    uint32_t pc;
    uint64_t owned; /* registers holding live owned values at pc */
    uint64_t gc;    /* registers holding live @gc references at pc */
} wb_drop;

wb_t *wb_new(void);

/* constants: returns the constant index */
uint32_t wb_const_int(wb_t *b, int64_t v);
uint32_t wb_const_text(wb_t *b, const char *s); /* strlen'd, no NUL stored */

/* classes: returns the class id */
uint32_t wb_class(wb_t *b, uint32_t name_const, uint32_t flags,
                  const uint8_t *kinds, uint32_t field_cnt);

/* interfaces: returns the interface id; global slot ids accumulate in
 * declaration order (first interface's methods get slots 0..n-1, etc.) */
uint32_t wb_iface(wb_t *b, uint32_t name_const, uint32_t method_cnt);
/* vtable row: this class implements this interface with these method
 * indexes (one per interface method, in interface order) */
void wb_vtab(wb_t *b, uint32_t class_id, uint32_t iface_id,
             const uint32_t *methods, uint32_t method_cnt);

/* methods: returns the method index. lines = flattened ascending pc,line
 * pairs (2*nlines u32s). drops = ascending drop-table entries. */
uint32_t wb_method(wb_t *b, uint32_t name_const, uint32_t class_id,
                   uint8_t argc, uint8_t regc, const uint32_t *code,
                   uint32_t ninstr, const uint32_t *lines, uint32_t nlines,
                   const wb_drop *drops, uint32_t ndrops);

void wb_entry(wb_t *b, uint32_t method_idx);

/* concatenate sections, compute header offsets, free the builder;
 * returns one malloc'd image (caller frees) */
uint8_t *wb_finish(wb_t *b, size_t *len);

#endif /* WO_WOB_BUILD_H */

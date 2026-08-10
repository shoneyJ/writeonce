/* gc.h — deterministic destruction + @gc reference counting (spec §4).
 * Owned objects die deterministically via drop plans; @gc objects die at
 * refcount zero. One kind-directed dispatcher is the workhorse: scalars
 * ignored, owned values drop recursively, gc refs decrement, texts free,
 * containers free element-wise then their backing. The budgeted cycle
 * collector extends this module (Bacon–Rajan trial deletion). */
#ifndef WO_GC_H
#define WO_GC_H

#include "obj.h"

/* Drop an 8-byte field/register value known to be of field kind `kind`.
 * Null (0) values are ignored for every kind. */
void wo_drop_kind(wo_rt *rt, uint8_t kind, uint64_t v);

/* Drop any heap value by its header: native sentinels route to their own
 * frees; class objects walk their kind array over the field slots, then
 * free themselves. */
void wo_drop_obj(wo_rt *rt, wo_hdr *o);

void wo_rc_inc(wo_hdr *o);
/* Decrement; at zero, release contents and free — unless the object sits
 * in the cycle-candidate buffer (WO_F_BUF): the collector owns its death. */
void wo_rc_dec(wo_rt *rt, wo_hdr *o);

/* Budgeted cycle collection step (Bacon–Rajan trial deletion over the
 * candidate buffer). Processes up to `budget` buffered roots (whole
 * strongly-connected components process atomically, so overshoot is
 * bounded); returns how many objects it freed. */
size_t wo_gc_step(wo_rt *rt, size_t budget);

#endif /* WO_GC_H */

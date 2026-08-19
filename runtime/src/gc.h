/* gc.h — deterministic destruction + incremental tri-color mark-sweep for
 * traced (inferred-gc) objects (iteration 7b, spec 2026-08-11).
 *
 * Two reclamation systems, one module:
 *   - OWNED values die deterministically via drop plans (wo_drop_obj /
 *     wo_drop_kind), exactly as before.
 *   - TRACED objects (instances of classes the compiler inferred `gc`) die
 *     only by the collector: every traced allocation links onto the
 *     per-shard traced list; a cycle marks from the VM's root snapshot and
 *     sweeps the unmarked.
 *
 * The algorithm is snapshot-at-beginning: roots (the value/frame stacks,
 * read through the per-pc gc/owned masks) are scanned atomically when a
 * cycle starts; a Yuasa deletion barrier shades the OLD value of every
 * gcref edge deleted while marking (SETF overwrites and owned-value deaths
 * both funnel through here); objects allocated mid-cycle are born black.
 * Marking and sweeping are budgeted (WO_GC_BUDGET objects per slice) so no
 * slice's pause grows with the heap. */
#ifndef WO_GC_H
#define WO_GC_H

#include "obj.h"

/* Drop an 8-byte field/register value known to be of field kind `kind`.
 * Null (0) values are ignored for every kind. WO_K_GCREF is the deletion
 * barrier: while marking, the old target is shaded; otherwise a no-op —
 * tracing owns traced lifetimes, so an owned value dying never frees them. */
void wo_drop_kind(wo_rt *rt, uint8_t kind, uint64_t v);

/* Drop any OWNED heap value by its header: native sentinels route to their
 * own frees; class objects walk their kind array over the field slots, then
 * free themselves. Never called on a traced object (sweep frees those). */
void wo_drop_obj(wo_rt *rt, wo_hdr *o);

/* Link a freshly allocated traced object onto the traced list and account
 * its bytes toward the cycle trigger (called by wo_obj_new). */
void wo_gc_track(wo_rt *rt, wo_hdr *o, size_t size);

/* Yuasa deletion barrier half: shade a traced object gray if it is still
 * white. Also the root-shading primitive. Safe on any phase; only MARK
 * callers need it. */
void wo_gc_shade(wo_rt *rt, wo_hdr *o);

/* Root visitor: shade a traced object, or walk an owned value's interior
 * (fields, container elements) shading every traced object it can reach —
 * pruned by the per-class may-gcref bit. Used for the root snapshot and by
 * the mark phase when it crosses an owned field. */
void wo_gc_scan_root(wo_rt *rt, wo_hdr *o);

/* 1 = the trigger says a cycle should start (idle + traced bytes past the
 * goal). The caller (the VM's safepoints, the post-exit pump) snapshots
 * roots and calls wo_gc_begin. */
int wo_gc_want_start(const wo_rt *rt);

/* Enter MARK phase. The caller shades the roots (wo_gc_scan_root over the
 * live frames' masks) immediately after — before the mutator resumes. */
void wo_gc_begin(wo_rt *rt);

/* One budgeted slice: pops up to `budget` gray objects and scans them;
 * when the worklist drains, switches to SWEEP and frees up to `budget`
 * unmarked traced objects per slice, repainting survivors white. Returns
 * the number freed by this slice. No-op when idle. */
size_t wo_gc_slice(wo_rt *rt, size_t budget);

#endif /* WO_GC_H */

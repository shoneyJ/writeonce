# Inferred GC + incremental mark-sweep — design spec

**Date:** 2026-08-11
**Status:** approved design, pre-implementation
**Scope:** removing `@gc` as a developer-facing annotation, inferring GC-ness in the
compiler, and replacing reference counting with an incremental per-shard
tri-color mark-sweep collector
**Amends:** [`2026-08-01-oop-compiler-vm-design.md`](2026-08-01-oop-compiler-vm-design.md)
(decision table's GC-granularity row, §3 rule 5, §4 memory model),
[`../../00-principles.md`](../../00-principles.md) (principle 3),
[`../../plan/oop-vm/00-wob-format.md`](../../plan/oop-vm/00-wob-format.md) (opcodes 27–28, drop-table
contract, class flags), [`../../plan/oop-vm/01-error-catalog.md`](../../plan/oop-vm/01-error-catalog.md)
(WO-W201, WO-E304)
**Reference studied:** Go's collector, `.dev/reference/go/src/runtime/mgc.go` and neighbours

## Motivation

`@gc` asks the developer to answer a question the compiler is better placed to
answer: does this type need tracing? Worse, the annotation is not even
sufficient — the OOP spec's own example, `@gc class PriceCache { entries:
map<SKU, Money> }`, is acyclic. It needs GC because it is a shared cache
aliased from many places, and second-class borrows cannot be stored or
returned, so a long-lived shared alias has nowhere else to live. So the
developer is being asked to reason about two different things at once: the
shape of their types *and* how those values will be aliased across the whole
program.

The reference-counting implementation makes this worse rather than better.
RC requires the compiler to emit a balanced acquire/release at **every** alias
site, and every recorded `@gc` defect in this repo is exactly that failure:

| Defect | Cause |
| --- | --- |
| `push(multi, gcVal)` emitted no `RC_INC` | unresolved builtin skipped the transfer path — use-after-free |
| `set(m, k, v)` still emits none (open) | same gap, never closed |
| `mut`-`@gc` argument didn't clobber its root | stale rc elision — use-after-free |
| `gc/held-cycle` leaks 1184 bytes | the entry return value's reference is never released, inflating rc forever |

Inferring GC-ness would *widen* that population and therefore widen that bug
class. Tracing deletes the category outright: the compiler emits no per-alias
bookkeeping at all.

And most of what tracing needs already exists. Go's collector depends on nine
pieces of compiler metadata; the load-bearing ones are precise per-PC pointer
maps and per-frame unwinding. The emitter already produces exactly that — the
drop table carries an owned-register mask *and* a gc-register mask at every
trap-capable pc, and the class table carries per-field kinds. Go's dependence
on OS threads is incidental, not load-bearing: what the algorithm actually
requires is the ability to suspend one stack and look up a PC→pointer map per
frame, which a bytecode VM with an explicit value stack satisfies without
native stacks. Non-moving matches the arena. The per-P coordination machinery
(ragged barrier, write-barrier buffers, work stealing, assist credit) exists
to run N mutators over one heap and is deletable in a per-shard design.

## Decisions locked during brainstorming

| Question | Decision |
| --- | --- |
| Scope | **One spec: infer *and* replace the collector.** Dropping `@gc` without replacing RC would widen the exact bug class that has already bitten four times. |
| What decides GC-ness | **Hybrid: structural + reported promotion.** Cycles from a whole-program SCC over the class-reference graph; long-lived aliasing promotes on demand at the site that would otherwise error. Every promotion is reported. |
| Collector cadence | **Incremental tri-color, budgeted slices, with a Yuasa deletion barrier.** Bounded pause regardless of heap size — which principle 3 already promises and the `budget-steps` fixture already asserts. |
| Barrier scope | Pointer stores into traced objects only, and only while marking is active. Owned objects, scalars and text pay nothing. |
| `@gc` in source | **Errors**, with a diagnostic pointing at inference. Accepting a now-meaningless annotation would be a lie surface. |

Rejected: structural-only inference (leaves `PriceCache`-shaped types still
needing an annotation); demand-only inference (a distant edit silently flips a
type's memory strategy with nothing reporting it); stop-the-shard full trace
(no barrier, but the pause grows with the live set and principle 3's
"budgeted" wording would have to weaken); post-exit-only collection
(contradicts principle 6 — a service that never stops never collects);
sequencing the collector before inference (defensible, and rejected only
because the coupling argument above makes one coherent landing cheaper than
two).

## 1. Inference — `compiler/src/gcinfer.ml`

A new pass between `types` and `owner`.

**Structural half.** Build a class-reference graph: an edge from A to B when A
has a field whose type is B, `multi B`, `map<B, _>`, `map<_, B>`, or any of
those under `?`. **`ref B` creates no edge** — it is an id, not a pointer, and
already classifies as `Copy`. Run Tarjan's SCC. Every class in a non-trivial
SCC, or with a self-loop, is traced. This is decidable from declarations
alone, so it is stable under edits elsewhere in the program.

**Demand half.** Run the ownership analysis in a collect-promotions mode: at
each site where it would report an escape or aliasing error (today's
`WO-E304` and the long-lived-alias cases), record the class rather than the
error. Promote all recorded classes, then re-run ownership. The promoted set
only grows and is bounded by the class count, so this terminates; two owner
passes is the worst case in practice because promotion removes errors and
never creates them.

**Everything is reported.** Each promotion emits a note stating the reason —
the cycle path for a structural promotion, the escape site for a demand
promotion. `--dump-gc` renders the whole classification, which is the
golden-testable artifact:

```
Cache      gc     (alias escape, cache.wo:12)
Node       gc     (cycle Node -> Node)
Product    owned
Price      owned
```

**Field kinds follow.** A field whose type is a traced class derives
`WO_K_GCREF` automatically; `types.ml`'s existing derivation reads the
inferred set instead of `is_gc_class`.

**The annotation is removed.** The parser's `| "gc" -> is_gc := true` arm
becomes a diagnostic in the WO-E1xx range naming the inference pass and
`--dump-gc`. `class_info.is_gc` and `Types.is_gc_class` are replaced by the
inferred set; `dump.ml` stops rendering ` @gc`.

## 2. What the compiler emits

- **`RC_INC` / `RC_DEC` are no longer emitted.** Opcodes 27–28 become
  reserved. This is a `.wob` version bump, recorded in the format doc.
- **No barrier opcode and no emitter barrier.** `SETF` already resolves the
  field kind from the class table, so the barrier lives inside the VM's store
  paths (`SETF`, `map_set`, `push`). Zero new opcodes, zero emitter change for
  the barrier — a deliberate contrast with Go, which must insert barrier calls
  because it compiles to machine code.
- **The drop table's gc mask keeps its bits and changes contract.** It stops
  meaning "`rc_dec` these registers while unwinding" and starts meaning
  "these registers are GC roots at this pc". The owned mask is unchanged, and
  `DROP` placement for owned values is unchanged.
- **`wo_drop_kind` for `WO_K_GCREF` becomes a no-op** — tracing owns the
  lifetime of traced objects, so an owned object dying never frees them.
- The class table's `@gc` bit survives with the same encoding; only its
  *source* changes from annotation to inference.

Everything else the emitter does — register allocation, window calls, moves,
owned drops, residual borrow guards, line tables — is untouched. Inference
changes which classes are traced, not how anything is lowered.

## 3. Runtime — header and the sweep list

**The arena cannot enumerate objects.** It is bump allocation plus 16-byte
size-class free lists, with anything over 1024 bytes falling through to bare
`malloc`; `wo_arena_free` requires the caller to pass the size back, and no
size headers exist anywhere. Sweep therefore needs its own object list. This
is the single largest runtime addition in this spec.

**The header stays exactly 16 bytes.** Retiring `rc` frees four bytes, and
traced objects are exempt from borrow rules so their `borrow` word is dead
too. Those two adjacent words give exactly eight contiguous bytes — one
64-bit intrusive list link. The `_Static_assert(sizeof(wo_hdr) == 16)` and the
format doc's layout size both survive unchanged.

**A per-shard traced list.** `wo_rt` gains a list head; every traced
allocation links itself in. Sweep walks the list, recovers each object's size
from the class table via `wo_obj_size`, frees the unmarked, and unlinks. The
existing cycle-candidate buffer (`cycbuf`) and the `WO_F_BUF` flag are
retired — they exist only to serve trial deletion.

## 4. Runtime — incremental marking and the barrier

**Colors.** The two existing color bits (`WO_F_COLOR`) carry white/grey/black.
No header growth.

**Roots.** The VM's value stack and frame stack, read through the per-pc gc
masks the emitter already emits — one mask lookup per live frame, exactly the
mechanism Go gets from `FUNCDATA_LocalsPointerMaps` but already present here.

**Owned objects are traversed, never freed.** An owned object can hold a
`GCREF` field, so tracing must walk through owned subtrees to find traced
objects. To stop that costing the whole owned graph, the class table gains a
precomputed **"transitively contains a gcref"** bit, so owned subtrees that
cannot reach a traced object are skipped outright. This bit is derivable in
the same pass that computes the SCCs.

**Safepoints** go at loop back-edges and calls — the pcs that already carry
drop-table entries, so no new metadata is needed.

**Barrier.** Yuasa deletion barrier, active only while marking: on a pointer
store into a traced slot, shade the *old* value before overwriting it. This is
the half of Go's hybrid barrier that eliminates stack rescanning; the
Dijkstra insertion half is unnecessary because a shard's own stack is
re-read from its masks at each slice rather than being scanned once and
trusted.

**Budget and trigger.** `WO_GC_BUDGET` keeps its name and meaning — objects
marked per slice. The cycle starts on a heap goal over the shard's
traced-bytes since the last cycle. `WO_GC_TRACE` keeps its stderr trace, with
freed/marked counts per slice.

## 5. Error handling

No new error mechanism. The barrier and the collector cannot fail: allocation
failure already traps `WO_T_OOM`, and a sweep-list allocation failure does not
exist because the link is inside the object. Inference reports notes, never
traps. The one genuinely new failure mode is a **barrier bug**, which
manifests as silent corruption rather than a diagnostic — §7 addresses it with
a dedicated adversarial test rather than a runtime check.

## 6. What this deletes

Recording this plainly, because it is the design's main argument:

- `gc.c`'s trial deletion — `mark_gray`, `scan_black`, `scan_`,
  `collect_white`, `white_free`, the candidate buffer, the zombie guard.
- `owner.ml`'s rc machinery — the rc table, elision groups, `rc_escaped`,
  `gc_escape`, `resolve_rc`, `release_gc`, and the clobber rule that exists
  only to invalidate elision.
- `emit.ml`'s `emit_rc` and the escape-acquire anchor.
- The `RC_INC`/`RC_DEC` interpreter cases.
- **All four recorded `@gc` defects**, by construction: the `set` gap has no
  `RC_INC` to omit; the held-cycle leak was rc inflation from an unreleased
  return value, and with tracing that value simply is not a root; the
  `mut`-`@gc` clobber protected an elision that no longer exists; the `push`
  bug cannot recur.

## 7. Testing

- **The barrier is the load-bearing test.** An adversarial fixture where the
  mutator hides a traced object between marking slices — store it into an
  already-blackened object and drop the original reference — must not free it.
  A barrier bug is silent corruption, so this test is the design's safety net
  and must fail loudly if the barrier is compiled out.
- **Inference:** unit tests for SCC classification (self-loop, mutual
  recursion, `multi Self`, `map<_, Self>`, and the `ref T`-creates-no-edge
  case), promotion cases, and golden `--dump-gc` output including the note
  text.
- **Collector:** `runtime/test/test_cycle.c` and `test_rc.c` are rewritten —
  they currently assert rc values, which cease to exist. New assertions: an
  abandoned cycle is freed, a rooted cycle survives, slices are bounded, and
  the sweep list has no leak after N cycles.
- **Corpus:** `tests/corpus/gc/abandoned-cycle` and `budget-steps` survive
  with re-blessed traces. `held-cycle` is **redefined** — with tracing, a
  post-exit heap has no roots at all, so the honest fixture is "a cycle rooted
  from a live frame survives a slice", tested from inside a running program
  rather than after the entry returns.
- **ASan across the corpus**, as today, plus a leak-free assertion after
  repeated collection cycles.
- The milestone-1 acceptance gate's criterion 4 is restated in terms of
  tracing; criterion 3's ASan clause is expected to go green, since the
  held-cycle leak is one of the defects this deletes.

## 8. Migration — normative claims to amend

| Where | Change |
| --- | --- |
| `docs/00-principles.md` principle 3 | "`@gc` is a per-class opt-in" → GC-ness is inferred; keep "no global pause exists by construction" (still true — per-shard, and other shards never stop) |
| OOP spec decision table, GC-granularity row | per-class annotation → inferred, with the hybrid rule named |
| OOP spec §3 rule 5 | "`@gc` class instances alias freely" → traced classes alias freely, and which classes those are is inferred |
| OOP spec §4 memory model | replace the RC + Bacon–Rajan paragraph with tracing; header `rc` → sweep-list link; drop `IN_CYCLE_BUF` |
| `00-wob-format.md` | opcodes 27–28 reserved; version bump; drop-table gc-mask contract restated as GC roots; class-flag provenance |
| `01-error-catalog.md` | WO-W201 (`@gc` suggestion) retired — inference supersedes it; WO-E304's `@gc`-exemption wording updated; new WO-E1xx for `@gc` in source |
| `08-builtin-surface.md` | the `push` special case and the `set` gap both deleted — neither exists without RC |
| Goldens | every fixture rendering ` @gc`, `flags=gc`, `gc={rN}`, `RC_INC`/`RC_DEC`, or the `== RC ==` table section re-blessed |
| `docs/00-status.md` | records this as the iteration that supersedes part of iteration 2's memory model |

## Success criteria

1. No `.wo` file in the repo contains `@gc`, and using it is a diagnostic.
2. `--dump-gc` classifies every class in the pricing and corpus samples, and
   every traced class's reason is either a cycle path or a named escape site.
3. `RC_INC`/`RC_DEC` appear in no emitted image; the opcodes are reserved in
   the format doc.
4. The adversarial barrier fixture fails when the barrier is compiled out and
   passes when it is in.
5. An abandoned cycle is collected within budgeted slices with no slice
   exceeding the configured budget; a rooted cycle survives.
6. The whole corpus is ASan-clean, including after repeated collection cycles
   — closing milestone-1 criterion 3.

## Out of scope

Cross-shard tracing (ownership moves mean no traced object spans shards);
generational collection (no remembered set, no age bits — Go's isn't
generational either); compaction (non-moving is load-bearing: no forwarding
pointers, no read barrier); scheduler-integrated pacing beyond the heap-goal
trigger, which remains sub-project 2's concern; and `ref T` semantics, which
are unchanged.

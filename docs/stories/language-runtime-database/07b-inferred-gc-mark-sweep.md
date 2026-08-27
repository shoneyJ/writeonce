---
iteration: "7b"
status: done
---

# Iteration 7b — inferred GC + incremental mark-sweep

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).
>
> **Inserted 2026-08-11**, after the plan was first drawn — hence `7b` rather
> than a renumber. It sits here because the log-watcher critical path
> (iterations 3–7) must not be delayed, and because the collector should be
> settled before iteration 8 multiplies shards.


> **Status (2026-08-18): LANDED** (branch `inferred-gc`; plan
> [`2026-08-18-inferred-gc-mark-sweep.md`](../../superpowers/plans/2026-08-18-inferred-gc-mark-sweep.md)).
> The front end infers GC-ness (structural SCC + demand promotion,
> `woc --dump-gc`), `@gc` in source is WO-E104, and the runtime's RC +
> Bacon–Rajan collector is replaced by an incremental per-shard tri-color
> mark-sweep with a Yuasa deletion barrier — `.wob` is v4, opcodes 27–28
> reserved. The worked example is
> [`docs/examples/gc-cycle`](../../examples/gc-cycle/README.md): its ring
> compiles with no annotation, runs, and is reclaimed in budgeted slices,
> ASan-clean. All four recorded `@gc`/RC defects are deleted by
> construction; `just oop-accept` is fully green (criterion 3's ASan clause
> included).
>
> *(Historical status 2026-08-14: not on the log-watcher critical path — 35
> classes, none gc, arena + deterministic drops end to end. That is still
> true; log-watcher simply never allocates a traced object.)*

## Goals

- **The developer stops deciding which types are garbage collected.** `@gc`
  disappears from the language; the compiler infers GC-ness and reports every
  decision with its reason.
- Reference counting is replaced by an incremental per-shard tri-color
  mark-sweep collector, so the compiler no longer has to emit a balanced
  acquire/release at every alias site — the source of every recorded `@gc`
  defect.
- Milestone 1's acceptance criterion 3 (ASan-clean across the corpus) closes,
  because the leak blocking it is one of the defects this deletes.

## Acceptance Criteria

- What to achieve?
    - **Given** a class whose declaration can form a reference cycle, and a
      separate class that is only ever shared through a long-lived alias,
    - **when** the program is compiled,
    - **then** both are classified GC-managed without any annotation, and
      `--dump-gc` names the reason for each — a cycle path for the first, the
      escaping alias site for the second.
- What to achieve?
    - **Given** any `.wo` source containing `@gc`,
    - **when** it is compiled,
    - **then** it is a diagnostic pointing at inference and `--dump-gc`, not a
      silently accepted no-op.
- What to achieve?
    - **Given** a program that hides a traced object from the collector —
      storing it into an already-blackened object between marking slices and
      dropping the original reference,
    - **when** the collector completes,
    - **then** the object is still alive; and the same fixture fails loudly if
      the write barrier is compiled out.
- What to achieve?
    - **Given** an abandoned cycle and a cycle still rooted from a live frame,
    - **when** collection runs,
    - **then** the abandoned one is freed within budgeted slices with no slice
      exceeding the configured budget, and the rooted one survives.
- What to achieve?
    - **Given** the whole conformance corpus, run repeatedly so several
      collection cycles occur,
    - **when** it runs under ASan,
    - **then** zero leaks and zero errors — the clause that currently fails.
- What to achieve?
    - **Given** any emitted `.wob` image,
    - **when** it is disassembled,
    - **then** no `RC_INC` or `RC_DEC` appears, and the format doc records
      opcodes 27–28 as reserved behind a version bump.

## Out Of Scope

- Cross-shard tracing — ownership moves mean no traced object spans shards.
- Generational collection and compaction. Non-moving is load-bearing: no
  forwarding pointers, no read barrier. Go's collector is not generational
  either.
- Scheduler-integrated pacing beyond the heap-goal trigger; that stays
  iteration 8's concern, which is part of why this lands first.
- `ref T` semantics, unchanged — it is an id, not a pointer, and creates no
  edge in the inference graph.

## Info

- Governing spec: [`docs/superpowers/specs/2026-08-11-inferred-gc-mark-sweep-design.md`](../../superpowers/specs/2026-08-11-inferred-gc-mark-sweep-design.md).
- **Gated by the benchmark (2026-08-15):** this is the "implement garbage
  collection" lever of the performance arc — tri-color mark-sweep replacing
  RC changes the write path's tail latency, so landing it means re-running
  iteration [22](22-durability-throughput-scale.md) and recording the
  delta (does tracing help or hurt p99 under write load?).
- **Constraint added by the database track (2026-08-15):** a GC-managed value
  in a `@table` field is a compile error (the engine/heap bulkhead — 9b
  design, section 6). Once GC-ness is inferred rather than annotated, the
  inference pass must classify every class **before** table-field validation,
  and the diagnostic must name the inference reason ("class X is
  garbage-collected via Y and cannot be stored in a table field") — otherwise
  the error becomes unactionable exactly when it stops being self-evident.
- **Why the annotation was insufficient, not merely inconvenient:** the OOP
  spec's own example, `@gc class PriceCache { entries: map<SKU, Money> }`, is
  acyclic. It needs GC because it is shared, and second-class borrows cannot
  be stored or returned. So the developer was being asked to reason about type
  shape *and* whole-program aliasing at once — hence the hybrid rule
  (structural SCC plus reported demand promotion).
- **Why tracing rather than better reference counting:** all four recorded
  `@gc` defects are RC bookkeeping failures — `push` missing an increment,
  `set` still missing one, the `mut`-`@gc` clobber, and the held-cycle leak.
  Inferring GC-ness would widen that population and so widen that bug class.
  Tracing emits no per-alias bookkeeping at all.
- **Most of what tracing needs already exists.** The emitter already produces
  precise per-pc pointer masks (the drop table's gc mask) and the class table
  already carries per-field kinds — the two pieces Go gets from stack maps and
  type maps. Go's dependence on OS threads is incidental; the algorithm needs
  only per-frame PC→map lookup and the ability to suspend one stack.
- **The one real runtime addition:** the arena cannot enumerate objects — bump
  allocation plus size-class free lists, with large objects on bare `malloc`
  and no size headers anywhere. Sweep needs its own list. Retiring `rc`, plus
  the `borrow` word that traced objects never use, frees exactly eight
  contiguous bytes for an intrusive link, so the 16-byte header survives.
- This iteration **supersedes part of iteration 2's memory model** (§4 of the
  OOP spec) and closes iteration 4's open gate clause. Neither is renumbered;
  both carry pointers here.

## Proposed Solution

- Write the implementation plan from the approved spec, then execute it: the
  `gcinfer.ml` pass (Tarjan SCC over the class-reference graph, then demand
  promotion to a fixpoint, with a note per decision), the `@gc` removal and
  its diagnostic, retiring `RC_INC`/`RC_DEC` and the rc machinery from
  `owner.ml`/`emit.ml`, the per-shard traced list and sweep, incremental
  tri-color marking with roots read from the existing pc masks, the Yuasa
  deletion barrier inside the VM's store paths, and the doc/golden migration
  the spec's §8 table enumerates.

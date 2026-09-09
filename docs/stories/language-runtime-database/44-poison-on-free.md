---
track: language-runtime-database
iteration: "44"
status: done
readiness: ready
review_pending: "forks auto-approved 2026-09-09 for autonomous execution — developer second review; design is language 41's decision 3, lifted into its own iteration; landed 2026-09-09 (obj.c poison + offset-8 link, gc.c double-free abort, test_arena 17/0, full suite + db-actor gate green)"
---

# 44 — poison-on-free: a freed block can never pass for a live object

> Split out of [language 41](41-actor-arena-crash.md) (its decision 3), where it
> was named and deferred. Brainstormed to `ready` 2026-09-09; the design is the
> one 41 recorded, made concrete against `runtime/src/obj.c`.

## Why this exists

Language 41's hang was a double free that the runtime could not *see*.
`wo_arena_free` pushes a block onto its size class's freelist by writing the
next-pointer over the block's first 8 bytes — exactly the `wo_hdr` fields
`class_id` (0..3), `shard_id` (4..5), `flags`, `pad`. When the block is the
tail of its class that pointer is NULL, so the dead header reads back
`class_id 0, shard_id 0` — a **valid class index on the primary shard**. A stale
drop then ran `class_free` with class 0's field kinds over dead memory, forged
further headers out of freelist links, and self-routed forever. Every one of
those steps would have been an immediate, named trap if a freed block simply
could not look alive.

That is the whole iteration: stamp a poison on free, and make the drop path
refuse it. It is defensive — 41's marshal fix removed the double free at its
source — but it turns any *future* ownership bug into a one-line diagnostic at
the first stale drop instead of a silent corruption or a spin.

## Decisions locked

1. **The freelist link moves to offset 8.** Every arena block is at least 16
   bytes (`round16`; `sizeof(wo_hdr) == 16`, statically asserted), and on a dead
   block the header's second 8 bytes — the `borrow`/`gclink` union — carry no
   meaning. The link lives there; the first 8 bytes are free for a poison. Both
   sites move together: the push in `wo_arena_free` and the pop in
   `wo_arena_alloc`. Nothing else reads the link (checked: `obj.c` only).
2. **The poison is a reserved class id plus an impossible shard.**
   `class_id = WO_CLS_FREED` (`0xFFFFFFFF`, the one sentinel value the
   `WO_CLS_*` space does not use), `shard_id = 0xFFFF` (no shard; also trips
   `wo_route_free`'s `>= nshards` guard if a freed block ever reaches it),
   `flags = pad = 0`. Stamped on every arena free; a fresh allocation overwrites
   the header as it always did, so a *live* object never carries the poison.
3. **`wo_drop_obj` refuses a poisoned header, loudly.** Before the home/route
   decision: `class_id == WO_CLS_FREED` is a double free — print the class/shard
   and abort. Every drop path (`class_free`, `multi_free`, `map_free`, the
   settle loop, teardown) funnels through `wo_drop_obj`, so one check covers all.
   It is a fatal, not a catchable trap: continuing past a double free is
   undefined behaviour, exactly as language 41's evidence showed.
4. **Malloc-backed blocks are untouched.** Sizes above `WO_ARENA_MAX_CLASS` go to
   `free()` and never sit on a freelist; there is nothing to poison and glibc's
   own checks apply.
5. **Same-shard and every existing workload are byte-unchanged.** The only
   observable change is that a double free now aborts with a message instead of
   corrupting.

## Phases

- **A — poison + relocate.** `WO_CLS_FREED` in `wob.h`; `wo_arena_free` stamps
  the poison and stores the link at offset 8; `wo_arena_alloc` pops from
  offset 8. Verify: `test_arena` — a freed block reads `WO_CLS_FREED`/`0xFFFF`;
  a same-class allocation hands the same block back; two frees chain in LIFO
  order through the relocated link; the malloc path is untouched.
- **B — the refusal.** `wo_drop_obj` aborts on `WO_CLS_FREED`. Verify: the full
  runtime battery and the lang-41 fixtures (`just db-actor`) are unchanged — no
  live object is ever poisoned.

## Acceptance Criteria

- **Given** any arena block, **when** it is freed, **then** its header reads
  `class_id == WO_CLS_FREED` and `shard_id == 0xFFFF` until it is reallocated.
- **Given** a freed block, **when** the same size class is allocated, **then**
  the freed block is returned and its header is rewritten by the allocator.
- **Given** two freed blocks of one class, **when** the class is allocated
  twice, **then** they return in LIFO order — the relocated link chains.
- **Given** a poisoned header reaching `wo_drop_obj`, **when** dropped,
  **then** the runtime aborts with a message naming the double free.
- **Given** every existing test binary, corpus fixture and gate, **when** run,
  **then** results are unchanged (no live object carries the poison).

## Out Of Scope

- **Poisoning the block body** (beyond the header) or guard pages — the header
  check is what catches the class of bug 41 exposed; body poisoning is a
  debug-build luxury with a real cost.
- **A catchable trap** instead of an abort — a double free is not a recoverable
  program state.
- **The remaining language-41 follow-ups** (the `try…catch nil` Int-0 ambiguity,
  the `json.decode as T` cross-return corruption) — each its own fixture and fix.

## Info

Pure `runtime/src` (obj.c, gc.c, wob.h) plus a `test_arena` KAT. A few lines;
the value is that the next ownership bug announces itself.

# Iteration 2 — VM core (`wovm`)

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).

## Goals

- A C, libc-only virtual machine that loads a `.wob` bytecode module and
  executes method calls with the language's memory model enforced: owned
  objects with deterministic drops, runtime borrow checks at residual
  sites, and `@gc` classes reference-counted — **RC only in this
  iteration**; the cycle collector is staged to iteration 8 (see Info).
- Arithmetic and text operations execute correctly — the language's first
  observable behavior.

## Acceptance Criteria

- What to achieve?
    - **Given** a hand-assembled `.wob` fixture with arithmetic and method
      calls,
    - **when** `wovm` loads and runs it,
    - **then** output matches the fixture's expectation and a corrupted
      image is rejected by the loader before any instruction executes.
- What to achieve?
    - **Given** a fixture that acquires conflicting exclusive borrows,
    - **when** it runs,
    - **then** the VM traps with the borrow-violation code, unwinds running
      every drop, and ASan/Valgrind report zero leaks on both success and
      trap paths.
- What to achieve?
    - **Given** `@gc` objects whose aliases are created and dropped,
    - **when** the last reference drops (`rc == 0`),
    - **then** the object frees immediately, elided RC pairs stay elided,
      and ASan/Valgrind report zero leaks on every acyclic fixture.
- What to achieve?
    - **Given** a cyclic `@gc` graph that becomes garbage,
    - **when** the corpus runs,
    - **then** the leak is *expected and asserted* by a must-leak fixture —
      the recorded debt iteration 8's cycle collector retires.

## Out Of Scope

- The OCaml compiler (iteration 3) — fixtures here are assembled by the
  test tool, not compiled.
- Threads, shards, mailboxes (iteration 8); any DB or HTTP capability.
- The Bacon–Rajan cycle collector — staged to iteration 8, where the
  shard's event loop (its per-tick budget host) first exists. Until then a
  cyclic `@gc` graph leaks, documented and fixture-asserted.

## Info

- The 16-byte object header reserves a shard id now so iteration 8 needs no
  relayout.
- Staging rationale (decided 2026-08-08): trial deletion under mutation is
  the subtlest piece of this iteration, while every sample to date needs
  zero cyclic `@gc` (log-watcher: none; pricing: one acyclic cache). Swift
  ships RC-without-cycles at mass scale. The header's `IN_CYCLE_BUF` flag
  bit and the possible-cycle buffer hook stay reserved, so iteration 8
  adds the scan without relayout or opcode changes.
- Format contract: `docs/plan/oop-vm/00-wob-format.md` twinned with
  `runtime/src/wob.h`; ~40-op register instruction set, computed-goto
  dispatch.

## Proposed Solution

- Execute the existing plan: `docs/superpowers/plans/2026-08-01-wob-format-and-vm-core.md`
  (16 TDD tasks: arena, object model, borrow word, RC, test assembler,
  validating loader, interpreter, drop-map unwinding, builtins, CLI +
  `just` gate) — with its cycle-collector task deferred: that task moves
  to iteration 8's plan, replaced here by the must-leak cycle fixture.

# Iteration 2 — VM core (`wovm`)

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](../00-story.md).

## Goals

- A C, libc-only virtual machine that loads a `.wob` bytecode module and
  executes method calls with the language's memory model enforced: owned
  objects with deterministic drops, runtime borrow checks at residual
  sites, and `@gc` classes collected without stop-the-world pauses.
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
    - **Given** a cyclic `@gc` object graph that becomes garbage,
    - **when** the collector's budgeted ticks run,
    - **then** the cycle is freed within the configured budget and no pause
      exceeds the configured slice.

## Out Of Scope

- The OCaml compiler (iteration 3) — fixtures here are assembled by the
  test tool, not compiled.
- Threads, shards, mailboxes (iteration 8); any DB or HTTP capability.

## Info

- The 16-byte object header reserves a shard id now so iteration 8 needs no
  relayout.
- Format contract: `docs/plan/oop-vm/00-wob-format.md` twinned with
  `runtime/src/wob.h`; ~40-op register instruction set, computed-goto
  dispatch.

## Proposed Solution

- Execute the existing plan: `docs/superpowers/plans/2026-08-01-wob-format-and-vm-core.md`
  (16 TDD tasks: arena, object model, borrow word, RC + cycle collector,
  test assembler, validating loader, interpreter, drop-map unwinding,
  builtins, CLI + `just` gate).

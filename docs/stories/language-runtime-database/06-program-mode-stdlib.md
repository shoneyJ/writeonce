---
iteration: "6"
status: done
---

# Iteration 6 — program mode + systems stdlib

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).


> **Status (2026-08-14):** ✅ landed for the surface the driving workload uses —
> program mode (`fn main(args: multi Text) -> Int`, argv from the runtime, the
> return value as the exit code), the text/container builtins, and the OS half
> (`fs`, `time`, `env`, `net`, `proc`) with predeclared `Stat`/`TimeParts`/
> `Proc` records, plus `json` encode/decode over `.wob` v2 class metadata.
> What the workload never calls was not written. Two lifetime defects found
> here are being fixed as part of
> [`plan/compiler/2026-08-14-logwatcher-executable.md`](../../plan/compiler/2026-08-14-logwatcher-executable.md):
> the runtime's own argv container is never freed, and blocking `accept`/`read`
> ignore the stop signal.

## Goals

- writeonce stops being server-only: a project with a free
  `fn main(args) -> Int` compiles as a CLI program with exit codes — the
  daemon/tool shape log-watcher represents.
- Five typed builtin modules — `fs`, `proc`, `net`, `time`, `json` — give
  programs system access with no FFI hole: bounded reads, args-array-only
  process runs, TCP listen/accept, monotonic time, typed JSON decode.
- Every handle is an owned object whose drop closes it: RAII from the
  ownership model, leaked fds impossible by construction.

## Acceptance Criteria

- What to achieve?
    - **Given** a program with `fn main`,
    - **when** `wo run` executes it and `woc build` packages it,
    - **then** the return value propagates as the process exit code in both
      forms, and SIGTERM flips `env.stopping()` without any callback
      machinery.
- What to achieve?
    - **Given** the stdlib corpus against real resources (tempdir files,
      rename-simulated rotation, spawned trivial processes, loopback
      sockets),
    - **when** the suite runs under ASan,
    - **then** all fixtures pass, and the fd-battery fixture (open many
      handles in a loop) shows no fd-count growth.
- What to achieve?
    - **Given** a JSON document missing optional fields or shaped wrongly,
    - **when** `json.decode … as Record` runs,
    - **then** missing optionals decode as nil, shape mismatches yield nil
      overall, and no trap fires — expected absence is data, not error.

## Out Of Scope

- The log-watcher sample itself (iteration 7 proves this slice).
- UDP/TLS, fs mutation beyond append, signal callbacks, worker threads.

## Info

- One API, two disciplines: the same stdlib calls are blocking in program
  mode and loop-integrated on server shards — no `async` keyword exists.
- Spec: `docs/superpowers/specs/2026-08-01-systems-track-design.md` Parts 2–3.

## Proposed Solution

- Execute the existing plan: `docs/superpowers/plans/2026-08-01-program-mode-stdlib.md`
  (program entry + env module, time, fs with inode stat + bounded
  `read_at`, proc.run with capped capture, net RAII handles, typed json,
  sys corpus battery in the `oop-accept` gate).

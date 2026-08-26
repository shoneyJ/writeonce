---
iteration: "28"
status: hold
---

# Iteration 28 — skillhost: a host-shaped workload, and the capability gaps it exposes

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).
>
> **Inserted 2026-08-16.** A driving-workload iteration, the way iteration 7's
> log-watcher drove the systems stdlib. The workload is a writeonce port of
> **`~/projects/skillhost`** (a C++ MCP host that links `libllama` in-process,
> discovers filesystem skills, and runs their scripts under confinement). The
> point is not the port for its own sake — it is that a *host-shaped* program
> (an MCP tool server that orchestrates subprocesses) exercises runtime
> capabilities no prior sample needed, and this iteration names each gap so it
> becomes schedulable.
>
> **No spec exists yet.** This iteration frames the workload and records the
> gaps (surveyed 2026-08-16 against the running compiler + the skillhost
> source); each gap below is a candidate iteration of its own.

## Goals

- **A writeonce program that is skillhost's shape**: one MCP tool
  (`delegate_task`) exposed to a hosted caller, an on-disk skill catalog
  discovered at startup, and a confined subprocess runner that executes a
  skill's scripts and returns exit code + captured output. Target sample:
  `docs/examples/skillhost`.
- **Drive the runtime's missing host capabilities into the open.** Each thing
  the port cannot express today is a named gap with a decision attached — the
  same discipline log-watcher used to grow `fs`/`proc`/`net`/`time`.
- **Ship the expressible part now, block honestly on the rest.** Much of
  skillhost is already writable (below); the port lands in the pragmatic
  variant that avoids the hard blockers, and the blockers become their own
  iterations.

## What is already expressible (verified 2026-08-16)

- **The skill catalog** — `@table` with the query surface already exceeds
  skillhost's in-memory SQLite `skills` table; iteration 27's
  `docs/examples/skill-catalog` is literally this table, running. (Or a plain
  `map`/`multi` would do — the catalog is a lookup cache, not persistence.)
- **Discovery** — `fs.exists`/`fs.list` (one level) + `fs.read_all` walk
  `.agents/*/SKILL.md` exactly as skillhost does one level deep.
- **Frontmatter** — skillhost's own parser is a hand-rolled YAML subset
  (`--- … key: value …`); that is plain Text parsing in writeonce, no YAML
  library needed.
- **Config** — `env.get` (the `SKILLHOST_*` overrides), `fs.read_all` +
  `json.decode` (the config file), `fn main(args)` (flags). The "context
  gate" is pure Int arithmetic (`tokens + max_tokens >= n_ctx`); skillhost
  does **no** runtime VRAM query, so none is needed.
- **Single-threaded serving** — skillhost handles one request at a time (its
  mutexes are defensive only); writeonce's blocking single-thread model maps
  cleanly, as log-watcher's MCP mode already proved.

## The gaps (each a candidate iteration)

### Blocker A — in-process native library binding (FFI)

skillhost links `libllama`'s C API in-process (`llama_model_load_from_file`,
`llama_decode`, the sampler chain, GBNF grammar, tokenizer, chat template).
writeonce has **no FFI**, so none of it can be linked or called. The
architecture doc is explicit that in-process linkage is *what forces C++* —
so this gap is the whole reason skillhost is not already portable.

Two directions, and they are a real fork, not a detail:

- **FFI as a language capability** — a way to declare and call C functions
  from writeonce. This is a large, doctrine-level addition (the runtime is
  libc-only by principle; the one sanctioned exception so far is the vendored
  Ed25519, 21). FFI would reopen the dependency-sprawl question the whole
  project is built to avoid. Likely its own spec, likely contested.
- **Out-of-process model, no FFI** — drive a llama.cpp binary via `proc`
  (`llama-cli`) or `llama-server` over `net` + `json` (it accepts a GBNF
  `grammar` param, so grammar-constrained tool calls survive). This needs
  **nothing new** and is how a host in any language would do it — the doc
  notes `llama-server` "a host in any language could drive." It loses
  skillhost's per-turn `llama_memory_clear` + custom sampler control, which
  become server request options. **This is the variant the port should use.**

Leaning: build the port on the out-of-process model; record FFI as a separate
iteration to be opened only if a workload genuinely needs in-process linkage,
and expect it to be weighed hard against the libc-only doctrine.

### Blocker B — stdio transport (process stdin/stdout)

skillhost speaks MCP as **newline-delimited JSON-RPC over stdin/stdout**
(`server.serve(std::cin, std::cout)`), which is what a stdio-launching MCP
client expects. writeonce has **no stdin builtin and no stdout-write
builtin** — only `net` sockets and `print`. So a faithful stdio transport is
impossible today.

- The pragmatic port re-hosts MCP on a **TCP socket** (`net.listen/accept/
  read/write` + newline framing + `json`), exactly as log-watcher's MCP mode
  does — a legitimate but *different* transport than skillhost's stdio.
- The real capability gap: **raw stdin/stdout byte I/O** (read fd 0, write fd
  1, flush), so a writeonce program can be a stdio-transport MCP server — the
  default an MCP client launches. Small stdlib addition (`io.stdin_read` /
  `io.stdout_write`, or an `fd` surface). A candidate iteration.

### Blocker C — bounded, killable subprocesses

skillhost's `run_script` spawns a script, drains stdout+stderr, and — on a
120s deadline — `SIGKILL`s the whole **process group** so backgrounded
grandchildren die too (`poll`-with-deadline + `kill(-pid, SIGKILL)`).
writeonce's `proc.run` captures stdout/stderr/exit but has **no timeout, no
signal control, no process-group kill**, and blocking-only I/O with no
threads means it cannot wait on a child with a wall-clock deadline. A runaway
skill script cannot be bounded — unacceptable for a host that runs untrusted
scripts.

The capability gap: **`proc.run` with a timeout that kills the child (group)
on expiry** — a deadline-bounded subprocess. This composes with the
log-watcher Task-4 stop-flag work (interruptible blocking calls) and is a
clear candidate iteration.

### Partial cluster — filesystem metadata

Smaller gaps, all in `fs`:

- **Recursive directory walk** — skillhost lists a skill's executables/
  references recursively; `fs.list` is one level, so this is built by hand
  (fine) or `fs` grows a recursive walk.
- **Executable-bit detection** (`access(X_OK)`) — skillhost only offers
  runnable scripts to the model; unclear whether `fs.stat` exposes mode bits.
  Candidate `fs.stat` extension.
- **Symlink-resolving path confinement** (`weakly_canonical`/`realpath`) —
  skillhost resolves symlinks before its prefix check so a link pointing out
  of the skill dir is rejected. writeonce has no `realpath`; a string-prefix
  check alone is **weaker** (a symlink escape). Candidate `fs` addition —
  and a security-relevant one, since confinement is the host's trust boundary.

## Acceptance Criteria

- What to achieve?
    - **Given** the expressible subset (catalog + discovery + frontmatter +
      config + a TCP-hosted `delegate_task` tool + a subprocess runner),
    - **when** `docs/examples/skillhost` is written and compiled,
    - **then** it runs as an MCP server over a socket, discovers `.agents/*`
      skills, answers `initialize`/`tools/list`/`tools/call`, and runs a
      skill's script returning its exit code and captured output — the
      skillhost shape, minus the named blockers.
- What to achieve?
    - **Given** a skill script that runs forever,
    - **when** the runner has no bounded-subprocess capability (Blocker C),
    - **then** the port documents that it cannot yet bound it — the gap is
      demonstrated, not hidden, and the acceptance records it as the reason
      Blocker C's iteration exists.
- What to achieve?
    - **Given** each gap iteration lands (bounded subprocess, stdio
      transport, fs metadata, and — if ever — FFI),
    - **when** the port adopts it,
    - **then** the port moves one step closer to skillhost's exact behavior,
      and the divergence list in its README shrinks by exactly that gap.

## Out Of Scope

- **In-process `libllama` / CUDA offload** — requires Blocker A (FFI); the
  port uses an out-of-process model instead and says so.
- **Runtime VRAM/NVML introspection** — skillhost does none (the context
  gate is arithmetic); nothing to build.
- **A resident MCP-over-stdio server** until Blocker B lands — the port uses
  a socket transport meanwhile.
- **Reproducing skillhost's exact sampler chain / per-turn memory clear** —
  those are in-process libllama specifics; the out-of-process variant
  approximates them with server request options.

## Info

The three blockers are independent and each merits its own iteration; the
filesystem partials could ride together as one small `fs`-metadata iteration.
Priority order by leverage:

1. **Bounded subprocess (Blocker C)** — the smallest, most broadly useful, and
   a hard requirement for any host that runs untrusted scripts. Do first.
2. **stdio transport (Blocker B)** — small, unblocks writeonce being a
   *standard* MCP server (stdio is the default an MCP client launches), useful
   far beyond skillhost.
3. **fs metadata (the partials)** — small, security-relevant (symlink
   confinement).
4. **FFI (Blocker A)** — largest and most contentious; open only on genuine
   demand, and expect the out-of-process variant to make it unnecessary for
   this workload.

The port itself is buildable today in the out-of-process / TCP-transport
variant *once Blocker C exists* (a host that cannot bound a runaway script is
not honest to ship) — so the natural first step is Blocker C, then the port,
then B and the partials narrow the gap to skillhost's real behavior.

## Proposed Solution

- **Brainstorm each gap iteration on demand**, starting with the bounded
  subprocess (Blocker C) since the port cannot responsibly run scripts
  without it.
- **Write `docs/examples/skillhost`** in the out-of-process / socket-transport
  variant, with a README whose "divergence from the C++ skillhost" list is
  exactly the open gaps (A: out-of-process model, B: socket not stdio, C:
  bounded once its iteration lands, plus the fs partials) — the list is the
  iteration's own scoreboard.
- Reuse iteration 27's `skill-catalog` as the catalog layer, log-watcher's
  MCP mode as the transport skeleton, and the systems stdlib for discovery
  and execution.

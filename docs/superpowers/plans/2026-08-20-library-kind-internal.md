# Iteration 17 — library kind + `internal/`: implementation plan

> **Status: ⏸ PARKED 2026-08-20** (developer directive: framework v1 work
> proceeds instead; this plan stays ready on branch `library-internal`,
> execution not started). Board: [docs/00-status.md](../../00-status.md).

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.
>
> **Style rule (user convention):** this plan carries concept, reason, and
> required behavior in words plus verification commands only — no
> implementation or test code blocks; the executor writes the code.

**Goal:** `kind = "library"` in `wo.toml` makes a project checkable without
an entry, the `internal/` rule keeps a dependency's plumbing private
(WO-E108), and the framework adopts both — `just web-app` proves the public
surface unmoved.

**Architecture:** every change is compile-time and lives in the driver
(`compiler/bin/main.ml`): the manifest reader learns one key, the manifest
build path grows a check branch, and the dep-use resolution walk in
`compile_image` grows the boundary rule. No lexer, parser, typechecker, VM,
`.wob`, or GC change — the spec's impact analysis is normative.

**Tech Stack:** OCaml stdlib only (`woc`), bash acceptance scripts, `just`.

**Spec:** [`../specs/2026-08-20-library-kind-internal-design.md`](../specs/2026-08-20-library-kind-internal-design.md)
(normative). Story:
[`17-library-projects-internal.md`](../../stories/language-runtime-database/17-library-projects-internal.md).

## Global Constraints

- Branch `library-internal`; commits local only, never push.
- OCaml stdlib only; no new executables, no new build steps.
- Absent `kind` means program — every existing project's behavior is
  byte-identical; the standing gates prove it each task.
- Exit-code doctrine stays: 0 clean, 1 diagnostics, 2 usage/manifest/IO.
  WO-E108 is a diagnostic (exit 1, printed by the normal collector path);
  WO-E109 is a manifest error (exit 2, `woc: <file>: error WO-E109: ...`,
  the WO-E106 shape).
- `internal` matches a whole path SEGMENT of a dep-relative module path —
  never a substring.
- Gates that must stay green after every task: `just woc-test`,
  `just oop-e2e`, `just deps-accept`, `just web-app`, `just log-watcher`,
  `just employee`.

## Spec deviations, disclosed up front

1. The spec put WO-E108/E109 fixtures in the compile-fail corpus. The
   corpus harness (`scripts/oop-e2e.sh`) compiles a fixture DIRECTORY with
   no manifest and no network — a `wo.toml` in a fixture dir would flip woc
   into manifest-build mode, and WO-E108 additionally needs a fetched dep.
   Both diagnostics are therefore gated in `scripts/web-app-accept.sh`
   (which already builds a file:// dep chain), not the corpus. Task 5.
2. The spec says check mode "stops before image emission". The cheapest
   correct implementation reuses `compile_image` whole — emission happens
   in memory and the image is discarded; no artifact is written, no entry
   is required (an entry-less image is already legal there, the `--emit`
   precedent). Behavior matches the spec; the pipeline boundary is one
   step later than the spec's wording.
3. `woc build <dir> -o <out>` never reads the manifest, so it resolves no
   `[deps]` (pre-existing, iteration 15). The dual lib+bin case therefore
   holds for libraries without `[deps]` — the framework qualifies. Recorded
   here, not fixed; a dep-aware explicit build is future work.

---

## Task 1 — the manifest `kind` key + WO-E109

**Files:**
- Modify: `compiler/bin/main.ml` (`manifest_parse` known-key table
  ~line 808; `manifest_build` ~line 1023).

**Interfaces:**
- Produces: `manifest_parse` accepts top-level `kind`; `manifest_build`
  exposes the validated kind ("program" when absent) to Task 2's branch.

- [ ] `manifest_parse`: add `kind` to the known TOP-LEVEL keys (the match
  arm that today allows `name`, `version`, `description`). Nothing else in
  the parser changes — the value is a quoted string like every other key.
- [ ] `manifest_build`: after the `name` check, read `kind`. Absent or
  `"program"` continues to build. `"library"` is Task 2's branch (for this
  task, temporarily fall through to build — the framework does not carry
  the key until Task 4, so nothing observable changes). Any OTHER value
  fails as a manifest error in the WO-E106 print shape with code WO-E109,
  naming the given value and the two legal ones, exit 2.
- [ ] Verify by hand: a scratch project under the scratchpad with
  `kind = "junk"` refuses with WO-E109 and exit 2; the same project with
  `kind = "program"`, and with no `kind` line, builds as before.
- [ ] Verify nothing moved: `just woc-test && just deps-accept && just employee`.
- [ ] Commit.

## Task 2 — library check mode + the build-error hint

**Files:**
- Modify: `compiler/bin/main.ml` (`manifest_build`; `build_mode` no-entry
  error ~line 639; `usage_msg` ~lines 42–52 and the long help's build
  paragraph ~line 96).

**Interfaces:**
- Consumes: Task 1's validated kind.
- Produces: `woc <dir>` on a `kind = "library"` manifest runs the full
  check; the behavior Tasks 4–5 verify against.

- [ ] `manifest_build`, kind `"library"`: resolve `[deps]` and enforce the
  `[runtime]` constraint exactly as build does (a library must be
  checkable offline once locked), then run `compile_image ~deps dir`,
  print diagnostics through the normal `finish` path on error (exit 1),
  and exit 0 silently on success. No `target/` directory is created, no
  file is written, no entry is required. The full pipeline runs — parse,
  typecheck, interface satisfaction, ownership, GC inference — because
  `compile_image` already runs it; the in-memory image is discarded
  (deviation 2).
- [ ] `build_mode`, the no-entry error: when `<dir>/wo.toml` exists and
  names `kind = "library"`, append the library hint to the existing
  message — the project is a library; add a `main` for a demo binary or
  check it with `woc <dir>`. Read the manifest only if the file exists;
  malformed manifests keep failing as they do today.
- [ ] `usage_msg`: the `woc <dir>` line notes "builds a program / checks a
  library, per the manifest's `kind`"; the long help gains two sentences
  on `kind` and check mode. No new flags.
- [ ] Verify by hand: scratch library project (`kind = "library"`, one
  class, no `main`) — `woc <dir>` exits 0 with no output; plant a type
  error, `woc <dir>` exits 1 with the normal diagnostic; `woc build <dir>
  -o x` fails with the no-entry message plus the library hint; add a
  `main`, `woc build` produces a runnable binary (dual case) while
  `woc <dir>` still only checks.
- [ ] Verify nothing moved: `just woc-test && just oop-e2e && just deps-accept`.
- [ ] Commit.

## Task 3 — the `internal/` boundary rule (WO-E108)

**Files:**
- Modify: `compiler/bin/main.ml` (`compile_image`'s dep-use walk,
  ~lines 487–518).

**Interfaces:**
- Consumes: the existing walk that already knows, per file, whether a dep
  owns it (`owner`) and prefixes dep-internal use paths.
- Produces: WO-E108 diagnostics in the collector; Task 5 gates on the code
  string.

- [ ] In the same `List.map` over parsed files: for a file NOT owned by any
  dep (the `owner = None` branch, today untouched), inspect each `Use`
  whose FIRST segment names a dep in `deps` — when any LATER segment is
  exactly `internal`, add a diagnostic to the collector: code WO-E108, the
  use's file and `pos`, message naming the internal module path and the
  dependency it belongs to (the `Diag.error` + `Collector.add` pattern at
  ~line 277). Do not drop the use — the collector's has-error path already
  prevents emission, and later resolution errors on the same use are
  harmless duplicates suppressed by exit-on-first-report ordering as
  today.
- [ ] Dep-owned files stay untouched on this path — the boundary is
  consumer-only (spec §3): the dep's own `use internal` (prefixed to
  `<dep>/internal` by the existing arm) must keep compiling.
- [ ] The root project's own `internal/` directories are NOT matched: the
  rule keys on the first segment being a DEP name, so a root-project
  `use internal` never fires it. No code needed — assert it in the Task 5
  gate instead.
- [ ] Verify by hand: temp dir pair — a dep with an `internal/` module and
  a consumer importing `<dep>/internal` — compile fails exit 1 printing
  WO-E108 at the `use`'s line; the dep's own file importing `internal`
  compiles.
- [ ] Verify nothing moved: `just woc-test && just deps-accept && just web-app`.
- [ ] Commit.

## Task 4 — framework reorg: adopt `kind` + `internal/`

**Files:**
- Modify: `docs/examples/writeonce-framework/wo.toml` (add
  `kind = "library"`), `app.wo` (imports), `README.md` (verification note).
- Move: `http/parse.wo` → `internal/parse.wo`,
  `http/serve.wo` → `internal/serve.wo` (git mv; module becomes
  `framework/internal` under a consumer, `internal` standalone).
- Not touched: `http/types.wo`, `router/router.wo`, the web-app.

**Interfaces:**
- Consumes: Task 2's check mode (standalone verification), Task 3's rule
  (what the move protects).
- Produces: the reorganized layout Task 5's gate checks against.

- [ ] Move the two plumbing files. Same-module access dies with the move:
  `internal/serve.wo` and `internal/parse.wo` now need `use http` for
  `Req`/`Resp` (they shared the `http` module with `types.wo` before);
  `app.wo` adds `use internal` for the serve loop and `Parsed` seam. No
  declaration changes — imports only.
- [ ] `wo.toml` gains `kind = "library"` (top-level, beside `name`).
- [ ] README: replace the `--emit` verification workaround sentence with
  the check-mode invocation (`woc <dir>` — full pipeline, no entry), and
  one sentence on `internal/` being unimportable by consumers.
- [ ] Verify: `compiler/_build/default/bin/woc docs/examples/writeonce-framework`
  exits 0 silently — the workaround is dead.
- [ ] Verify the consumer: `just web-app` — all 14 standing checks pass
  unchanged (the app imports only `framework`, `framework/http`,
  `framework/router`, so NOTHING in it changes).
- [ ] Commit.

## Task 5 — gate: three new checks in web-app-accept

**Files:**
- Modify: `scripts/web-app-accept.sh` (after the build check, before the
  serve matrix).

**Interfaces:**
- Consumes: the `$W/fw` framework copy and `$W/app` consumer the script
  already builds; Tasks 1–4's behavior.
- Produces: `just web-app` at 17 checks, the iteration's single gate.

- [ ] Check 15 — library check mode: `woc "$W/fw"` (the framework copy,
  which now carries `kind = "library"` and no `main`) exits 0 with empty
  output.
- [ ] Check 16 — the boundary: copy the app to a second temp dir, append a
  `use framework/internal` line to its `main.wo`, compile; assert exit 1
  and `WO-E108` in stderr.
- [ ] Check 17 — kind validation: copy the app again, set
  `kind = "junk"` in its manifest, compile; assert exit 2 and `WO-E109`
  in stderr.
- [ ] Renumber nothing — the script counts dynamically (`pass`/`fail`);
  update only the header comment's check inventory.
- [ ] Run the full battery: `just web-app` (17/0) and the standing gates —
  `just woc-test`, `just oop-e2e`, `just deps-accept`, `just log-watcher`,
  `just employee`.
- [ ] Commit.

## Task 6 — docs closeout

**Files:**
- Modify: `docs/00-status.md` (NEXT PLAN advances; board row 17 → done
  with what landed; in-progress row moves to the next order item),
  story `17-library-projects-internal.md` (landing banner),
  `compiler/src/CODE-LOGIC.md` (driver section: `kind`, check mode, the
  boundary rule), `docs/08-project-structure.md` (framework layout gains
  `internal/`), framework story `16-web-framework.md` only if it names the
  `--emit` workaround.
- [ ] Apply; every claim carries its measured gate result (17/0 etc.); no
  forward-looking "will".
- [ ] `just web-app` once more after the doc edits (nothing should move —
  honesty check).
- [ ] Commit.

## Success criteria (spec, restated as the gate reads them)

1. Framework checks entry-less (`woc <dir>` exit 0) and `woc build` on it
   fails naming the library kind — gate check 15 + Task 2's hand check.
2. A consumer import of `framework/internal` is WO-E108 at the `use`;
   the framework's own import stays legal — gate check 16 + Task 3.
3. `just web-app`'s original 14 checks pass unchanged after the reorg —
   Task 4/5.
4. A manifest without `kind` behaves byte-identically — every standing
   gate, every task.

## Self-review notes

- Spec coverage: §1 kind key → Task 1; §2 driver modes → Task 2; §3 rule →
  Task 3; §4 diagnostics → Tasks 1–3; §5 reorg → Task 4; §6 gate → Task 5;
  out-of-scope list untouched by any task. Corpus-fixture clause replaced
  by deviation 1; emission-boundary wording by deviation 2; dual-with-deps
  limit by deviation 3.
- Type consistency: the only cross-task names are the two code strings
  (WO-E108, WO-E109), the manifest key `kind`, and the module path
  `framework/internal` — spelled identically in every task.
- Risk, disclosed: moving serve/parse breaks same-module visibility they
  silently enjoyed beside `types.wo`; Task 4 names the exact import each
  file must gain, and the standalone check catches any miss before the
  gate runs.

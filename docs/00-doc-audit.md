# Documentation truth audit — 2026-08-26

> **Status: findings resolved 2026-08-26, same day.** Every section below was
> acted on; see [What was fixed](#what-was-fixed) at the end for the
> disposition of each, including **one row where this audit was wrong and the
> document it accused was right** (B3's `WO-W201` claim). The findings are kept
> as written — a fix list whose findings have been edited away cannot be
> checked. `just linkcheck` went from 77 broken paths to 23, and all 23 that
> remain are in `.dev/`, which this repo does not author.
>
> A separate structural directive landed the same day and **removed the status
> folders** (`done/`, `refine/`, `hold/`, `in-progress/`) — status now lives only
> in frontmatter. Paths of the form `…/done/NN-*.md` quoted in the findings below
> were correct when written and no longer resolve; see
> [Structural change 2026-08-26](#structural-change-2026-08-26--status-folders-removed).

Scope: every `*.md` that documents THIS repo — root `README.md`, the numbered
`docs/0*.md`, `docs/guides/`, `docs/stories/`, `docs/plan/`, `docs/examples/`,
`docs/superpowers/`, the code-directory READMEs and `CODE-LOGIC.md` files,
`tests/corpus/README.md`, `bench/compare/go-sqlite/README.md`,
`scripts/install-readme.tmpl.md`, `.claude/agents/database-developer.md`.

Excluded, and why: `.dev/skills/` (vendored copies of plugin skills, not ours),
`.dev/reference/` (other people's codebases), `.superpowers/sdd/` (dated task
reports — snapshots, correct as history).

Method: claims were checked against the tree, not read off prose. `woc`
(`compiler/_build/default/bin/woc`) and `wovm` (`runtime/wovm`), both built
2026-08-25, were run against every sample; `justfile` recipes, `wob.h`
constants, `types.ml`'s builtin tables, story frontmatter and
`scripts/linkcheck.py` were used as ground truth. Nothing in this report is
inferred from another document.

Verdict: **the deep reference docs are in good shape; the front door is not.**
`docs/guides/language-surface.md`, `docs/plan/oop-vm/08-builtin-surface.md`, the
three `CODE-LOGIC.md` files and the status board's tables track the code
closely. The root `README.md`, `runtime/README.md`, `docs/00-code-review.md`,
`docs/00-dependency-graph.md` and two example status banners describe a repo
that stopped existing between one and six weeks ago.

No document was changed by the audit pass itself — the findings below record the
tree as it stood before any fix. What was then changed in response is listed in
[What was fixed](#what-was-fixed).

---

## A. Wrong about shipped features (the highest-cost class)

### A1. `README.md` — the Roadmap lists three landed features as unavailable

`README.md:326-342` is headed "Planned, **not yet available**", and
`README.md:10-14` promises "Features that are planned but **not yet available**
are listed separately under Roadmap — they are not described as if they work."
Three of its six entries have shipped:

| README claim | Reality |
| --- | --- |
| `:336` "**Concurrency** — a shard-actor runtime and green-threaded fibers." | Both landed 2026-08-21 (iterations 8 and 11, both `done/`). `spawn` is a lexer keyword (`lexer.ml:163`); `send`/`call` are builtins (`WO_B_SEND=69`, `WO_B_CALL=88` in `wob.h`); `actor M` is a type (`types.ml`'s `TActor`). Gates exist and run: `just fibers`, `just db-actor`. `runtime/src/park.c` is the parking implementation; `runtime/test/test_fiber.c` and `test_mailbox.c` are its unit suites. |
| `:335` "**HTTP service layer** — `service` blocks that route requests to methods." | The *`service` block syntax* is genuinely absent — that half is honest. But it sits under a banner that also denies HTTP entirely, which is false (see A2). |
| `:332` "**Query aggregates** — `group … by … into g`" | **This one is correct.** `types.ml:2220,2241` rejects it: "group-by aggregation is not supported yet". Kept here only because `docs/guides/language-surface.md` contradicts it — see C1. |

### A2. `README.md:33-37` — "does not serve HTTP, WebSockets, or a UI"

> "writeonce is **not** a web framework and does not (yet) serve HTTP,
> WebSockets, or a UI."

Contradicted 30 lines later by its own §"Worked examples" (`:306-313`), which
describes `docs/examples/porch/` as "a web framework written in
writeonce (HTTP/1.1 …, router with `:param` captures, interface-based
handlers)" and `just web-app` as its gate. Also contradicted by:

- iteration 16 (web framework) and 37 (wo-html components), both `done/`;
- `docs/examples/site/` — server-rendered pages, gated by `just site`;
- WebSockets: `docs/examples/porch/http/ws.wo` (`ws_accept`, the 101
  hijack sentinel) and `http/wsframe.wo` (a pure-`.wo` RFC 6455 frame codec),
  both landed per `docs/in-progress/2026-08-23-chat-ws-lifecycle.md` (T6, T7).

### A3. `README.md:30` — "no package manager"

> "**Small on purpose.** No FFI, no package manager, no framework."

Same page, `:265-281`, documents `[deps]`, `.wo-deps/<name>/`, `wo.lock` and
`woc --update-deps`. Iteration 15 (deps package manager) is `done/`;
`just deps-accept` is its gate. "No framework" is contradicted by `:306`.
The intended claim is presumably "no *registry*" — which is true and is what
`docs/00-code-review.md:77` says.

### A4. `docs/examples/employee/README.md:3` — "does not compile on today's toolchain"

> "**Status: target workload — does not compile on today's toolchain.** …
> It becomes buildable when iteration 9 … and iteration 9b … land."

Both landed. `woc docs/examples/employee/` exits 0. `just employee` is a
first-class acceptance gate, and the `justfile:88-91` comment calls it "the
database track's acceptance workload". The banner is ~2 weeks stale.

### A5. `docs/examples/log-watcher/README.md:12-20` — same shape

> "**Status: design artifact — the spec's forcing function.** The systems
> track is approved, pre-implementation. Today's `woc` (milestone 1) …
> diagnoses the adopted surface as WO-E101: `use`, `typedef`, standalone union
> aliases …, `pub(read)`, `switch`, `try`."

Every one of those forms is shipped (`docs/guides/language-surface.md` §2–§5,
verified in `lexer.ml`/`parser.ml`). `woc docs/examples/log-watcher/` exits 0.
`just log-watcher` is the gate the `justfile:82-87` calls "the test the whole
track exists to pass".

Same file, `:8-9`: "the five builtin stdlib modules — `fs`, `proc`, `net`,
`time`, `json`". There are **six** (`types.ml:206`): `env` is missing.

### A6. `runtime/README.md` — describes the pre-2026-08-18 world

The directory's orientation README is still the old `wo-rt-c` prototype page
with the VM bolted on at `:78`. Concretely wrong:

- `:31` "Or from the repo root: `just rt-c-demo`" and `:60` "`just rt-c-bench`"
  — **neither recipe exists.** The justfile has 16 recipes plus two `mod`s;
  no `rt-c-*` among them.
- `:5,:83` "the production Rust runtime (`crates/rt/`)", `:76` "This file is for
  reading; `crates/rt` is for running writeonce" — the Rust runtime was removed
  2026-08-18 (`docs/08-project-structure.md:11`).
- `:3` `prototypes/wo-db/`, `:43` `docs/runtime/database/03-inmemory-engine.md`,
  `:47` `docs/plan/09-concurrency-scaleout.md` — none of these paths exist
  (also in the link audit's sections B/C/E).
- `:84` "**`@gc` reference counting** + budgeted cycle collection … Bacon–Rajan
  trial deletion" — retired by iteration 7b. `@gc` on a class is now
  **rejected** (`docs/guides/language-surface.md:73`), and
  `runtime/src/CODE-LOGIC.md` states the replacement outright: "incremental
  tri-color mark-sweep … (iteration 7b — RC and Bacon–Rajan are gone)".
- `:89` "`DB_STUB` traps 'engine not linked' until the DB engine binds (plan
  5)" — the engine bound in iteration 9. The opcode survives
  (`wob.h:232`, `vm.c:1806`) but the sentence reads as "no database yet".
- `:89` builtin list "`now/print/print_int/words/multi_*/map_*`" — there are
  now ~70 free builtins plus six module namespaces (`types.ml:784-849`).
- File map (`:92-107`) omits four of the thirteen sources in `runtime/src/`:
  `crypto.c/.h`, `json.c`, `park.c/.h`, `sysio.c`.
- `:105` and `:117` "13 suites" / "one of the 13 ASan test binaries" — there
  are **18** (`runtime/test/test_*.c`).
- `:1` "now at **phase E**" vs `:57` "A → B → C → D → E → F, all ✅ shipped"
  vs `:60` "Measured (phase F…)" — three answers in one file.

Verified-correct in the same file, for contrast: `WO_HEAP_MB` / 64 MiB arena,
the four `.vscode/launch.json` configs, the exit-code contract, and the
`make -C runtime` targets.

---

## B. Verification tables that no longer verify

### B1. `docs/00-code-review.md` — the 2026-08-20 table has decayed

The doc's value is that it *checked* a critique line by line. Seven rows of
`:55-82` are now false, and the doc carries no superseded banner:

| Row | Then | Now |
| --- | --- | --- |
| "no `Float`, no `Bytes` \| absent from `compiler/src/types.ml`" | true | `types.ml:174` `builtin_scalars = ["Int";"Bool";"Text";"Timestamp";"Id";"Float";"Bytes"]` — iteration 19, `done/` |
| "`send` is one-way \| `WO_B_SEND=69` is the last builtin (`WO_B_MAX 69u`)" | true | `WO_B_MAX 95u`; `WO_B_CALL = 88` is a send that parks for a typed reply |
| "no crypto primitives \| none" | true | `WO_B_SHA1=85`, `WO_B_SHA256=86`, `WO_B_HMAC_SHA256=87`; `runtime/src/crypto.c`; `runtime/test/test_crypto.c` |
| "22's battery never run \| … no `bench/baseline.json`, no `just db-bench`" | true | `bench/baseline.json` exists, `just db-bench` / `db-bench-quick` exist, 30+ result files in `bench/results/`, iteration 22 is `done/` |
| "no fuzzing, no CI \| **no `.github/`**, no fuzz target" | true | `.github/workflows/release.yml` exists (fuzzing still absent) |
| "one framework, five samples, one consumer" | true | 13 sample projects under `docs/examples/` |
| "accept on one shard \| one listener, `SO_REUSEADDR` only" | true | iteration 35 landed `serve_conn` + fiber-per-connection |
| `:46-51` "The multi-shard DB gap is structural … `wo_builtin_db` returns `WO_T_DB` 'database engine not initialized'" | true | that string is gone from `runtime/src/`; arc stage 3 landed the transparent DB actor, gated by `just db-actor` |

Rows that still hold, checked: interpreted-only/no JIT, no SIMD, the
`WO_STACK_SLOTS 4096` / `WO_MAX_REGS 64` / `WO_MAX_FRAMES 256` correction, no
generics, no closures, byte strings, no Result type, switch-not-destructuring,
no supervision, growable mailboxes, no `timerfd`, no TLS, no debugger/LSP,
deps-are-git-rev-only, blue-green is a future, TSan covers one demo. And
**`map<K,V>` lookup is still a linear scan** — `cont.h:1-6` says so in as many
words.

### B2. `docs/00-link-audit.md` — numbers and paths both stale

Dated 2026-08-20. `just linkcheck` today reports **files=235, local=675,
broken=77, bad anchors=0**; the doc's table says 206 / 569 / 88. Its own
sections B–F sum to 77, not the 88 its prose claims twice (`:12`, `:145`) —
an internal arithmetic error independent of the drift.

Its repair table (`:29-37`) references paths that have since moved:
`docs/00-status.md` (now `docs/stories/00-status.md`), and
`refine/{08,11,19,20,21}` (now under `done/` and `hold/`).

Two broken links exist today that the audit does not account for:

- `docs/examples/employee-list/README.md:5,6` → `…/refine/20-cross-program-tables.md`
  and `…/refine/21-keypair-attach-auth.md`; both files are now in `hold/`.
- `docs/stories/language-runtime-database/hold/26-blue-green-deploy.md:9` →
  `00-story.md`; the sibling stopped being a sibling when 26 moved into `hold/`.

Conversely `docs/00-principles.md:57,77,78,87` — four links the audit lists as
open — resolve now.

### B3. `docs/plan/oop-vm/01-error-catalog.md` — not the complete catalog it claims

`docs/guides/language-surface.md:8` calls it "every diagnostic";
`compiler/README.md:39` says "every shipped code is cataloged" there. Ten
codes the compiler emits are absent:

| Code | Defined at | What it is |
| --- | --- | --- |
| `WO-E003` | `lexer.ml:56` | `#if`/`#else`/`#end` misuse |
| `WO-E108` | `bin/main.ml:277` | `internal/` crossed at a `[deps]` boundary |
| `WO-E109` | `bin/main.ml:275` | unknown `wo.toml` `kind` value |
| `WO-E219`–`WO-E223` | `types.ml` | five type-pass codes |
| `WO-E226` | `types.ml:443` | `call`'s reply type through actor-`M` erasure (iteration 24) |
| `WO-E250` | `types.ml:435` | the whole query surface (iteration 9b) |

`WO-E250` is the notable one: it is the only diagnostic the shipped query
language produces, and it is the code a reader hits first when they mistype a
query. Meanwhile `WO-W201` is still catalogued and no longer exists — the
inferred-GC plan (`docs/superpowers/plans/2026-08-18-inferred-gc-mark-sweep.md:151`)
listed "retire WO-W201" as an amendment; the code went, the catalog entry
stayed.

`WO-E004`/`WO-E005` (raw text literal, iteration 37) *are* catalogued —
checked, since `language-surface.md:29,31` depends on them.

---

## C. Docs that disagree with each other

### C1. Is group-by shipped? Two live docs, two answers

- `README.md:332` — Roadmap, "not yet available". **Correct.**
- `docs/guides/language-surface.md:175` — "Present today: from / where / order
  / take / select **plus group-by aggregation**." **Wrong**, and the same page
  opens (`:16-17`) with "Every form listed below was compiled and run against
  `woc`/`wovm` while writing this page, not read off the parser and hoped for."
  The `group … by … into` clause in its §6 grammar block *parses*
  (`parser.ml:1137-1144`) and is then rejected by the typechecker
  (`types.ml:2241` "group-by aggregation is not supported yet";
  `types.ml:2222` for the navigation form).

Reproduced: `docs/examples/employee-list/main.wo:38-41` uses `group e by
e.dept into g` and fails to compile.

`docs/00-dependency-graph.md:45` correctly still lists group-by under the
parked drain.

### C2. `docs/stories/00-status.md` — the narrative and the table disagree about iteration 24

The board's *Current work* table is right: `:314` records "🔄 **iteration 24
(absorbing 31 + 34): chat + actor lifecycle** — spec + plan approved
2026-08-23 … executing on branch `chat-ws-lifecycle`" and links the marker.

The ▶ NEXT PLAN narrative above it is not:

- `:82-85` "Next slice: **iteration 31, actor lifecycle** — its spec brainstorm
  is the next act". 31 was absorbed into 24 by directive, and its
  actor-death half already landed (`docs/in-progress/2026-08-23-chat-ws-lifecycle.md:20-23`,
  commit `ed69841`).
- `:125` "**Next steps:** 31 (lifecycle spec brainstorm) → 24 (chat) → 23 → 32"
  — same stale ordering.
- `:286` iteration 24 marked "⬜ fourth in chain … after 31".
- `:290` iteration 34 marked "⬜ off-chain but GATES 24". T1 crypto landed
  (`d14fa9f`); `sha1`/`sha256`/`hmac_sha256` are in both `types.ml:847-849`
  and `wob.h:461-463`. The gate is cleared —
  `docs/00-dependency-graph.md:169` already says so.

Also missing from the board entirely: the 2026-08-25 packaging/release track.
`VERSION`, `scripts/mkdist.sh`, `just dist`, `just install-accept`,
`.github/workflows/release.yml`, `docs/guides/releasing.md` and `dist/writeonce-0.1.0-linux-amd64.tar.gz`
all exist; the last five commits are that work; no standup entry covers it.

### C3. `docs/00-dependency-graph.md` — the main graph is a generation behind

Its own header (`:3`) defers state to the board, but it paints state inline
anyway, and the first mermaid graph paints it wrong:

- `:29` `I17["17 library kind + internal/ (**PARKED** — spec+plan ready…)"]:::parked`
  — story 17 is in `done/` with `status: done`; board `:302` reads
  "✅ **landed 2026-08-20**".
- `:31` `I18[… (spec APPROVED — **the next implementation**)]:::specd` — story
  18 is in `hold/`; board `:303` reads "⏸ hold (2026-08-21)".
- `:33,34` iterations 20/21 as `open` — both `hold/`.
- `:38,40,41,42` use the pre-renumber ids: "10 HTTP service layer", "12
  blue-green deploy", "13 metaprogramming @derive", "14 skillhost workload".
  The stories are **26**-blue-green-deploy, **29**-compile-time-metaprogramming,
  **28**-skillhost-host-workload; no story numbered 10, 12, 13 or 14 exists.
- `:45` `DRAIN["post-12 parked drain: pub(read)/using/#if, …"]` — `pub(read)`
  (`ast.ml:118-123`), `using` (`lexer.ml:164`) and `#if` (`lexer.ml:245-249`)
  all shipped.
- The main graph has no node for iterations 19, 24, 31, 32, 33, 35, 36 or 37.
  Four of those are `done/`. The later sub-graphs *do* cover 34/35/36
  correctly (`:141,164-170`), so the drift is confined to the first graph.

---

## D. Structural claims that don't match the tree

### D1. `docs/08-project-structure.md` — "canonical map", four divergences

- `:39` "`plan/` compiler-track docs: architecture.md + the woc plans" under
  `compiler/`. **`compiler/plan/` does not exist**; those docs live at
  `docs/plan/compiler/` — which the same file's `:49` links correctly.
- `:22,76-77` `tests/corpus/` as "`run/`, `compile-fail/`, `trap/`, `gc/`".
  There are nine directories: also `actor/`, `db/`, `lang/`, `sys/`,
  `sample-logwatcher/` — all five **empty**. `tests/corpus/README.md:11-15`
  documents them as planned per-plan additions, so the corpus README is the
  honest one; the structure doc undercounts and neither mentions that five are
  placeholders. (Live fixture counts: run 60, compile-fail 46, trap 5, gc 2.)
- `:79-81` `scripts/` as "`oop-e2e.sh`, `mkdist.sh` + `install-accept.sh`, and
  the per-sample acceptance scripts (`employee-accept.sh`,
  `log-watcher-accept.sh`)". There are 14 scripts; unmentioned:
  `db-actor-accept.sh`, `db-bench.py`, `deps-accept.sh`, `fibers-accept.sh`,
  `linkcheck.py`, `single-binary-smoke.sh`, `site-accept.sh`,
  `web-app-accept.sh`.
- `:89` `examples/` as "log-watcher/, employee/, employee-list/ samples" —
  there are 13.
- `:87` puts status at the `docs/` root; it is `docs/stories/00-status.md`
  (the file's own `:5` links it correctly). The root also has
  `00-dependency-graph.md` and `00-link-audit.md`, unlisted.
- The one-page map (`:17-29`) omits four tracked root entries: `bench/`,
  `dist/`, `.github/`, `.claude/`.
- `docs/examples/db-actor/` has `main.wo`, a `wo.toml` and a gate
  (`just db-actor`) but **no README** — the only sample without one.

Correct in the same file, checked: `runtime/wo-rt.c` exists; `.dev/` is
gitignored except `.dev/README.md` (`git ls-files .dev` returns exactly one
path) and `.dev/reference/` does hold `crates/`, `colibri/`, `llama-cpp/`,
`linux/`, `go/`.

### D2. `compiler/README.md` — stage banner and CLI list both behind

- `:5` "**Stage: plan 3 … complete, Tasks 1–6 + 8**". Plan 3 closed in early
  August; the front end has since taken iterations 15, 17, 19, 24, 34, 35, 36
  and 37. A reader takes this page as the compiler's current extent.
- `:26-34` "Running `woc`" omits four of the nine modes the binary's own
  `usage_msg` prints: `--dump-gc`, `--update-deps <dir>`, `version`, and the
  `woc <dir>` manifest build (the mode `README.md:111` teaches as the primary
  one). `-D <name>`, the `#if` flag setter documented at
  `language-surface.md:34`, is in neither the README nor `usage_msg` —
  it exists at `bin/main.ml:1162`.
- `:37` "same contract as `wo run` (`crates/rt/src/lib.rs::discover`)" — the
  Rust runtime is gone. The identical stale sentence is also the doc comment
  at `compiler/bin/main.ml:16-19`. *(Code, not markdown — noted, not
  changed.)*

Correct: the `=== path ===` multi-file header (`dump.ml:128`), the five golden
stages, the exit-code contract, OCaml 4.14 / dune 3.14 (`dune-project` says
`(lang dune 3.14)`).

### D3. `runtime/src/CODE-LOGIC.md` — file table missing two sources

`:12-25` is a complete-looking table of "the files, in dependency order" and
omits `park.c/.h` (fiber parking — iteration 11, and central to how blocking
builtins work) and `crypto.c/.h` (iteration 34). The doc is dated 2026-08-14,
before both; nothing marks it as of-that-date beyond the first line.

`database/src/CODE-LOGIC.md` and `compiler/src/CODE-LOGIC.md` were checked
against their sources and hold up.

---

## E. Runbook and instruction errors

`docs/guides/releasing.md`, authored 2026-08-25, contains steps that cannot be
followed:

- `:110` (step 11) "Remove `--draft`, commit, push." **`.github/workflows/release.yml`
  contains no `--draft`** — `gh release create` at `:135-139` passes only the
  two asset paths, `--title` and `--generate-notes`. Nothing to remove.
- Steps 5, 6 and 10 disagree with each other. `:50-54` (step 5) says the
  rehearsal is a `workflow_dispatch` run that "skips the tag guard and the
  publish step"; `:56` (step 6) says "nothing to undo — a dry run creates no
  tag and no release"; `:89-92` (step 10) then instructs
  `gh release delete v0.0.0-test --yes` and two tag deletions. There is no
  path in the workflow that creates `v0.0.0-test`.

`.github/workflows/release.yml:1-2` — "this workflow has never run. Authored
2026-08-25 and not executable locally" — is contradicted by `:43` of the same
file ("The first run failed here with `dune: command not found`") and by
commits `05fafd3`, `d66087d`, `4470f03`, which are fixes read off real runs.
*(Code comment, not markdown.)*

Verified correct against the workflow and the built binaries: the asset name
`writeonce-0.1.0-linux-amd64.tar.gz`, the tag↔`VERSION` guard, the
`ubuntu-22.04` pin and its glibc reasoning (this machine's binaries need
`GLIBC_2.38`, matching `:8` step 8's "2.38 from this dev machine"), and
`scripts/install-readme.tmpl.md:24-25` — `woc version` prints
`writeonce 0.1.0 linux/amd64` and `wovm --version` prints `wovm 0.1.0`, exactly
as documented.

---

## F. Small factual errors

| Where | Claim | Actual |
| --- | --- | --- |
| `README.md:28` | "~100 KB for the sample programs" | 163–254 KB. Smallest built sample 163,117 B (`fibers`), largest 253,808 B (`site`); bare `wovm` is 161,848 B, so ~160 KB is the floor |
| `README.md:142` | "**Types:** `Int`, `Text`, `Bool`, and user `class` types" | seven builtin scalars (`types.ml:174`): also `Float`, `Bytes`, `Timestamp`, `Id` |
| `README.md:170` | `time` → "`sleep`, `now`, `local`, `iso`" | also `ticks` (µs monotonic, builtin 84 — iteration 22's one runtime addition) |
| `README.md:172` | `net` → "TCP `listen`/`accept`/`read`/`write`/`close` (host + port)" | also `read_dl`, `accept_dl`, `write_dl`, `listen_unix`, `peer` (ids 91–95, iteration 35) |
| `README.md:344` | "`net` is TCP host+port only" | `net.listen_unix` binds a unix socket (builtin 94) |
| `README.md:272,276-277` | `[deps]` key `niceserve`, then "`use niceframework`" | the `[deps]` KEY *is* the module name — `docs/examples/web-app/wo.toml:19-20` keys it `serve` and `main.wo:9` says `use serve`. The example as written would not compile |
| `README.md:295` vs `:298-320` | "**Two** complete sample programs" | three bullets follow; `:322` "Read **either** program's `main.wo`" compounds it. There are 13 samples, 8 of them gated |
| `docs/guides/language-surface.md:36` | "**Keywords (35)**" | 37. The list printed immediately after is complete and correct against `lexer.ml:147-184` — only the count is wrong |
| `docs/00-principles.md:104-105` | capabilities are "(`fs`, `proc`, `net`, `time`, `json`)" | six modules — `env` missing (`types.ml:206`) |
| `docs/superpowers/plans/2026-08-18-inferred-gc-mark-sweep.md:153-154` | `[x]` "Add a `docs/examples/gc-cycle` acceptance script + `just gc-cycle` recipe" / `[x]` "Verify: `just gc-cycle` green" | **no `gc-cycle` recipe exists** and there is no `scripts/gc-cycle-accept.sh`. `docs/examples/gc-cycle/` has sources, a `target/` and a "Run status" section (`README.md:186`) but no gate. Two checked boxes for work that did not land |

Forward references that are correctly labelled and are *not* findings:
`just chat` (iteration 24, T9 — pending in the marker), `just oop-parity`
(deferred by explicit decision, recorded at `compiler/README.md:5`), and
`docs/examples/employee-list/`, whose banner honestly says it does not compile
(confirmed: `woc` exits 2 on `[connect.employee]`, a section for the `hold/`
iteration-20 feature).

---

## What to fix first

1. **`README.md`** — it is the writeonce.de landing content
   (`docs/08-project-structure.md:28`), so A1–A3, F's README rows and the
   `use niceframework` example are the highest-value corrections in the repo.
2. **The two example status banners** (A4, A5) — one line each, and they
   currently tell a visitor that the repo's two flagship gates don't build.
3. **`runtime/README.md`** (A6) — the largest single body of stale prose. It
   wants splitting: `wo-rt.c` is a historical reference card, `runtime/src/` is
   the shipped VM, and one page is trying to be both.
4. **`docs/00-code-review.md`** and **`docs/00-link-audit.md`** (B1, B2) — both
   are dated verifications whose value depends on being re-run. Either re-run
   them or banner them as of-date.
5. **`docs/plan/oop-vm/01-error-catalog.md`** (B3) — it is cited as normative by
   two other docs; ten missing codes including the query surface's only one.
6. **`docs/guides/language-surface.md:175`** (C1) — a single false clause on
   an otherwise excellent page.
7. **`docs/00-dependency-graph.md`** first graph (C3) and the board's NEXT PLAN
   narrative (C2) — both trail their own companion tables.

---

## What was fixed

All on branch `docs-truth-audit-fixes`, 2026-08-26. Docs only — no code changed,
so no gate output changed.

| Finding | Disposition |
| --- | --- |
| **A1** README roadmap listed shipped concurrency | Concurrency entry removed; `spawn`/`send`/`call`/`receive` documented under "Language at a glance" as shipped. The `service`-blocks entry stayed but now says what you write *instead* today. Group-by stayed — it was the one correct entry. |
| **A2** "does not serve HTTP, WebSockets, or a UI" | Rewritten: both work, as `.wo` libraries consumed through `[deps]`, never as runtime features — which is the real (and more interesting) claim. TLS-by-proxy stated. |
| **A3** "no package manager" | Now "no package **registry** — dependencies are exact-rev git URLs and nothing else", which is true and is the distinction the doc meant. |
| **A4** `employee` README "does not compile" | Banner flipped to shipped, `just employee` named as the gate, with the one genuinely-ahead clause (`group … by … into`) called out rather than left to surprise a reader. |
| **A5** `log-watcher` README "design artifact" | Banner flipped to shipped with iteration 7's actual acceptance evidence; the "five stdlib modules" list corrected to six. |
| **A6** `runtime/README.md` a generation stale | **Restructured, not patched.** Leads with `wovm`; `wo-rt.c` demoted to a marked "Historical" section that keeps its measured numbers as the prototype record they are. Fixed: the two nonexistent recipes, `crates/rt`, `prototypes/wo-db`, the `@gc` refcount/Bacon–Rajan description (now inferred mark-sweep), `DB_STUB`, the builtin list, "13 suites" → 18, four missing source files, and the phase E/F contradiction. Three broken links went with it. |
| **B1** `00-code-review.md` decayed | History kept intact with a pointer at the top; a **Re-verification 2026-08-26** section added listing the eight overtaken rows against source, the ~20 that still hold, and the two new gaps (now iteration 38). "No supervision/actor death" is marked *partly* overtaken — death landed, supervision did not. |
| **B2** `00-link-audit.md` stale | Re-run and rewritten. The 48 dead-era exploration links are **resolved by de-linking, not re-pointing** — their prose names the retired plan by number, so re-targeting would have made each sentence lie. A successor map was added to `plan/discarded.md`, which is what that report's own "Still open" note asked for. Three more fixable breaks fixed. 77 → 23. |
| **B3** ten codes missing from the error catalog | Added with definitions read from source: WO-E003, E108, E109, E219–E223, E226, E250. Header's "as of plan 3" scope line corrected. The Completeness method section now records *why* the sweep rotted — codes are built as `<stage>_prefix ^ "NN"`, so grepping for the literal `WO-E250` finds only a comment. **The `WO-W201` half of this finding was wrong:** the catalog already marked it *(retired, iteration 7b)* with "*(no longer emitted)*". The doc was right; the audit misread its own grep. |
| **C1** `language-surface.md` claimed group-by works | Corrected in three places: the clause is marked in the grammar block, the "present today" list drops it, and the page's "every form was compiled and run" promise now names the exception. Keyword count 35 → 37. |
| **C2** board narrative trailed its own tables | ▶ NEXT PLAN rewritten: the live slice is 24 (absorbing 31 + 34), not "31 next". Rows for 24, 31 and 34 updated — 31's remaining surface is cited as the *reserved holes at ids 89/90*, which is machine-checkable. A standup entry for the 2026-08-25 packaging/release track was added; it had none. |
| **C3** dependency graph a generation behind | Graph 1 rebuilt: 17 → done, 18/20/21 → held, the pre-renumber ids 10/12/13/14 replaced by stories 25/26/29/28, nodes added for 19/24/30/31/32/33/34/35/36/37/38, and the parked drain reduced to what is actually left (`pub(read)`/`using`/`#if` all shipped). Node/edge references validated. |
| **D1** `08-project-structure.md` "canonical map" | Fixed: the nonexistent `compiler/plan/`, the corpus's nine directories (four with fixtures, five reserved and empty, with counts), all 14 scripts, the `docs/` subtree, and the four missing root entries (`bench/`, `dist/`, `.github/`, `.claude/`). The sample-acceptance list now names all eight gates. |
| **D2** `compiler/README.md` stage banner + CLI | Banner replaced with the eight iterations the front end has taken since plan 3. The CLI list gained `woc <dir>` (the primary mode), `version`, `--update-deps`, `--dump-gc` and `-D`. `crates/rt/src/lib.rs::discover` reference dropped. `gcinfer` added to the module list. |
| **D3** `runtime/src/CODE-LOGIC.md` file table | `park.c/.h` and `crypto.c/.h` added, dated so the gap is visible rather than papered over. |
| **E** `releasing.md` unfollowable steps | The `--draft` step and the phantom rehearsal cleanup deleted, remaining steps renumbered, and a paragraph added explaining why neither exists (plus how to opt into a draft if you want one). |
| **F** small factual errors | All corrected: binary size ~100 KB → 160–260 KB (measured), the scalar list, `time.ticks`, the five missing `net` members, the `fs` read-and-append limit, the `[deps]` key/`use` mismatch (the example would not have compiled), "two samples" → 13 with 8 gated, the six-module count in `00-principles.md`, and the `just gc-cycle` recipe that two checked boxes claimed. |
| **Later findings** | `00-story.md` gained the missing iteration 36 row; `docs/examples/db-actor/` gained the README it never had. |

### Left deliberately unfixed

- **23 broken links in `.dev/`** — vendored plugin-skill copies and reference
  study trees. `.dev/` is gitignored (`git ls-files .dev` returns one path), so
  these are per-developer notes. Fixing them means re-vendoring the skills with
  their `references/` subdirectories.
- **The `just gc-cycle` gate itself.** The false checkbox is now disclosed in
  both the plan and the sample's README, but wiring the acceptance script is
  work, not documentation, and belongs to whoever picks up that loose end.
- **`tests/corpus/`'s five empty directories.** Documented as reserved with the
  plan each was to be filled by; deleting or filling them is a test decision.
- **Two stale references in code comments**, recorded here rather than edited
  because this pass was scoped to markdown: `compiler/bin/main.ml:16-19` and
  `compiler/src/parser.ml:202` both still cite `crates/rt/src/*.rs`, removed
  2026-08-18; and `.github/workflows/release.yml:1-2` says "this workflow has
  never run" while line 43 of the same file reports what its first run failed
  with.

---

## Structural change 2026-08-26 — status folders removed

Directive from the developer, applied after the fixes above: **`docs/` no longer
uses directories to encode status.** The four story subfolders
(`done/`, `refine/`, `hold/`, `in-progress/`) and top-level `docs/in-progress/`
are gone. All 34 story iterations sit flat in
`docs/stories/language-runtime-database/`, the slice marker sits flat in `docs/`
as `active-slice-<date>-<topic>.md`, and each file's `status:` frontmatter key is
the single place its state is recorded.

This reverses the 2026-08-20/21 convention ("the folder move IS the status
change"). The reason it is a good trade is visible in this repo's own history:
under the old scheme a status change relocated the file, which invalidated every
relative link in and to it — section A of the 2026-08-20 link audit was nine
instances of exactly that, and B2 above found two more that had accumulated
since. A status change is now a one-line edit that cannot break a link.

What the move required, all verified with `just linkcheck` (23 broken, all in
`.dev/`, unchanged from before the move):

- 34 files relocated with `git mv` so history follows them.
- **252 relative links recomputed in 70 files** — not by string substitution but
  by resolving each link to an absolute path from its *old* location, remapping
  through the move table, and re-deriving it relative to the file's *new*
  location. String surgery would have mangled the `../` depth changes on the
  moved files themselves.
- Link *text* and backticked paths that named a status folder stripped
  separately — a correct target under stale display text is still a lie.
- Convention prose rewritten where it taught the old rule:
  `stories/00-status.md`'s header, `stories/board-views.md` (including its Kanban
  caveat, which described status changes as folder moves), and
  `08-project-structure.md`'s map plus a new naming-convention entry.
- Phrases of the form "moves to `done/`" rewritten as "sets `status: done`" in
  the live docs — including the four open checkboxes in the active plan
  `2026-08-23-chat-ws-lifecycle.md`, which would otherwise have instructed a
  future session to recreate the folders.

Two things surfaced that the move made visible rather than caused:

1. **A frontmatter collision, caught and fixed.** Giving the marker doc
   `iteration: "24"` would have put two files in the repo claiming to be
   iteration 24 with contradicting `status:` values. The marker is a progress
   log, not a status carrier, so it takes `slice: "24"` and points at the story
   that owns the status.
2. **Story 24's frontmatter said `refine` while the board said 🔄 live.** Under
   the old scheme that drift was cheap to leave; under this one frontmatter *is*
   the answer, so it is now `status: in-progress`. Iterations 31 and 34 keep
   `refine` — they are absorbed into 24 but their own closeout is still pending,
   which is what 24's T10 exists to do.

Dated records were deliberately left naming the old paths: the findings sections
of this document (which declare themselves a pre-fix snapshot), the history
section of `00-link-audit.md` (which says every path in it is as it was on that
date), and the "Files:" lists of closed plans. Rewriting those would destroy the
record of what was true when each was written.

# `.claude/agents` — project agents for Claude Code

Committed, shared with the team (unlike `.dev/`, which is developer-local).
One file per agent: YAML frontmatter (`name`, `description` = when the main
thread should delegate, `tools`), then the system prompt. Keep each prompt
to doctrine + file map + gates + report format — the agent reads code for
the rest.

## Roster

| Agent | Role | Reads | Gates |
| --- | --- | --- | --- |
| `codd` | the embedded DB end to end: engine under `database/src` (WAL, group commit, checkpoint, keys-resident, migrations), DB seams in `runtime/src` (`.wob` v8 table bit, no-`WO_DATA`/`WO_EPHEMERAL` refusals), `@table`/query surface in `compiler/src` | `database/src/CODE-LOGIC.md`, `docs/plan/oop-vm/04-db-binding.md`, query spec `2026-08-15-table-relations-query-design.md`, `.dev/reference/{postgresql,dotnet-runtime}` | none run directly — brainstorms, owns contracts, reviews, names the checks; `codd-cyril` runs the ladder |
| `codd-shoney` | the developer's proxy for database design: brainstorms a `refine` databasev2 iteration to `ready` (forks enumerated, options grounded in code + references, KISS pick with reason, recorded in Info) and reviews `review_pending` forks — approve / amend / reject with evidence, clears or reopens the flag; docs-only, story decision sections | `codd.md`, the story + spec/plan, `.dev/reference/*`, `.dev/zack/*.md`, `.dev/skills/superpowers/brainstorming.md` | none (asks cyril for counts) |
| `codd-zack` | implementer for ONE `ready` database iteration: task list → failing test → code → unit + corpus gates, with a resume-safe ledger in `.dev/zack/<track>-<n>.md`, one local commit per green task (`type(db2-n): …`, bullets, ≤25 lines, on `dev`, never push); no example gates, no story/board/README edits — codd closes from the ledger | `.claude/agents/codd.md`, the story + its plan/spec, the ledger | `make -C runtime test`, `just woc-test` when compiler touched (unit level only) |
| `codd-pm` | project manager for the database tracks: reconciles story frontmatter, Progress tables, acceptance criteria, dependency graph §8, status board (standup entry, In-progress, Active slice, NEXT PLAN), discarded.md and story FORMAT against code, git log and zack's ledgers; surfaces forks, proposes cherry-picks; docs-only commits | `.claude/agents/codd.md`, code + `git log`, `.dev/zack/*.md`, the stories/board/graph | `just linkcheck` (read-only verification otherwise) |
| `codd-cyril` | test + benchmark engineer for the database tracks: corpus fixtures, `scripts/*-accept.sh` for database programs, `db-bench.py` legs + `bench/baseline.json`, crash/oracle batteries, sanitizer campaigns, example README run instructions; runs the gate ladder, classifies every red, hands failing checks to zack and bugs to pm; test/perf commits | `.claude/agents/codd.md`, zack's ledger, `docs/plan/perf-targets.md` | the whole ladder: `make -C runtime test` → `just woc-test` → `just oop-e2e` → `just residency` → `employee-accept.sh` → `just db-actor` → `just db-bench-quick` → consumers (`chat`, `wmux`, `web-app`, `site`) |
| `fielding` | architect + reviewer for porch (the .wo web framework): locks forks for porch 2–9, owns the README status ledger and specs, reviews .wo diffs against the language limits, names checks/tasks | `docs/examples/porch`, `docs/stories/porch`, `.dev/reference/{fiber,mcp-python-sdk,go}` | none run directly |
| `fielding-zack` | implementer for ONE ready porch iteration, phase by phase, ledger `.dev/zack/porch-<n>.md`, one commit per green task (`feat(porch<n>-slug)`) | `fielding.md`, the story + spec/plan | framework + consumer build, `just oop-e2e` when a fixture is added |
| `fielding-cyril` | test engineer for porch: `web-app`/`site`/`chat`/`deps` gate matrices, corpus fixtures, consumer README commands; failing-first rows, red classification | `fielding.md`, zack's ledger | `just woc-test` → `just oop-e2e` → `just deps-accept` → `just web-app` → `just chat` → `just site` |
| `fielding-pm` | PM for porch: story axes, phase tables, README status ledger, graph §7 P-nodes, board; format pass; docs-only commits | `fielding.md`, code + `git log`, ledgers | `just linkcheck` |
| `ada` | architect + reviewer for jarvis (the AI assistant, a porch app): story 1–3 forks, the LLM adapter boundary, stub-server spec; design-only until porch completes | `docs/stories/jarvis`, `.dev/reference/{mcp-python-sdk,llama-cpp}` | none run directly |
| `ada-zack` | implementer for ONE ready jarvis iteration against ada-cyril's stub LLM; refuses phases whose porch dependency is unbuilt; ledger `.dev/zack/jarvis-<n>.md`; commits `feat(jarvis<n>-slug)` | `ada.md`, the story | app build + scripted request vs stub, `just oop-e2e` |
| `ada-cyril` | test engineer for jarvis: the local stub LLM server, `scripts/jarvis-accept.sh` + `just jarvis` (prompt → stream → durable history → restart; disconnect, slow tokens, missing key), no network ever | `ada.md`, zack's ledger | `just woc-test` → `just oop-e2e` → `just web-app` → `just jarvis` |
| `ada-pm` | PM for jarvis: story axes, phase tables, Dependencies re-verified against porch frontmatter, graph §7 J-nodes, board; docs-only commits | `ada.md`, porch stories, ledgers | `just linkcheck` |
| `lintor` | Linux kernel expert; syscall semantics, uapi layouts, kernel floors; audits `park.c`/`sysio.c`/`main.c`; writes primitive cards | `.dev/reference/linux` (v7.0), `docs/plan/exploration/linux/` | `just fibers` (both `WO_IO` backends), `just subprocess`, `just wmux` |

## Families

Three tracks share one four-role pattern, so a prompt learned once works everywhere:
`<architect>` brainstorms, locks forks, owns contracts, reviews, names checks and tasks;
`<architect>-zack` implements ONE ready iteration with a resume-safe ledger under
`.dev/zack/` and one commit per green task; `<architect>-cyril` owns every test above
the unit level and runs the gate ladder; `<architect>-pm` keeps stories, board, graph
and story format truthful (`model: sonnet` by default — reconciliation work, not
design). Role files read their architect file first, so doctrine
lives in one place per track: `codd` (database), `fielding` (porch), `ada` (jarvis).
A fifth, optional role `<architect>-shoney` is the developer's proxy: brainstorms `refine`
stories to `ready` and reviews `review_pending` forks (only it and the developer clear that
key). Exists for databasev2 today. `lintor` is a cross-track consultant.

## Proposed — not yet written

Each line is one agent; the cut follows the repo's own seams (tracks in
`docs/stories/`, source folders, `.dev/reference/` study trees). Add one
only when a task keeps landing in that seam; a prompt nobody delegates to
is dead weight.

| Agent | Seam | Reads | Gates | Why a separate agent |
| --- | --- | --- | --- | --- |
| `runtime-developer` | VM core: `vm.c`, `gc.c`, `borrow.c`, `cont.c`, `obj.c`, `loader.c`; fibers, shard actors, mailboxes, park plane | `runtime/src/CODE-LOGIC.md`, `docs/plan/exploration/fibers/`, `.dev/reference/go/src/runtime/` (netpoll, proc) | `make -C runtime test` (ASan + TSan), `just fibers`, `just chat`, `just wovm-test` | Largest C surface; doctrine (ownership moves, no locks, drain guarantee) differs from the DB engine's |
| `compiler-developer` | OCaml `woc`: `compiler/src/{lexer,parser,types,owner,gcinfer,emit,diag}.ml`, golden fixtures | `compiler/src/CODE-LOGIC.md`, `docs/plan/oop-vm/`, `.dev/reference/llvm-project/clang/lib/{Lex,Parse,Sema}` for layering + diagnostics | `just woc-build`, `just woc-test` (golden + `test_diag`) | Different language, different test shape (golden files, `WO-E` diagnostics), open bugs like self-field concat-assign |
| `porch-developer` | (realised as the `fielding` family) the web framework in `.wo`: `use porch`, iterations porch 1–9 (cookies, sessions, CSRF, routing, streaming, SSE, static, replay) | `docs/stories/porch/`, `docs/examples/{porch,web-app,site}`, `.dev/reference/mcp-python-sdk` for streamable HTTP | `just web-app`, `just site`, `just deps-accept` | Writes writeonce, not C; must know builtin ids and language limits (no function values, no reflection) |
| `wmux-developer` | the terminal multiplexer: `docs/examples/wmux`, wmux iterations 1–23, WAL-persisted Window/Sess/Vte actors | `docs/stories/wmux/`, `.dev/reference/{tmux,alacritty,zen-browser}` parity studies | `just wmux` (real PTY harness) | Parity-driven against tmux; PTY/termios questions go to `lintor`, escape-sequence semantics to alacritty's `vte` |
| `crypto-reviewer` | adversarial review only of `tls.c`, `crypto.c`: constant-time paths, RFC 8448 vectors, X.509 chain/hostname, RSA-PSS / ECDSA nonce | `runtime/test/*_vectors.h`, RFCs 8446/8448/6979/6125, `.dev/reference/cryptography-06-00030.pdf` | `make -C runtime test` (`test_tls`, `test_crypto`), `just tls`, `just tls-server` | Hand-rolled crypto needs a reviewer that never implements; read-only tools |
| `story-steward` | (database tracks now covered by `codd-pm`; this row is the whole-project version) docs discipline: story frontmatter (`iteration`/`status`/`readiness`/`track`), `docs/stories/00-status.md` standup entry, dependency graph, commit-history table, `CODE-LOGIC.md` beside code, `discarded.md` | `docs/stories/`, `docs/00-*.md`, `.dev/reference/README.md` | `just linkcheck` | Every landed change must update the board the same commit; a dedicated agent keeps iteration numbers unique and status out of folder names |
| `postgres-expert` | sibling of `lintor` for `databasev2`: WAL, smgr/md, bufmgr, checkpointer, fsync policy | `.dev/reference/postgresql/src/backend/{access/transam,storage}`, `docs/plan/exploration/postgresql/` | none — consultant | Same shape as `lintor`: cite source, never port code (zero-dep doctrine) |
| `gopher` | sibling of `lintor` for the scheduler: Go's netpoll, `proc.go`, work stealing, `sysmon` | `.dev/reference/go/src/runtime/`, `.dev/reference/Scalable_work_stealing.pdf`, `docs/plan/exploration/assembly/` | none — consultant | writeonce mirrors Go's file-per-flavour runtime layout; asm policy already cites this tree |

Order to add, if all are wanted: `runtime-developer` and `compiler-developer`
first (most code lands there), then `porch-developer` (current track), then
the rest as their tracks reopen.

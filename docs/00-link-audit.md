# Markdown link audit — re-run 2026-08-26

Scope: every repo-authored `*.md`. `.git`, `target`, `dist`, `node_modules`,
`_build` and — since 2026-08-26 — `.dev/` and `.superpowers/` are excluded; see
the note under the table. External URLs are not fetched (no network
verification).

| | files | relative links | broken paths | bad anchors |
|---|---|---|---|---|
| first scan (2026-08-20) | 207 | 574 | 97 | 0 |
| after section A fixes (2026-08-20) | 206 | 569 | 88* | 0 |
| **re-run 2026-08-26, before fixes** | 235 | 675 | 77 | 0 |
| **re-run 2026-08-26, after fixes** | 237 | 652 | 23 | 0 |
| **after scoping the gate to repo-authored docs** | 149 | 656 | **0** | **0** |

\* The 2026-08-20 report's prose said 88 twice while its own sections B–F summed
to 77. The 77 was right; the 88 was an arithmetic slip, corrected here.

**The gate is now clean: 0 broken, 0 bad anchors.**

The last 23 were all in `.dev/` — vendored plugin-skill copies and cloned
reference projects, neither of which this repo authors. `scripts/linkcheck.py`
now skips `.dev/` and `.superpowers/` alongside `.git`/`target`/`dist`. That was
forced by adding gofiber/fiber as a reference (2026-08-26): its own docs are
Docusaurus pages whose links resolve at site-build time, not on disk, so the
clone alone contributed 21 broken paths and 39 bad anchors. A gate that reports
the same dozens of failures forever is a gate nobody reads. Everything the repo
actually ships — `docs/`, `compiler/`, `runtime/`, `database/`, `tests/`,
`bench/`, `scripts/`, the root README — is still scanned, and is clean.

Re-check with `just linkcheck`.

Tool: `scripts/linkcheck.py` — walks the tree, strips fenced/inline code,
extracts inline links and reference definitions, resolves each relative target,
and validates `#fragment` against GitHub-style heading slugs of the target file.

---

## What the 2026-08-26 re-run changed

### 1. The dead-era exploration links — RESOLVED (48 links, 15 files)

Sections B and C of the 2026-08-20 report left a decision open: the studies under
`docs/plan/exploration/` cite the old flat `docs/plan/NN-*.md` numbering and the
`docs/runtime/database/` tree, both removed with the Rust track on 2026-08-18,
and no successor map existed. That decision is now made.

**De-linked, not re-pointed.** The link *text* in these studies names the retired
plan by number — `[plan 09a]`, `[plan 11]`, ``[`12-engine-disk-cutover.md`]`` —
so aiming those at a story would have made each sentence assert something false
about a document that never said it. The targets were stripped and the text kept
as plain code spans. The studies still read correctly as the dated records they
are, and they no longer claim a file exists.

The successor map lives in
[`plan/discarded.md`](plan/discarded.md#successor-map-for-the-removed-rust-era-plan-paths)
— one row per retired path, naming what carries that work now (or stating
plainly that nothing does, as with `12-engine-disk-cutover.md` and
`08-sendfile-static-assets.md`). That table is what the 2026-08-20 report's
"Still open" note asked for.

Files touched: `assembly/{00-overview,02-writeonce-stance}.md`,
`c-runtime/{00-plan,01-architecture,02-single-binary}.md`,
`linux/{01-epoll,02-eventfd,03-timerfd,04-signalfd,05-inotify,06-sendfile,07-io_uring,08-mmap,11-memfd_create,12-pwrite-fsync}.md`.

### 2. `runtime/README.md` — RESOLVED (3 links)

`prototypes/wo-db/`, `docs/runtime/database/03-inmemory-engine.md` and
`docs/plan/09-concurrency-scaleout.md` all went when that README was restructured
to lead with `wovm` and demote `wo-rt.c` to a clearly-marked historical section.
It also carried two recipes that do not exist (`just rt-c-demo`,
`just rt-c-bench`) — not a link problem, fixed in the same pass. See
[`00-doc-audit.md`](00-doc-audit.md) §A6.

### 3. Two breaks the 2026-08-20 report did not have — RESOLVED

Both were caused by story files moving between status folders after that report:

| Source | Was | Now |
|---|---|---|
| `docs/examples/employee-list/README.md:5,6` | `…/refine/20-cross-program-tables.md`, `…/refine/21-keypair-attach-auth.md` | `…/hold/…` (both stories moved to `hold/` 2026-08-21) |
| `docs/stories/…/hold/26-blue-green-deploy.md:9` | `00-story.md` | `../00-story.md` (the sibling stopped being a sibling when 26 moved into `hold/`) |

This is the recurring shape: **a story folder move breaks every relative link
in and to that file.** Section A of the 2026-08-20 report was nine instances of
it; these are two more. Worth a check in whatever moves a story.

### 4. Stale paths inside the report itself — RESOLVED

The 2026-08-20 repair table cited `docs/00-status.md` (now
`docs/stories/00-status.md`) and `refine/{08,11,19,20,21}` (now under `done/` and
`hold/`). That table has been retired into the history section below rather than
carried forward with paths that no longer resolve.

### 5. Four links the report listed as open had already been fixed

`docs/00-principles.md:57,77,78,87` resolved before this re-run — including the
`examples/blog/README.md` reference that section D called a never-created file.
Section D's other entries stand.

---

## Out of gate scope — `.dev/` (was 23 links, now unscanned)

Recorded so the knowledge is not lost, but no longer reported by
`just linkcheck`. Not ours to fix, unchanged in character from the 2026-08-20
report's section F.

- **`.dev/skills/` (15 links)** — flattened copies of plugin skills. The
  originals ship as directories with sibling `references/` files; flattening
  dropped them. `context-mode.md:297-300`, `subagent-driven-development.md` (5),
  `writing-skills.md` (3), `requesting-code-review.md` (2),
  `test-driven-development.md:206`. Leave as-is, or re-vendor the skills with
  their subdirectories.
- **`.dev/reference/` (8 links)** — `README.md` (7) points at the removed
  `docs/plan/{linux,assembly}/` and `15-mcp-streamable-http.md`, the absent
  `exploration/colibri/`, and `prototypes/llama-moe-stream`;
  `rest/README.md:76` points at `docs/examples/blog/`, which never existed.
  `.dev/` is gitignored (`git ls-files .dev` returns only `.dev/README.md`), so
  these are per-developer notes, not repo content.

---

## History — the 2026-08-20 first pass

Kept for the record; every path below is as it was on that date.

### A. Regressions from the in-flight renumber — FIXED 2026-08-20

Nine links broke because files moved in the working tree; each had a known
successor. Sources: `docs/00-status.md:167,187`,
`docs/stories/language-runtime-database/00-story.md:60,69`, the `25`/`26` story
pair (which used `../00-story.md` while `00-story.md` was a sibling — the
`refine/`-relative form pasted into files one level up), `refine/08-shard-actor-runtime.md:98,100`,
and `refine/20-cross-program-tables.md:143`. Link labels were renumbered with
their targets, since the old IDs contradicted the new paths: `9e`→`22` and
`9f`→`23`.

### Structural problems found alongside the links

1. **Iteration 19 was double-booked — RESOLVED.** `refine/19-chat-websocket-workload.md`
   and `refine/24-chat-websocket-workload.md` were the same document while
   `19-missing-scalar-types.md` also claimed 19. `00-story.md`'s mapping line
   made **24** canonical, so the 19 copy was deleted after repointing
   `refine/11-fibers.md:13` at 24.
2. **`08-shard-actor-runtime.md` existed twice — RESOLVED.** 58 lines at the
   stories root vs 110 in `refine/`. The `refine/` copy superseded it outright
   (the root copy still required `@gc`, retired by 7b, and cited
   `runtime/wo-rt.c`, removed with the Rust runtime). Root copy deleted.
3. **Unresolved merge-conflict markers were committed** into
   `refine/20-cross-program-tables.md:139-145`, from a rename-conflicted merge —
   which is what produced that file's broken `09d` link. Resolved in favour of
   HEAD. `grep` confirmed no other conflict markers under `docs/`.
4. **A status disagreement, not a link problem:** `docs/00-status.md:171` showed
   iteration 8 as ⬜ while `00-story.md:68` recorded arc stages 1+2 as landed.
   Both now read landed.

# Markdown link audit — 2026-08-20

Scope: every `*.md` in the repo (`.git` excluded).
External URLs were not fetched (no network verification performed).

| | files | relative links | broken paths | bad anchors |
|---|---|---|---|---|
| first scan | 207 | 574 | 97 | 0 |
| after section A fixes | 206 | 569 | **88** | 0 |

Section A is repaired and verified. Sections B–F are pre-existing rot and
still open — every one of the remaining 88 lives there.

Re-check with `just linkcheck`.

Tool: `linkcheck.py` — walks the tree, strips fenced/inline code, extracts inline
links and reference definitions, resolves each relative target, and validates
`#fragment` against GitHub-style heading slugs of the target file.

---

## A. Regressions from the in-flight renumber — FIXED 2026-08-20

All nine broke because files moved in the working tree; each had a known
successor. Repaired:

| Source | Was | Now |
|---|---|---|
| `docs/00-status.md:167` | `stories/language-runtime-database/05-language-surface.md` | `…/done/05-language-surface.md` |
| `docs/00-status.md:187` | `stories/language-runtime-database/18-memory-db-features.md` | `…/hold/18-memory-db-features.md` |
| `docs/stories/language-runtime-database/00-story.md:60` | `05-language-surface.md` | `done/05-language-surface.md` |
| `docs/stories/language-runtime-database/00-story.md:69` | `18-memory-db-features.md` | `hold/18-memory-db-features.md` |
| `docs/stories/language-runtime-database/25-http-service.md:4` | `../00-story.md` | `00-story.md` |
| `docs/stories/language-runtime-database/26-blue-green-deploy.md:4` | `../00-story.md` | `00-story.md` |
| `.../refine/08-shard-actor-runtime.md:98` | `../hold/09e-durability-throughput-scale.md` | `22-durability-throughput-scale.md` |
| `.../refine/08-shard-actor-runtime.md:100` | `09f-io-uring-commit.md` | `23-io-uring-commit.md` |
| `.../refine/20-cross-program-tables.md:143` | `../hold/09d-keypair-attach-auth.md` | `21-keypair-attach-auth.md` |

The `25`/`26` pair used `../00-story.md` while `00-story.md` is a sibling — the
`refine/`-relative form pasted into files one level up.

Link labels were renumbered with their targets, since the old IDs contradicted
the new paths: `9e`→`22` and `9f`→`23` in `refine/08` (both the "Gated by the
benchmark" note and settled decision 4, "Order: 22 → the 8+11 arc → 23").

## B. Dead era: the old flat `docs/plan/NN-*.md` numbering (48 links)

`docs/plan/` now holds only `compiler/`, `exploration/`, `oop-vm/`,
`discarded.md`, `learnings.md`. Every flat-numbered plan doc is gone, and no
successor path was recorded. Missing targets, by inbound count:

- `09-concurrency-scaleout.md` — 12
- `11-wal-and-recovery.md` — 9
- `12-engine-disk-cutover.md` — 8
- `10-storage-foundations.md` — 8
- `done/02-event-loop-epoll.md` — 4
- `13-class-model-live-pricing.md` — 3
- `07-inotify-content-watcher.md` — 3
- `08-sendfile-static-assets.md` — 2
- `15-mcp-streamable-http.md`, `16-postgres-mirror.md`,
  `done/03-hand-rolled-http.md`, `done/04-cutover-remove-tokio-axum.md` — 1 each

Inbound from: all of `docs/plan/exploration/{linux,postgresql,c-runtime,assembly}/`,
plus `docs/00-principles.md:57,77,78`, `runtime/README.md:47`,
`.dev/reference/README.md:55,56,58`.

**Decision needed** — these exploration docs still cite a plan structure that no
longer exists. Either map each to its story successor
(e.g. concurrency-scaleout → `stories/.../refine/08-shard-actor-runtime.md`,
wal/storage → `refine/22-durability-throughput-scale.md`,
io_uring → `refine/23-io-uring-commit.md`) or strip the links and keep prose.

## C. Dead era: the `docs/runtime/database/` tree (7 links)

`docs/runtime/` does not exist. Missing targets:

- `03-inmemory-engine.md` — 5 (incl. one `#recovery` anchor)
- `02-wo-language.md` — 2 (incl. one `#concurrency-model` anchor)
- `07-wo-seg-migration.md` — 1

Inbound from `docs/plan/exploration/linux/{07-io_uring,08-mmap,11-memfd_create}.md`,
`docs/plan/exploration/{assembly/02-writeonce-stance,c-runtime/02-single-binary}.md`,
`runtime/README.md:43`, `.dev/reference/README.md:31`.

## D. Never-created / removed siblings (5 links)

| Source | Target | Note |
|---|---|---|
| `docs/plan/exploration/linux/06-sendfile.md:10` | `./07-splice.md` | slot 07 is `07-io_uring.md`; no splice doc was written |
| `docs/plan/exploration/assembly/00-overview.md:19` | `../../../.dev/reference/go/src/runtime/atomic_amd64.s` | wrong depth **and** file absent from the vendored Go tree |
| `docs/00-principles.md:87` | `examples/blog/README.md` | `docs/examples/blog/` never existed |
| `.dev/reference/rest/README.md:76` | `../../docs/examples/blog/README.md` | same missing example |
| `.dev/reference/README.md:41,59` | `../docs/plan/exploration/colibri/00-colibri-and-mixtral.md` | `exploration/colibri/` absent (2 links) |

## E. `prototypes/` tree gone (4 links)

`prototypes/` is not in the repo. Referenced as `prototypes/wo-db/` from
`docs/plan/exploration/c-runtime/00-plan.md:88`, `02-single-binary.md:83`,
`runtime/README.md:5`, and `prototypes/llama-moe-stream` from
`.dev/reference/README.md:59`.

## F. Vendored skill copies — not ours to fix (13 links)

`.dev/skills/` holds flattened copies of plugin skills. The originals ship as
directories with sibling reference files; flattening dropped them.

- `.dev/skills/context-mode/context-mode.md:297-300` → `./references/{patterns-javascript,patterns-python,patterns-shell,anti-patterns}.md`
- `.dev/skills/superpowers/requesting-code-review.md:34,95` → `code-reviewer.md`
- `.dev/skills/superpowers/subagent-driven-development.md:232,300,345,400,410` → `implementer-prompt.md`, `task-reviewer-prompt.md`, `re-review-prompt.md` (×2), `../requesting-code-review/code-reviewer.md`
- `.dev/skills/superpowers/test-driven-development.md:206` → `writing-good-tests.md`
- `.dev/skills/superpowers/writing-skills.md:12,587` → `../using-superpowers/references/{codex,gemini}-tools.md`, `testing-skills-with-subagents.md`

Leave as-is, or re-vendor the skills with their `references/` subdirectories.

---

## Structural problems found alongside the links

1. **Iteration 19 was double-booked — RESOLVED.**
   `refine/19-chat-websocket-workload.md` and `refine/24-chat-websocket-workload.md`
   were the same document, differing only in the `# Iteration NN` heading, while
   `19-missing-scalar-types.md` also claimed 19. `00-story.md`'s mapping line
   (`24←19(chat)`) and table row 20 make **24 canonical**, so the 19 copy was
   deleted. `refine/11-fibers.md:13` had been pointing at the 19 copy — repointed
   to 24 first, so the delete broke nothing. Prose in `refine/08` that named
   "iteration 19" for chat now says 24 (4 places).

2. **`08-shard-actor-runtime.md` existed twice — RESOLVED.**
   58 lines at the stories root vs 110 in `refine/`. The `refine/` copy supersedes
   it outright: same acceptance criteria plus the 2026-08-20 settled decisions, the
   inferred-GC restatement (7b retired `@gc`, which the root copy still required),
   and the corrected substrate path (the root copy cited `runtime/wo-rt.c`, removed
   with the Rust runtime). Root copy deleted; the one inbound link,
   `docs/00-status.md:171`, now points at `refine/`. Six other referrers already did.

3. **Unresolved merge-conflict markers were committed** into
   `refine/20-cross-program-tables.md:139-145` — `<<<<<<<< HEAD:… / ======== /
   >>>>>>>> language-surface-strictness:…/hold/09c-cross-program-tables.md`, from a
   rename-conflicted merge. This is what produced that file's broken `09d` link:
   the stale side was still in the file. Resolved in favour of HEAD (the renumbered
   `21` text). `grep` confirms no other conflict markers under `docs/`.

## Still open

- Sections B–F above: 88 broken links, all pre-existing.
- `docs/plan/discarded.md` and `docs/plan/learnings.md` are the only survivors of
  the old flat plan layout, which is why B and C have no successor map. A rename
  table in one of them would let the exploration docs be repaired mechanically
  rather than by guesswork.
- `docs/00-status.md:171` still shows iteration 8 as ⬜ while `00-story.md:68`
  records arc stages 1+2 as landed 2026-08-20. Not a link problem — a status
  disagreement between the two index docs. Left alone.
- `refine/23-io-uring-commit.md:26` still quotes the old order as
  "9e → 8+11 → 9f" in a dated note. No link involved; left as historical record.

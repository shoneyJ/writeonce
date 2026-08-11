# .dev — developer-local links into this project's AI-tooling state

Symlinks to the state that AI tooling reads and writes **for this repo
only** — no global/all-projects state is linked here. Same convention as
`log-watcher/.dev`: **user-specific paths, gitignored** (only this README
is committed). Each developer recreates the links for their own machine
(commands below).

| Link | Points at | What a developer uses it for |
| --- | --- | --- |
| `claude-project/` | `~/.claude/projects/<this-repo, slashes→dashes>` | Claude Code's per-project state: session transcripts (`*.jsonl`) and `memory/` (the persistent memory index + facts). Grep a past session, read/curate what Claude remembers about this repo. |
| `skills/<plugin>/<skill>.md` | Each installed plugin's `skills/*/SKILL.md` (`~/.claude/plugins/cache/…`, version-pinned) plus `~/.claude/skills/*` under `skills/user/` | Read the exact instructions a skill injects (caveman, context-mode, superpowers, …) without digging through the plugin cache. Links break when a plugin updates to a new version — rerun the recreate loop below. |

## reference/ — v1 archive, smoke files, study-tree symlinks

`reference/` (moved here from the repo root, then untracked entirely,
2026-08-08) is **developer-local, gitignored** — disk-only on each
machine; the v1 history remains reachable in git at the pre-move
`reference/` path. It holds:

- `reference/crates/` — the v1 writeonce blog (13 `wo-*` crates, nested
  Cargo workspace, excluded from the root workspace). Port source for the
  dependency-removal plans; do not delete without checking
  `docs/runtime/database/07-wo-seg-migration.md`.
- `reference/rest/` — `.rest` HTTP smoke files for the running runtime.
- `reference/writeonce-api/`, `reference/writeonce-app/` — earlier cuts,
  archived.
- Gitignored, user-specific symlinks to study trees:
  `linux`, `go`, `postgresql`, `mcp-python-sdk`, `colibri`, `llama-cpp`,
  `llvm-project` (recreate per machine; see `.gitignore` for the
  `ln -s` lines).

Study-tree notes:

| Link | Points at | Why it's a reference |
| --- | --- | --- |
| `reference/llvm-project/` | `~/projects/llvm-project` (shallow clone) | Compiler-architecture study for the OCaml `woc` compiler: pass pipelines (`llvm/lib/Passes/`), IR design (`llvm/docs/LangRef.md`), Clang's lexer/parser/sema layering (`clang/lib/{Lex,Parse,Sema}/`), diagnostics machinery (`clang/include/clang/Basic/Diagnostic*.td`). Study-only — writeonce does NOT link against LLVM (zero-dep doctrine; `woc` emits `.wob` bytecode, no LLVM backend). |
| `reference/dotnet-runtime/` | `~/projects/dotnet-runtime` (shallow + **sparse**: only `src/libraries/System.Linq`, 15 MB instead of multi-GB) | Query-surface study for story iteration 9b (`@table` relations + language-integrated query). Read `src/libraries/System.Linq/src/System/Linq/` for the operator set and how each is specified (`Where.cs`, `Select.cs`, `Join.cs`, `GroupBy.cs`, `OrderBy.cs`), and the `*.SpeedOpt.cs` files for how LINQ specializes when the source's shape is known. Study-only, and note the deliberate divergence: LINQ-to-Objects is *runtime* iterator composition over `IEnumerable`, while writeonce has no function values and forbids reflection — so writeonce takes the operator vocabulary and semantics, not the delegate/expression-tree machinery. |

(The former separate `references/` directory was merged into `reference/`
on 2026-08-08 — one home for all study trees.)

Deliberately **not** linked (global, cross-project state): `~/.claude`
(all projects' transcripts, credentials), `~/.config/opencode` and
`~/.local/share/opencode` (global config; sessions for every project;
`auth.json` tokens), `~/.cache/llama.cpp` (shared model weights), the
llama.cpp checkout's `llama-server.log` (lives with that checkout —
`tail -f` it there when running `prototypes/llama-moe-stream/start-local-agents.sh`).

Related, already at the repo root: project-level agent definitions go in
`.opencode/agents/*.md` (opencode) and `.claude/agents/*.md` (Claude Code) —
those are *committed* when they should be shared with the team, unlike these
links.

## Recreate on a new machine

```bash
cd .dev
ln -sfn "$HOME/.claude/projects/$(git rev-parse --show-toplevel | tr / -)" claude-project
git clone --depth 1 https://github.com/llvm/llvm-project.git ~/projects/llvm-project
ln -sfn "$HOME/projects/llvm-project"  reference/llvm-project

# skills/ — one .md link per installed skill (rerun after plugin updates)
jq -r '.plugins | to_entries[] | .value[0].installPath' ~/.claude/plugins/installed_plugins.json | while read -r p; do
  plug=$(basename "$(dirname "$p")")
  for d in "$p"/skills/*/; do
    [ -f "${d}SKILL.md" ] || continue
    mkdir -p "skills/$plug"
    ln -sfn "${d}SKILL.md" "skills/$plug/$(basename "$d").md"
  done
done
for d in ~/.claude/skills/*/; do
  [ -f "${d}SKILL.md" ] || continue
  mkdir -p skills/user
  ln -sfn "${d}SKILL.md" "skills/user/$(basename "$d").md"
done
```

(`claude-project`: Claude Code names the directory after the repo's absolute
path with `/` replaced by `-`, hence the `git rev-parse | tr` trick.)

## Privacy

This link points into **private state**: full conversation transcripts and
Claude's memory for this repo. It is gitignored so none of it can be
committed — keep it that way, and don't bulk-feed the directory to tools
that upload content.

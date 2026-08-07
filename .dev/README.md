# .dev — developer-local links into this project's AI-tooling state

Symlinks to the state that AI tooling reads and writes **for this repo
only** — no global/all-projects state is linked here. Same convention as
`log-watcher/.dev`: **user-specific absolute paths, gitignored** (only this
README is committed). Each developer recreates them for their own machine
(commands below).

| Link | Points at | What a developer uses it for |
| --- | --- | --- |
| `claude-project/` | `~/.claude/projects/<this-repo, slashes→dashes>` | Claude Code's per-project state: session transcripts (`*.jsonl`) and `memory/` (the persistent memory index + facts). Grep a past session, read/curate what Claude remembers about this repo. |

## references/ — related source trees

`references/` holds read-only symlinks to source trees under `~/projects/`
used as study material for the compiler/runtime work (same idea as the
`reference/{linux,go}` links at the repo root):

| Link | Points at | Why it's a reference |
| --- | --- | --- |
| `references/llvm-project/` | `~/projects/llvm-project` (shallow clone) | Compiler-architecture study for the OCaml `woc` compiler: pass pipelines (`llvm/lib/Passes/`), IR design (`llvm/docs/LangRef.md`), Clang's lexer/parser/sema layering (`clang/lib/{Lex,Parse,Sema}/`), diagnostics machinery (`clang/include/clang/Basic/Diagnostic*.td`). Study-only — writeonce does NOT link against LLVM (zero-dep doctrine; `woc` emits `.wob` bytecode, no LLVM backend). |

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
mkdir -p references
git clone --depth 1 https://github.com/llvm/llvm-project.git ~/projects/llvm-project
ln -sfn "$HOME/projects/llvm-project"  references/llvm-project
```

(`claude-project`: Claude Code names the directory after the repo's absolute
path with `/` replaced by `-`, hence the `git rev-parse | tr` trick.)

## Privacy

This link points into **private state**: full conversation transcripts and
Claude's memory for this repo. It is gitignored so none of it can be
committed — keep it that way, and don't bulk-feed the directory to tools
that upload content.

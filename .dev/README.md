# .dev — developer-local links into the agent-tooling filesystem

Symlinks to the state that the AI tooling around this repo reads and writes —
Claude Code, opencode, and the local llama.cpp model server. Same convention
as the `reference/{linux,go,llama-cpp,colibri}` links: **user-specific
absolute paths, gitignored** (only this README is committed). Each developer
recreates them for their own machine (commands below).

| Link | Points at | What a developer uses it for |
| --- | --- | --- |
| `claude-project/` | `~/.claude/projects/<this-repo, slashes→dashes>` | Claude Code's per-project state: session transcripts (`*.jsonl`) and `memory/` (the persistent memory index + facts). Grep a past session, read/curate what Claude remembers about this repo. |
| `claude-home/` | `~/.claude` | Claude Code global home: `settings.json`, `skills/`, `plugins/`, `plans/`, `history.jsonl`, all projects. Where global (cross-repo) agents/skills would live. |
| `opencode-config/` | `~/.config/opencode` | opencode config: `opencode.jsonc` (the `llama.cpp/qwen3-coder` provider), global `agents/`, MCP registrations. Edit here to add agents/tools (see `docs/examples/agent-loop/README.md`). |
| `opencode-data/` | `~/.local/share/opencode` | opencode runtime data: `opencode.db` (sessions), `storage/`, `tool-output/`, `log/`. Debug what an opencode agent actually did. |
| `llama-cache/` | `~/.cache/llama.cpp` | Downloaded GGUF weights (Qwen3-Coder-30B-A3B etc.). What `MODEL_PATH`/`start-local-agents.sh` resolve against. |
| `llama-server.log` | `<llama.cpp checkout>/llama-server.log` | Live log of the background `llama-server` started by `prototypes/llama-moe-stream/start-local-agents.sh` — `tail -f` it to watch prompt processing and tok/s. |

## references/ — related source trees

`references/` holds read-only symlinks to source trees under `~/projects/`
used as study material for the compiler/runtime work (same idea as the
`reference/{linux,go}` links at the repo root):

| Link | Points at | Why it's a reference |
| --- | --- | --- |
| `references/llvm-project/` | `~/projects/llvm-project` (shallow clone) | Compiler-architecture study for the OCaml `woc` compiler: pass pipelines (`llvm/lib/Passes/`), IR design (`llvm/docs/LangRef.md`), Clang's lexer/parser/sema layering (`clang/lib/{Lex,Parse,Sema}/`), diagnostics machinery (`clang/include/clang/Basic/Diagnostic*.td`). Study-only — writeonce does NOT link against LLVM (zero-dep doctrine; `woc` emits `.wob` bytecode, no LLVM backend). |

Related, already at the repo root: project-level agent definitions go in
`.opencode/agents/*.md` (opencode) and `.claude/agents/*.md` (Claude Code) —
those are *committed* when they should be shared with the team, unlike these
links.

## Recreate on a new machine

```bash
cd .dev
ln -sfn "$HOME/.claude/projects/$(git rev-parse --show-toplevel | tr / -)" claude-project
ln -sfn "$HOME/.claude"                claude-home
ln -sfn "$HOME/.config/opencode"       opencode-config
ln -sfn "$HOME/.local/share/opencode"  opencode-data
ln -sfn "$HOME/.cache/llama.cpp"       llama-cache
ln -sfn <your-llama.cpp-checkout>/llama-server.log llama-server.log
mkdir -p references
git clone --depth 1 https://github.com/llvm/llvm-project.git ~/projects/llvm-project
ln -sfn "$HOME/projects/llvm-project"  references/llvm-project
```

(`claude-project`: Claude Code names the directory after the repo's absolute
path with `/` replaced by `-`, hence the `git rev-parse | tr` trick.)

## Privacy

These links point into **private state**: full conversation transcripts
(all projects, under `claude-home`), Claude's memory, and opencode's
`auth.json` (credentials/tokens under `opencode-data`). They are gitignored
so none of it can be committed — keep it that way, and don't bulk-feed these
directories to tools that upload content.

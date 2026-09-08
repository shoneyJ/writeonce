# Status board — what is done, what is next

Edges live in [00-dependency-graph.md](../00-dependency-graph.md) — mermaid
graphs of iteration and feature dependencies; anything with all-green
incoming arrows is startable. This board carries the STATES.

The single place to learn where this project stands. Organised in six buckets:
**stories** (the narrative arc), **in progress**, **done**, **pending**,
**discarded**, **learnings**. The buckets are **sections of this board, not
folders** — a doc stays where it was authored, and only its frontmatter, its
banner and this board change.

**Five tracks** (2026-08-26; wmux and runtime-v2 added 2026-09-01):
[`language-runtime-database/`](language-runtime-database/00-story.md) — the
language and runtime; [`porch/`](porch/00-story.md) — the web framework written
in it; [`databasev2/`](databasev2/00-story.md) — the database beyond RAM;
[`runtime-v2/`](runtime-v2/00-story.md) — the runtime beyond sockets
(processes, terminals, signals); [`wmux/`](wmux/00-story.md) — the terminal
multiplexer, first of the *softwares built with writeonce*. Each
numbers its iterations from 1, so a porch 3 is not a language 3; every non-language
story carries `track:` in frontmatter, and moved ones keep
`was_language_iteration:` so a search for the old number still finds them. Track
folders are fine; **status** folders are not.

**Status lives in frontmatter, nowhere else** (directive 2026-08-26). Every
story iteration file sits flat in its track folder and
carries `status:` in its YAML header; the active slice's marker doc sits flat
in `docs/`. **No directory anywhere encodes state.** This replaces the
2026-08-20/21 convention under which files moved between `done/`, `refine/`,
`hold/` and `in-progress/` — those folders are gone. A status change is now a
one-line edit, not a move, which is the point: the old scheme broke every
relative link in and to a file each time its status changed, and the two link
audits ([`00-link-audit.md`](../00-link-audit.md)) were mostly that.

Every plan and phase doc opens with a `> **Status:**` banner linking back here;
normative contracts (`plan/oop-vm/`), exploration studies, reference docs and
the discarded/learnings registers carry none by design.

Update this board in the same change that finishes work — set the item's
`status:`, record _what actually landed_ here, set the next in-progress item,
and record any rejection in [`discarded.md`](../plan/discarded.md) with its
reason.

**Two frontmatter axes** (2026-08-27), deliberately orthogonal:

- `status` — where the WORK is: `done` · `in-progress` · `pending` · `hold`.
  Rendered here as ✅ · 🔄 · ⬜ · ⏸.
- `readiness` — whether the DESIGN is settled: `ready` (brainstorm complete,
  decisions LOCKED — a spec approved or the forks confirmed) · `refine` (open
  forks; cannot be planned yet).

`status: refine` is retired: it meant both "not started" and "design not
settled", so a held iteration with an approved spec was indistinguishable from
one nobody had thought about. **The startable set is `readiness: ready` and
`status: pending`.**

**Startable-set counts below are STALE** — the paragraph that follows is the
2026-08-27 sweep and predates the entire **wmux** (23 rungs, ~17 done/partial
as of 2026-09-05) and **runtime-v2** (6 iterations, all done) tracks, plus
lang 42. Treat the per-track tables further down as the current truth; this
snapshot is kept for its explanation of the two-axis model, not its numbers.

**As of the 2026-08-27 sweep (STALE — see above):**
[databasev2 4, io_uring group-commit](databasev2/04-io-uring-commit.md) — its
four forks were confirmed settled on 2026-08-20 and nothing has started. Across
47 iterations: 19 done, 5 in-progress, 15 pending, 8 hold; 27 `ready`, 20
`refine`. So of the 15 pending items only **one** can be planned without a
brainstorm first — which is the number this second axis exists to surface, and
it was invisible while one key carried both meanings.

Story frontmatter (`iteration`/`status`/`chain`) is the machine-readable truth
behind this board; live Obsidian Dataview views:
[`board-views.md`](board-views.md) (Kanban = view only, never edits status).

---

## ▶ NEXT PLAN

### Brainstormed 2026-09-06 — the porch track (2–8) and language 41's fix, both to `ready`

**What happened this session (docs only, no code):** the whole
[porch track](porch/00-story.md) 2–8 was brainstormed to `readiness: ready`
against `.dev/reference/fiber`; the track's language bill is three small builtins
(`random_bytes`, `deflate`/`crc32`, `time.utc`), plus two gaps moved out to
runtime-v2 ([7 observability](runtime-v2/07-observability.md),
[8 symmetric cipher](runtime-v2/08-symmetric-cipher.md)). And
[language 41](language-runtime-database/41-actor-arena-crash.md) — the arena
hang — was **root-caused and its fix designed to `ready`**.

**Language 41, settled:** the hang is a **double free from a broken invariant**.
`wo_db_rpc` marshals ("VM heaps are never read cross-shard"), but cross-shard
actor `send`/`call` pointer-shares the message into the receiver's shard — a
worker then drops an object in the sender's arena. **Fix = marshal cross-shard
messages** (copy into the receiver's arena, matching the DB RPC), which
eliminates the class by construction without needing the exact aliasing site;
**plus** aligning the `shard_id % nshards` route/compare mismatch and asserting
`shard_id < nshards`. Poison-on-free and a minimal corpus fixture are named
follow-ups. Proven against `archive/porch-idempotency` 18a–18h/19.

**Next steps:** implement language 41's marshal fix (unblocks
[porch 9](porch/09-idempotent-replay.md), already written); then porch is
buildable — [porch 5](porch/05-routing-response-ergonomics.md) has zero upstream
deps and is the natural start, with `random_bytes` (porch 2) opening the 3→4
chain.

### Landed 2026-09-04 — wmux: switch-client + choose-session (rung 21)

**Implemented (2026-09-04):** an in-session `switch-client -t B` / `-l`
moves a live client between sessions — the Input `call`s the session (sync,
so a refusal keeps the client on A), a `reg` handle threaded into every
session resolves B, the fds hand off WITHOUT closing, B adopts them and
spawns a fresh Input, the old Input exits only on success; B-occupied
refuses, missing→error, per-session `last_session` for `-l`. Plus a native
**`choose-session` picker** (bound `prefix o`): lists sessions on the status
row, a digit switches. Brainstormed + story-refined first (readiness ready),
then built. Landmine: a `?actor RMsg` nullable field reordered the checkpoint
schema (spurious restart migration) — fixed with a non-nilable `me` +
`DeadReg` placeholder. Gate 52 → 54/0; committed + reinstalled. Full fuzzy
`sesh connect` (fzf + zoxide/config dirs) still needs the tmux-compatible
CLI shim — that's rung 23.

### Landed 2026-09-04 — wmux: theming, active-pane border, automatic-rename

**Implemented (2026-09-04):** three developer-requested UI features, each
gated (`just wmux` 50 → 52/0) + committed + reinstalled.
- **Theming (rung 22, first slice)** — a tmux-style style engine
  (`style_sgr`: `fg=`/`bg=` named/`bright*`/`colourN`, plus
  bold/dim/italic/underscore/reverse), read from durable options
  (`opt_style`). Applied to `status-style`, `window-status-current-style`,
  `pane-active-border-style` — all `set-option`-able, restart-durable.
- **Active-pane border** — the split divider is now the active border:
  drawn in `pane-active-border-style` (green default) as box-drawing, with
  a marker pointing at the focused pane (`◄`/`►` / `▲`/`▼`). Focus changes
  (click / `select-pane` / cycle) redraw so it follows immediately. This
  answers the "panes not clickable" report — clicks always worked (the
  cursor moved), there was just no visible cue.
- **Automatic-rename** — the VTE captures the pane's OSC 0/1/2 title; the
  Window pushes it to a non-durable `AutoName` table; `window_list` shows
  it unless a manual `rename-window` overrides. So starship/vim/bash
  setting the title renames the window (refreshed on the status tick).

**Also fixed same day:** the popup mouse wheel (was dropped — now forwarded
to the popup's app as translated SGR) and mouse re-arm after a full-screen
app disables it (`\e[?1000l` on exit); both gated.

**`.dev/reference`:** the developer's live lazydocker session + screenshots;
tmux's `pane-active-border-style` / `automatic-rename` / style-string shape.

**Still the big ones (each its own focused run, NOT rushed):** N-way panes
(3+) + break/swap-pane (rung 10 full), sesh `switch-client` (20), plugin
ports thumbs/fzf/fzf-url (21), control-mode `%notifications` (14),
run-shell/if-shell, pane/layout persistence (16). And the rest of the new
[mouse-UX rung 19](wmux/19-mouse-ux.md): status-line click → window,
drag-resize, drag-select.

### Landed 2026-09-04 — wmux: lazydocker popup renders clean (OSC + flicker)

**Reported:** the developer ran `lazydocker` via `display-popup`; it showed
`8;;` garbage smeared across every border/panel and flickered — while the
same lazydocker in tmux was pixel-clean.

**Root-caused (measured, not guessed):** captured lazydocker's real pty
bytes and replayed a 24 KB slice through the VTE. Two gaps, both gated now:
- **OSC dropped.** lazydocker wraps every bordered element in an OSC-8
  hyperlink (`\e]8;;URI\e\…\e]8;;\e\`). The ESC dispatch had no `\e]` arm,
  so it dropped `\e]` and printed the `8;;` payload as cells. Now `\e]…`
  consumes the string to its `ST`/`BEL` terminator (covers OSC 0/2 title +
  OSC 52 clipboard); `\e(`/`\e)`/`\e*`/`\e+` eat their charset byte too (no
  literal `B`/`0`). The garbage was **repaint-only** — a focused pane
  passes OSC raw to the client's own terminal, which handles it.
- **Flicker.** `PopReader` read 4 KB, so a ~24 KB frame split into ~6
  partial repaints. It now reads 64 K and drains already-available bytes
  (3 ms poll) → one frame, one paint.

**Verified:** replay of the real capture shows box-drawing/colours/text
intact, zero `8;;`. New gate leg runs an OSC-8 line in a popup (the
repaint path) and asserts clean. `just wmux` **46 → 47/0**. Binary
rebuilt (`woc build … -o wmux`) + reinstalled to `~/.local/lib/wmux/wmux`.

**`.dev/reference`:** a live `lazydocker` capture via a python pty harness;
the developer's screenshots (wmux vs tmux side by side).

**Next:** the big remaining parity items are unchanged (N-way panes, sesh
switch-client, plugin ports) — see the marathon entry below.

### Landed 2026-09-03 — wmux tmux-parity marathon (7 rungs, all gated)

**Implemented (2026-09-03, "complete all rungs" multi-rung push):** the
switch-blocking tmux features, each gated (`just wmux` 45 → 46/0) +
committed + reinstalled to `~/.local/lib/wmux/wmux`, in dependency order:
- **Rung 10 core** — `split-window -h` (side-by-side) + `-v`; directional
  `select-pane -L/R/U/D` (h/j/k/l); zoom (`resize-pane -Z`). Fixed a
  spawn-time winsize race (spawn panes at their band size).
- **Rung 12** — system **clipboard** (OSC 52 on yank, verified base64);
  **mouse** (SGR enable on attach, wheel→copy-scroll, click→select-pane).
- **Rung 20** (committed at the time as "rung 19") — **display-popup**: a
  session-owned modal float running a command (lazygit/lazydocker/sesh) in
  a bordered box, reaped on exit.
- **Rung 13** — copy-mode **char selection** (vi `v`/`y`, highlighted,
  multi-line range yank → buffer + OSC 52).
- **Rung 15** — terminfo-lite: accept the common TERM families (tmux/
  screen/alacritty/kitty/…), still refuse `dumb`.
- **Rung 17 (full)** — **`#(shell-command)`** (cached, `time.after`-
  refreshed) PLUS the recursive expander: `#{?cond,a,b}` conditionals,
  `#{b:}`/`#{d:}` modifiers, `#{time}` clock, `#{host_short}`.
- **Window names** — durable `rename-window`, shown as `[idx:name*]`,
  `#{window_name}`; **last-window** (`prev`).
- (Earlier same day) **Rung 18** — key tables (`bind-key -n`, Meta/named
  keys); and the screen-completeness cluster (UTF-8, sizing, alt-screen,
  erase 0/1, SGR reset, O(n log n) replay).

**Config:** `~/.config/wmux/wmux.conf` maps the developer's tmux binds
(prefix C-a, `-n M-h/M-l`, h/j/k/l select-pane, z zoom, %, G/D/T popups),
auto-loaded by the launcher.

**Daily-drivable now on xterm.** Remaining for FULL parity (larger, each
its own effort) — rung numbers corrected to final: **N-way panes (3+)** +
break/swap-pane (rung 10 full); ~~format conditionals/modifiers (17)~~ and
~~theming (22)~~ and ~~sesh switch-client (21)~~ have since landed;
**plugin ports** thumbs/fzf/fzf-url + the tmux-compat CLI shim (rung 23,
popup+capture-pane ready); control-mode `%notifications` (14),
run-shell/if-shell, pane/layout persistence (16).

**`.dev/reference`:** the developer's `~/.tmux.conf` + plugins (the target
config), tmux `popup.c`/`window-copy.c`/`tty.c` shapes, runtime `proc`/
`term`/`time` seams.

### Landed 2026-09-03 — wmux screen-completeness + rung 18 (key tables, first slice)

**Implemented (2026-09-03):** a burst of real-usage fixes driven by
running wmux with the developer's live starship/eza + tmux setup, plus
the first slice of the new [key-tables rung 18](wmux/18-key-tables.md).
- **Dynamic sizing** — the session sizes to the client's terminal via
  `term.size` at attach (was a fixed 80×23 box); gate 120×40 → 39×120.
- **Screen completeness (vte.wo)** — the alternate screen (`\e[?1049h/l`,
  so btop/vim stop bleeding into the shell), scroll region + cursor
  save/restore, erase-display modes 0/1 (`\e[J` clears stale lines below
  the cursor), a per-row SGR reset (no colour-bleed blank rows), and
  **UTF-8 decoding** (one cell per glyph via `term.width` — nerd-font/CJK/
  emoji render instead of `000`).
- **O(n log n) boot replay** — killed an O(n²) startup CPU burst.
- **Rung 18 slice** — a no-prefix `RootBind` table, `key_code` (Meta +
  named keys), a tty key decoder in the Input actor, `bind-key -n`/`-T`.

**Key findings (measured):** a byte-based VTE mangles every multi-byte
glyph — `feed()` now decodes UTF-8 lead+continuation bytes into one cell
and `term.width(cp)` sets the advance. `\e[K` erases with the CURRENT
SGR, so an un-reset colour painted whole blank rows once the grid filled
the screen. And a **compiler bug** surfaced: `self.f = self.f .. x`
(self-referential field concat-assign) miscompiles — worked around with a
local temp, `emit.ml` fix tracked. Gate `just wmux` 37 → 42, 0 failures.

**Dependencies unblocked:** rung 18's decoder + tables are the seam the
copy-mode-vi table and `-r` repeat extend; UTF-8 + sizing make the VTE
usable for real prompts/TUIs. The audit's siblings have since landed with
final numbers — 17 formats-v2, 20 display-popup, 21 sesh switch-client, 22
theming all DONE; 23 plugin ports (the tmux-compat CLI shim) remains.

**Next steps:** finish rung 18 (copy-mode-vi + `-r`, with rungs 10/13), or
the paused rung 10–22 story map; the `emit.ml` self-concat fix is a
standalone language follow-up.

**`.dev/reference` used:** the developer's own `~/.tmux.conf` + plugins
(the config the fixes had to render), and the runtime's `term`/`sysio`
seams (`term.size`/`term.width`, EIO).

### Landed 2026-09-02 — wmux 16 (first slice): the Window owns + reaps its panes

**Implemented last time (2026-09-02):** the pane-ownership + reaping fix,
a discrete slice of [rung 16](wmux/16-durability-polish.md) surfaced by
live usage — a pane whose child exited on its own (a shell exiting, a
command pane finishing) was left a `<defunct>` zombie until the session
was killed. The `Window` actor now spawns its OWN panes: `make_window`
and `do_split` `call` the Window (`kind 0` / `kind 6`), it `spawn_pane`s
in-actor (so `owner_actor` = the Window) and returns the reader fd. A
pane's Reader, on EOF/EIO, sends `kind 8`; the Window `wait_dl`s the
child on its own shard to reap it and marks it dead. `DeadWin` and the
`kind 9` replay-feed became dead code and were removed. Gate leg
`attach-zombie` added; `just wmux` 36 → 37, 0 failures.

**Key findings (measured, not asserted):** the leak was structural, not
a missing `wait()` — the runtime's child slots are PER-SHARD, so
`proc.wait_dl(id)` only works from the actor that spawned the child.
Instrumentation proved it: two live panes reported the SAME shard-local
id (64), and the reader's `wait_dl` trapped `"process id is not a live
child"` because it ran on a different shard. So the kill-time `wait_dl`
was silently failing too; only owner-actor death (kill-session, server
exit) was actually reaping. Moving the spawn into the Window put the
child and its reaper on one shard. Verified: after a command pane and a
window shell both exit, the server has zero defunct children; 0 traps.

**Learned:** an actor cannot fetch its own address (no self primitive),
so the Reader's window address is threaded from the spawn site — but the
child fd can ride back through a synchronous `call` return (WMsg receive
returns Int), which let the Window own the spawn while the caller (which
holds the window address) wires the Reader. That `call`-returns-a-fd
shape is the clean way to keep ownership and wiring in the right actors.

**Dependencies unblocked:** the rest of rung 16 (pane/layout
persistence, killw compaction, named buffers) is unchanged and still
pending. The Window-owns-panes shape also makes per-pane resize and
future pane persistence cleaner (the Window is now the single owner).

**Next steps:** the remaining wmux burn-down — rung 10 (layout tree) or
rung 12 (resize + mouse); then cherry-pick the wmux track dev→master
when the ladder is declared ready.

**`.dev/reference` used:** the runtime's own `sysio.c`
(`WO_B_PROC_WAIT_DL`, `proc_slot_by_id`, `owner_actor`) — to source the
per-shard child-ownership model that dictated the fix.

### Landed 2026-09-02 — wmux 11: options/formats/keys + two baseline bug fixes

**Implemented last time (2026-09-02):** wmux
[rung 11](wmux/11-formats-options-keys.md) — behaviour became durable
DATA. A `Setting {key, val}` options table (`set-option`, `wmux_opt`)
and a `Bind {key, cmd}` key table (`bind-key`), both seeded idempotently
at boot and replayed after a restart; a `#{...}` status-format expander
(`format()`); and ONE `run_command`/`run_session_command` dispatcher
that the CLI, control mode, the `C-b :` prompt, key bindings and the
`WMUX_CONF` config file all feed. Folded rung 14's command-prompt race
fix by moving the line editor into the Input actor. Pure `.wo`, zero
runtime work. `just wmux` 30→36 checks, 0 failures.

**Key findings (measured, not asserted):** the tmux options/format/keys
DSL (~12k lines in tmux) collapses to two durable tables, a ~40-line
`#{...}` walker, and one dispatcher — and unlike tmux the config SURVIVES
a server restart (proven: `set-option prefix C-t` + `bind-key X` are in
effect after SIGTERM). The prompt race the command-pane rung disclosed is
gone: with the flag and keystrokes in one actor, `C-b : split <cmd>` runs
the whole line. The gate's old "command prompt neww" leg was a false
positive — its `[1` needle matched an ANSI cursor escape, not a window;
the new legs assert on the expanded format text instead.

**Learned (two latent baseline bugs the new paths exposed, both fixed):**
(1) `net.read_dl` returns nil on a timeout but TRAPS on a hard error, and
a dead PTY master returns EIO, so the pane Reader's `catch (e) nil;
continue` pinned a core at 100% the moment ANY command pane's child
exited — a pre-existing runaway confirmed identical on the pre-rung-11
build. Fix: the catch `return`s (the parser allows a `{ … }` catch arm),
stopping the reader like EOF; same guard added to the Input tty read.
(2) `kill-session` fired `kind 4` at every window at once and each
scanned the shared `Chunk where c.sess` bucket and deleted — colliding
cursors trapped WO-5 "no such row"; chunk cleanup moved into the
serializing session. Only reachable once prompt-`neww` opened real
windows.

**Dependencies unblocked:** rung 14 narrows to the control-mode command
surface + `%notifications` (its prompt-race scope is done). The unified
dispatcher is the seam rungs 12–16 extend (each new verb is added once).

**Next steps:** the remaining wmux burn-down — rung 10 (layout tree) or
rung 12 (resize + mouse, runtime already ready via rt2 3/6); then
cherry-pick the wmux track dev→master when the ladder is declared ready.

**`.dev/reference` used:** tmux (`options.c`, `format.c`, `key-bindings.c`
— the option/format/key shapes wmux compresses); the runtime's own
`sysio.c` `read_dl` (to source the EIO-vs-timeout distinction).

### Landed 2026-09-02 — runtime-v2 COMPLETE: all five iterations in one run

**Implemented last time (2026-09-02):** the whole
[runtime-v2 track](runtime-v2/00-story.md) — streaming subprocess
(`proc.spawn`/`wait_dl`/`signal`, Child record, kernel-pipe
backpressure), PTY (`spawn_pty` via posix_openpt + `resize`), signals as
events (`signal.on` delivering Signal records), termios
(`term.raw/restore` with runtime-guaranteed restore), and fd passing
(`send_fd`/`recv_fd`/`connect_unix`). Ids 97–107, five commits, one
[plan](../superpowers/plans/2026-09-01-runtime-v2.md) against the
[track spec](../superpowers/specs/2026-09-01-runtime-v2-design.md).

**Key findings (measured, not asserted):** the PULL design paid off
exactly as argued — the five iterations added ZERO transport code; the
existing net verbs drove pipes, PTY masters and received fds unchanged
(`cat` echo, `stty size`, cross-socket pipe reads all through
`read_dl`/`write_dl`). A real child's SIGUSR1 landed in an actor's multi
as one coalesced Signal record. The tty that crossed the unix socket was
raw'd through the RECEIVED copy and restored at vm destroy — wmux's
detach/attach handover, proven in miniature. `test_proc` 193/0,
`test_term` 60/0, both dispatch flavors ASan clean; woc 557/0;
subprocess-accept 12/0; site-accept 23/0.

**Learned (three spec amendments, recorded in its History):** message
payloads are unconditionally dropped as heap objects, so scalar messages
crash by construction — anything delivered to an actor must be a record;
a streaming slot must NOT own the caller-visible fds (recycled numbers —
the sweep would close a stranger); and signalfd was the wrong mechanics —
the stop-latch pattern generalized (handler latch + wake eventfd + drain
at `wo_io_wait`'s loop head) needs no mask plumbing at all. Bonus: the
double-raw refusal is itself a trap, so the termios obligation restores
the terminal even THERE — the test caught it as a "bug" that was the
design working.

**Dependencies unblocked:** every runtime edge into
[wmux 1](wmux/01-wmux.md) is green — what remains for wmux is its own
`.wo` work (VTE grid + unicode width tables, server/client, the gate)
plus its brainstorm's terminfo fork. The alacritty stage A (headless PTY
runner) is fully unblocked; the zen CDP driver now lacks only the
WebSocket client; skillhost's stdin transport exists.

**Next steps:** cherry-pick `rt2` to master when declared ready; then
wmux 1's brainstorm (terminfo fork, v1 surface) — the first product
slice of the goal recorded 2026-09-01: acceptance by Linux-based
developers.

**`.dev/reference` used:** tmux (`spawn.c`, `imsg-buffer.c` — the
fd-passing and PTY shapes), the kernel's pidfd/termios/SCM_RIGHTS
interfaces.

### Landed 2026-09-01 — iteration 42, bounded subprocess (brainstorm to gate in one day)

**Implemented last time (2026-09-01):** iteration
[42](language-runtime-database/42-bounded-subprocess.md) end to end —
`proc.run` reworked from shard-blocking to parked (pipe read ends +
pidfd behind one epoll fd, the `_dl` retry mould), bounds everywhere
(30 s / 1 MiB / 64 KiB defaults; per-shard ceiling 32; every violation
kills the child and traps `WO_T_IO` naming the bound), owner-bound
reaping (`fib_reap`/`wo_vm_destroy`/stop all sweep), and `proc.run_dl`
(id 96) stating bounds per call. New suite `runtime/test/test_proc.c`
(128 checks) and `docs/examples/subprocess` + `just subprocess`
(12 checks). [Spec](../superpowers/specs/2026-09-01-bounded-subprocess-design.md)
· [plan](../superpowers/plans/2026-09-01-bounded-subprocess.md).

**Key findings (measured, not asserted):** the suspected drain deadlock
was REAL — a child writing 200 KB to stdout while holding stderr open
hung the old `proc.run` until the test's 5 s alarm (stdout silently
truncated at 8,192 bytes, exit code lost to SIGPIPE); the parked rework
answers the same child in 15 ms. A `ping` request was answered in 2 ms
while a `sleep 2` child was parked on the same shard. One thousand
sequential spawns left the fd table byte-flat. SIGTERM with a `sleep 30`
child live: clean exit 0, child pid verifiably gone from outside.

**Learned:** a new sysio builtin id is THREE registrations, not one —
the wob.h enum, the loader's arity table, and builtin.c's dispatch
range; missing any of them surfaces as `unknown stdlib builtin` from a
perfectly valid image. And glibc 2.35 (the release build floor) has no
pidfd wrappers — raw `syscall(SYS_pidfd_open/…_send_signal)` or the
release build breaks.

**Dependencies unblocked:** the streaming form (long-lived children,
output as mailbox messages) now has its registry/pidfd/cap machinery
built; the tmux/alacritty studies' stage A and the zen study's CDP
driver (stage C′) queue behind that plus their own named gaps
(PTY/termios/fd-passing; ws-client). Iteration 28's "bounded subprocess
first" ordering item is spent.

**Next steps:** cherry-pick lang42 to master when declared ready; the
startable set otherwise unchanged. The exploration studies' next
builtin-sized item is the WebSocket client (zen C′).

**`.dev/reference` used:** alacritty, tmux, zen-browser (the three
parity studies that promoted this gap to an iteration); the kernel's own
pidfd/epoll interfaces for the mechanics.

### Landed 2026-08-30 — keys-resident delta updates DONE, loader refusal lifted

**Implemented last time (2026-08-30):** the six-task
[keys-resident delta updates](../superpowers/plans/2026-08-30-keys-resident-delta-updates.md)
plan's final task — lifting the `runtime/src/loader.c` refusal of
`resident: keys` and proving update end to end. The refusal (databasev2 2's
Outstanding criterion) is now Met: a keys-resident row updates through a WAL
delta record, read-modify-**append**, folded back to a value by
`wo_wal_fold_row_at` on every read, replay and compaction. Proven four ways —
the fold itself (earlier tasks), group-commit staging with the id-map re-point
deferred to the post-barrier flush, replay/compaction folding delta chains the
same way reads do, and this task's oracle test
(`test_oracle_all_vs_keys_same_update_sequence`, `runtime/test/test_wal.c`)
driving the SAME update sequence against a `resident: all` table and a
`resident: keys` table and asserting byte-identical rows at every step.
`docs/examples/residency`'s `Product` table is genuinely `resident: keys` now;
`scripts/residency-accept.sh`'s gate leg inverted from "the annotation is
refused" to "the program runs and `place_order`'s stock decrement survives a
restart" (11 checks, 0 failures).

**A second bug surfaced auditing the request path before lifting the
refusal** — the same audit class that caught `delete`'s memory corruption
in the prior session. `idx_hash`, `idx_cols_equal` and `wo_idx_probe`
(`database/src/table.c`) read a TEXT column's slot as an engine `db_text*`,
but a keys-resident borrow was handing back VM-decoded `wo_str*` — a
different struct layout, reproduced as a genuine ASan heap-buffer-overflow,
not merely wrong values. The same bug was independently present in `db.c`'s
`GET_FIELD` and `PROBE` arms (inline and request-path), unaudited until now
because nothing could reach a keys-resident row through them while the
annotation was refused. Fixed at the root: a keys-resident borrow now hands
back engine values, exactly `wo_row_ptr`'s contract for `resident: all`
(`table.h`'s own "a row stores NO VM pointer" doctrine) — no index function
needed to change, and `db.c` needed none either. Pinned by
`test_keys_resident_update_indexed_text`, which reproduces the overflow
against the pre-fix code; all five pre-existing tests that read a
keys-resident Text field directly were auditing the OLD (wrong) contract and
are corrected alongside it. `test_wal` 4746/0 throughout.

**What did NOT land, by design — three limitations documented, not fixed:**
(1) mid-drain stale reads — a request reading a row in the same uncommitted
drain as an earlier request's in-flight update to it may see the last durable
value, not that write; (2) replay is O(N²) in a row's delta-chain length,
since each replayed delta re-folds the whole chain; (3) compaction triggers on
byte ratio only, with no per-row delta-count signal, so one hot row (a single
popular SKU — this feature's own motivating workload) can grow a long chain
without moving the aggregate ratio enough to checkpoint. Item 3 is the
sharper finding: the design's decision not to cap chain length rests on
compaction bounding it, and for a hot-row workload it does not. Recorded in
[the story](databasev2/02-table-storage-modes.md) and the example's README.

**.dev / reference projects used:** none — internal-only, `table.c`/`wal.c`/
`db.c` read directly to audit the request path and trace the representation
mismatch.

**Dependencies unblocked:** none newly technical — databasev2 2's own tasks 6
(the two runtime refusals: no-`WO_DATA`, the byte budget) and 7 (measure, gate,
close out) were already the next items and do not depend on this.

**Next steps:** databasev2 2 tasks 6/7, as before. `database/src/CODE-LOGIC.md`
is current with the stage-here/commit-in-caller update contract and the
engine-representation fix.

### Landed 2026-08-30 — porch 1 DONE, store-backed middleware closed out

**porch 1 (store-backed middleware) is `status: done`.** Task 5 added the
last gate leg — pool saturation fails closed — and closed out the story: a
one-actor pool with `WO_MAILBOX` shrunk to 2, 15 genuinely concurrent
requests, exactly 3 served (1 running + 2 queued) and 12 answer 503 with
`Retry-After` and the real cause named, and the execution count matches the
200 count exactly (no overflow request runs uncounted). `scripts/web-app-accept.sh`
is now 79 checks, 0 failures (`just web-app`). README's two ledger rows (rate
limiting, idempotency) moved 🔶 → ✅, scoped to exactly what the gate proves,
plus four API facts anyone wiring this into a real app needs (`Idempotent` is
a `Handler` decorator not a `Middleware`; `Pool` cannot live in actor state or
a message, WO-E222; a `call` reply is a scalar only, WO-E226; pool size is a
capacity decision — undersizing means more 503s, never a silent bypass).
Fixed en route: `make_pool(n)` with `n < 1` was a mod-by-zero in
`pool_select`, guarded by clamping in `make_pool` itself — guarding the
division alone would not have helped, since every `pool_select` call runs
inside the middleware's own `try ... catch (e) nil` and would have swallowed
the trap as ordinary saturation forever.

**What did NOT fully land:** 2 of the story's 9 acceptance criteria
(window-elapse pruning, clock-monotonicity) are implemented and verified by
code inspection only, not by an integration leg — neither was gated even in
the original phase plan, and gating them (waiting out a real window, faking a
backward clock) is future work. Also unresolved, and explicitly NOT this
task's to fix: a pre-existing C-runtime defect (recorded in Task 4's own
notes) where concurrent `call()`-parked callers doing real per-request table
I/O leave `main()` returning cleanly while the OS process itself sometimes
hangs (~1-in-5). The new saturation leg is, by design, the sharpest
reproducer of it yet; it and idempotent-check's own SIGTERM leg both contain
it with an unconditional `kill -9` rather than asserting graceful shutdown, so
it cannot flake either leg's actual subject.

**.dev / reference projects used:** none — this task was internal-only
(runtime/src/vm.c read directly for `wo_mailbox_cap`/`WO_MAILBOX` semantics to
design a deterministic saturation leg).

**Dependencies unblocked:** none newly technical — porch 2 (randomness and
cookies) was already sequenced next, blocked only on its own CSPRNG builtin
(language track). What porch 1 settles is the store pattern and gate shape
2/3/4 inherit: serialize through an actor pool, persist in a `@table`, prove
every claim with a gate leg scoped to exactly what it shows.

**Next steps:** databasev2 2 tasks 6/7 remain the language-track's own
critical path (unaffected by this session); on the porch track, porch 2's
brainstorm (CSPRNG builtin id 96+, then repeated response headers) is next
whenever that track resumes.

### Landed 2026-08-30 — porch 1 (rate limiting), and a runtime crash found

**Implemented last time (2026-08-30):** porch 1's rate limiter, and only that.
Counting moved out of the request's own fiber into a sharded actor pool, so two
concurrent requests can no longer lose an increment — proven by 30 genuinely
parallel clients, not two sequential ones. Counters are WAL-durable across a
SIGTERM restart, `trust_proxy` is off by default with a `net.peer` fallback, and
saturation fails closed.

**The iteration was re-scoped mid-flight.** Idempotency was built, reviewed and
then reverted to [porch 9](porch/09-idempotent-replay.md), whole, in the tag
`archive/porch-idempotency`. Not a design failure — it passed its gates. It
provokes a C-runtime SIGSEGV in `wo_arena_alloc`/`wo_str_new` under concurrent
`call()`-parked callers, and its legs flaked between 0 and 6 failures run to
run. After the split: five consecutive runs at 56 checks, 0 failures. **A
feature whose test passes some of the time is not shipped**, and the limiter was
finished either way — it was being held hostage by a defect in code it does not
call.

**The most valuable output is arguably the bug, not the feature.**
[Language 41](language-runtime-database/41-actor-arena-crash.md) records a
SIGSEGV in the arena allocator under concurrent actors, with the evidence that
localises it: every failure belonged to the path with 5x the allocation inside
`receive`, none to the light path driving the same pool; and it scales with
sequential insert+delete volume on one key (N=4/5 crashed, N=1-3 clean over 12+
trials). Two smaller runtime defects came with it — `try/catch` cannot tell a
literal `Int 0` reply from a trap, and a `json.decode` value is corrupted when
embedded in a struct crossing a function-return boundary.

**Corrections to our own record:** the earlier claim that `Pool` being traced
forces a one-slot re-wrap was WRONG — WO-E222 fires on the class, not on
`multi`, so an actor can hold `slots: multi PoolSlot`. It shipped in a README
before being caught by the whole-branch review. All fifteen execution decisions
are in
[the rulings log](../superpowers/plans/2026-08-29-porch-store-backed-middleware-rulings.md).

**.dev / reference projects used:** the Fiber parity study for porch 1's scope.

**Dependencies unblocked:** porch 2-4 inherit the store convention — serialize
through an actor, persist in a `@table`, never read-modify-write from a handler
fiber.

**Next steps:** language 41, the arena crash. It outranks the remaining porch
work: anything built on actors is exposed until it is fixed, and porch 9 is
written and waiting on it.

### Landed 2026-08-29 — databasev2 2 tasks 5c/5d, and a branch consolidation

**Implemented last time (2026-08-29):** `resident: keys` storage and every read
path. Rows are stored keys-only — the payload dropped after the WAL barrier,
the id map holding a log offset instead of a slab slot — and read back through
a borrow/release accessor. The scans go through a row iterator that walks the
id map for a keys table and the bitmap for a resident one, deliberately, since
hash order would reorder every unordered query.

**The obligation databasev2 3 left at the compactor is discharged**, and it
caught more than it predicted. The recorded hazard was that compaction moves
records and invalidates stored offsets. True — but the compactor walked the
slab *bitmap*, which a keys-resident row has no bit in, so every such row would
have been omitted from the new log outright. Silent data loss, not a bad
pointer, and rebuilding offsets would never have caught it. Records are now
moved byte-for-byte and re-pointed as they land.

**What is NOT done, and why the annotation is still refused:** updating a
keys-resident row. It lives in the log with no slab slot to mutate, so writing
into the borrow's scratch would discard the write *silently* — the one failure
this iteration must not ship. It needs read-modify-append. `wo_row_update_field`
and its slot variant refuse explicitly, and the loader still rejects the
annotation, with its message corrected to say so.

**Also this session:** every branch consolidated to `dev` and `master` only.
Three could not be replayed and are preserved as annotated tags rather than
merged or discarded — `archive/cleanup-pre-existing-changes` carries the Rust
runtime master deleted, and `archive/ipc-attach` + `archive/keypair-auth`
refactor the same row-API functions `db2-keys` rewrote. That last one is not a
merge conflict but an integration task: iteration 9c transfers ownership of
`vals` on failure, while the keys-resident arm returns early without freeing, so
a merge that compiles and passes could still leak or double-free.

**porch 1 brainstormed and specced.** Phases B and C are superseded before
review — both store the response after the handler returns, which cannot detect
an in-flight collision at all. Design settled: serialize through a sharded actor
pool, persist in a `@table`. Found and fixed en route: `docs/examples/porch/`
did not typecheck at all, for want of a `use json`.

**.dev / reference projects used:** the PostgreSQL checkpoint study
(`docs/plan/exploration/postgresql/buffer-and-checkpoint.md`) for the compaction
shape; the Fiber parity study for porch 1's scope.

**Dependencies unblocked:** nothing was blocked. databasev2 2's remaining tasks
6 and 7 depend only on iteration 2.

**Next steps:** databasev2 2 task 6 (the two runtime refusals) and task 7
(measure, gate, close out), then the porch 1 implementation plan.

### Landed 2026-08-29 — databasev2 3, WAL checkpoint (the chain's last link)

**Implemented last time (2026-08-29):** compaction. The log used to grow forever
— nothing removed superseded records, so boot replayed all history. It is now
rewritten as one record per live row into a temp file and swapped in with
`rename`. Six tasks, brainstormed and spec'd first
([spec](../superpowers/specs/2026-08-28-wal-checkpoint-design.md) ·
[plan](../superpowers/plans/2026-08-28-wal-checkpoint.md)).

**Key findings (measured, not asserted):** **2.16× space reclaimed**
(1 962 358 → 907 094 B), **boot 114 → 64 ms**, stop-the-world pause **2 651 µs**
against a stated 50 ms budget. Reading `.dev/reference/postgresql` was what made
the design defensible rather than lazy: **Postgres never compacts its WAL**,
because its records are page deltas and a compacted redo log is not a store —
hence heap files, a control file, a redo pointer, a second recovery source and a
separate checkpointer process. Ours are **full row images**, so a compacted log
*is* a complete store, and all of that machinery disappears. What was worth
porting is the ordering discipline — publish the switch atomically and last — and
one `rename` provides it.

**Learned — two bugs of mine that measurement found, not review:** wiring the
trigger only into the drain left **`WO_SHARDS=1` never compacting**, its log
growing forever (536 KB where multi-shard held 446 KB), because a statement on
the owner shard never enters that drain. And the dump was **8× slower than
necessary**, flushing through the committing path and paying one `fdatasync` per
256 records for durability that is worthless before the rename — one final
barrier took a 2 MB dump from 107 649 µs to 13 212 µs, ~22 MB/s to ~181 MB/s.
Separately, the crash battery's *first* version failed on correct code ~1 run in
3: it acked deletes after committing them, so a kill in between made it demand a
row the engine was right to remove. Deletes now announce intent first.

**Dependencies unblocked:** every link in the concurrency + fiber chain has now
landed its planned work — stage 3 → 22 → 24 (absorbing 31 + 34) → 40 →
databasev2 4 part A → databasev2 3. **Not "complete", precisely:** chain 5 stays
`in-progress` because databasev2 4's part B was never done, and its premise was
invalidated by part A rather than satisfied. Nothing in the chain is blocked on
anything else in it.

**Next steps:** the honest queue is (1) databasev2 2's outstanding 5c/5d, whose
`resident: keys` half is unimplemented and now carries a recorded obligation —
compaction invalidates every WAL offset it stores, so the compactor must rebuild
that map; (2) databasev2 4 **part B**, whose premise was invalidated by part A
and which needs re-brainstorming rather than starting; (3) the O(live rows)
pause, ~5.5 s at a 1 GB live set, which is the number an incremental checkpoint
must be bought against.

**`.dev/reference` used:** `postgresql` — `xlog.c` (`CreateCheckPoint`, segment
recycling), `checkpointer.c` (the time-or-volume trigger), and
`controldata_utils.c`, which also corrected a prior exploration doc: Postgres
updates its control file **in place with a CRC**, not by rename.

---

### Landed 2026-08-28 — databasev2 4 part A, WAL group commit

**Implemented last time (2026-08-28):** one durability barrier per drain
instead of one per statement. Shard 0 stages every queued write request, holds
each reply, commits once when its queue empties, then releases all — so a writer
is acknowledged after the barrier that carried *its* record, which was the
intended contract all along and was true before only because every batch had one
member. Six tasks, brainstormed and spec'd first
([spec](../superpowers/specs/2026-08-28-wal-group-commit-design.md) ·
[plan](../superpowers/plans/2026-08-28-wal-group-commit.md)).

**Key findings (measured, not asserted):** **≈2.9× durable write throughput,
≈2.1× lower p50** on a write-concurrent workload, confirmed a second way by the
`s1`-vs-`sN` split within one build (1467 → 5117 ops/s, mean batch 1.0 → 5.43,
peak 57) — 2.9× and 3.5× agreeing. Batching scales with contention: mean batch
1.13 / 1.76 / 5.35 at C = 4 / 16 / 64. **The story's premise was wrong**: it
said "fsync-per-commit" and the engine was fsync-per-**statement**, committing
after every append at all six sites — so part A was closer to deleting calls
than adding a mechanism.

**Learned — three things the measurement corrected, not the code:**
(1) **`/tmp` is tmpfs here, where `fdatasync` is free.** The same run reported
195 000 ops/s at p50 1 µs there against 2200 at 7200 µs on ext4. A group-commit
measurement taken on a memory filesystem measures nothing; `db-bench` is right
to keep its stores under `bench/`. (2) **No existing leg could exercise the
feature** — `mix` writes on one op in ten with C=4, giving 20 writes and mean
batch 1.01, so a `wmix` write-concurrent leg had to be added or the payoff was
unevaluable either way. (3) **The before-p99 was off the instrument** —
`hist_add` clamps at 20000 µs and both before-runs pinned there, so the gain is
*at least* 2.3× and the true old p99 is unknown.

**Dependencies unblocked — and one dependency invalidated.** `WO_T_IO` is
unreachable from a DB write: a failed stage or barrier now ends the process
(exit 74, diagnosed), replacing three behaviours that disagreed — `insert`
un-applied itself while `update` and `delete` returned a catchable trap and
admitted in their own comments that they left RAM ahead of disk. **Part B's
premise is invalidated**: it was justified by "close the 66× durable gap", but
that gap is two problems. Concurrent fan-in was a batching problem and is now
~3× better; a **serial** writer waiting on one barrier is a latency problem that
batching cannot touch and io_uring does not obviously help either. Part B should
be re-brainstormed, not started.

**Next steps:** either re-brainstorm part B against its corrected premise, or
take chain 6 ([databasev2 3](databasev2/03-wal-checkpoint.md), WAL checkpoint),
which now has the replay "before" it lacked. **(Superseded 2026-08-29: it
landed.)** Two debts named rather than hidden:
the abort path is not exercised (forcing a real `fdatasync` failure needs mount
privileges), and single-shard concurrent batching needs the inline-path park —
the same machinery part B would need.

**`.dev/reference` used:** none this slice. The sources were the engine's own
code and the Linux `fsync`-failure semantics that make retrying unsound.

---

### Landed 2026-08-27 — iteration 24, chat + actor lifecycle (absorbing 31 + 34)

**Implemented last time (2026-08-27):** the slice closed and merged to master
(`ed5334d`, fast-forward). T4 `monitor` + T5 `time.after` (ids 89/90) had
landed on the branch; this session merged master in (adopting the `porch`
rename), finished T8/T9, fixed the gate, found and fixed a runtime bug, and did
T10. Iterations 31 and 34 land inside it.

**Key findings (measured, not asserted):** finishing the gate mattered more than
finishing the sample. Making **every leg start its own server** — instead of the
drain leg inheriting the soak's warmed one — exposed that **5 of 16**
fresh-server SIGTERM drains left a client at EOF with no close frame and no
diagnostic. Traced to `shard_main`: `NEXT_RUNNABLE()` already stated the
contract ("a WORKER on stop keeps DRAINING … close frames!") but the **idle**
branch reaped and broke, abandoning its inbox. An actor between messages is
exactly that idle case. Split out as
[40](language-runtime-database/40-shutdown-drain-guarantee.md); **20 of 20
clean** after. Also measured: the fd check had been core-count dependent — lazy
per-shard init takes one `io_uring` + one `eventfd` per shard, capped at
`nproc`, so 26 → 44 on a 20-core box read as a leak. **1000 connections left it
at 44**, which settled it.

**Learned:** three of the four gate failures were **stale build artifacts**, not
code. A branch switch leaves `compiler/_build/` and `runtime/build/` holding the
other branch's binaries, and a `woc` emitting `.wob` v7 against a v6 runtime
surfaces only as "no listener" — rebuild both before believing a gate failure.
And a gate that reuses another leg's server is not merely untidy: it hid a real
bug, and when its own leg failed it orphaned a listener that broke the *next*
run. Example apps now log to `/tmp/<app>.log` so a developer can `tail -F` them.

**Dependencies unblocked:** PUBSUB2 (WebSockets + pub/sub, rejected until this
point) is done; the porch ledger's WebSocket rows are ✅ and its cancellation row
is unblocked-not-built. Chain position 4 is complete, so **the chain's next link
is [databasev2 4](databasev2/04-io-uring-commit.md)** (io_uring group-commit).
Still blocked: CSRF and sessions — iteration 34 shipped HMAC but **there is
still no RNG**, and HMAC authenticates a token without being able to mint one,
which is [39](language-runtime-database/39-web-framework-parity.md)'s leading
item.

**Next steps:** databasev2 4, or databasev2 2's outstanding 5c/5d. One debt is
named rather than hidden: iteration 40's guarantee is proven only by the chat
gate — nothing in `runtime/test/` drives `wo_engine_start`/`wo_engine_stop` and
no corpus fixture can trigger a stop, so pinning it lower needs new
multithreaded test infrastructure.

**`.dev/reference` used:** none this slice. The sources were RFC 6455, RFC
3174/4231 for the digest vectors, and the kernel's own interfaces for the drain.
### Landed 2026-08-27 — databasev2 1, the RAM ceiling measured

**Implemented last time (2026-08-27):** databasev2 1 refined (three forks
settled) and implemented. A text-heavy `Wide` reference shape beside the
Int-only `Item`; `growth N int|text` in the db-bench sample, reading its OWN
`/proc/self/status` RSS at each decile because the driver's 250 ms poll misses
the value *at* a boundary; `growth-verify`, which asserts the survivor of a
crash is a contiguous intact prefix; and two harness legs — four footprint legs
under a rootless cgroup v2 cap, a `ceiling` leg that deliberately dies at the cap
and then replays, a `randread` leg that reads an oversized table randomly, and a
`replay` leg that times boot against history length. 148 checks, 0 failures.

**Key findings (measured, not asserted):** per-row footprint is **96.5–100 B**
Int-only and **320.6–324 B** text-heavy — **3.3×**, not the "order of magnitude"
three docs asserted. Read as the median of per-decile marginals, never a
two-point slope: index doublings make a two-point read swing 2× (96 vs 205 B/row
for one shape). **Two predictions in the iteration's own premise were wrong.**
The ceiling is not a catchable `WO_T_OOM` for table storage — it is **SIGKILL,
signal 9**, because `vm.overcommit_memory = 0` lets `malloc` succeed and the
kernel kills on page *touch*, so the checked path never runs (the VM arena is
the opposite: `WO_HEAP_MB` is checked and traps). And swap is not "latency
collapse": 900 000 rows inside a 64 MiB cap with swap finished in **148 s
against 150 s uncapped** — ~1%, on a real disk swap file with no zram. Also
measured: **ack-after-fsync holds through an OOM kill** — ~40 000 rows came back
as an intact prefix, no holes, not read as corruption. And the pattern the swap
leg was missing: **random reads over an oversized table collapse 273×.** Plus
replay: **≈5.5 µs per WAL record**, and **1.9× the boot cost for an identical
live dataset** once each row has been updated once (110 → 211 ms for the same
20 000 rows) — boot replays **history, not data**.

**Learned:** an append-mostly workload never re-touches its cold pages, so swap
costs it nothing — and the opposite pattern was then measured on the same day.
The `randread` leg reads randomly across a table larger than the cap, both legs
walking the SAME Weyl key order so residency is the only variable: **273×
throughput collapse** (1 851 166 → 6 771 reads/s), p99 **1 µs → 487 µs**, all
20 000 reads resolving in both. So the two access patterns sit ~270× apart under
identical memory pressure, and **departure is a step, not a curve** — which is
why `p99_departure_decile` finds nothing: there is no knee to find. The RAM ceiling therefore has two shapes and neither
announces itself: without swap the process vanishes on signal 9, with swap it
keeps returning 0 while serving from disk. That is the argument for a budget
that fires at a declared threshold instead of at exhaustion.

**Dependencies unblocked — one, by *removing* it:** iteration 2's
resident-footprint budget default was to be derived from "swap onset". **There is
no onset.** Swap-off jumps straight from working to SIGKILL; swap-on shows no
degradation to detect. Iteration 2 must pick its budget on other grounds rather
than wait on a number this slice cannot produce. Iteration 3's replay baseline is
still NOT delivered — `bench/baseline.json` times no replay.

**Next steps:** iteration 2's 5c/5d. Its task 7 gained a criterion from this:
`resident: keys` must measure its OWN read path rather than inherit 273×. That
number bounds demand-paged anonymous memory through swap (4 KiB per fault, no
readahead); `pread` through the page cache should beat it, and **the entire value
of `resident: keys` rests on how much** — if it is not materially better than
swapping, the design buys nothing the kernel was not already doing.

Iteration 3 now HAS its before, and a correction: it planned to use "22's
aged-store replay numbers", which never existed — 22 proved restart correctness
and never timed it. Also recorded there, before it could be rediscovered late:
**compaction invalidates every `resident: keys` offset**, since it rewrites the
log and moves every record. Not stale-but-readable — an arbitrary byte in a
rewritten file. So compaction cannot be a pure file operation that ignores
in-memory table state.

**`.dev/reference` used:** none. Sources were the kernel's own interfaces —
cgroup v2 `memory.max`/`memory.swap.max`, `/proc/self/status`, `/proc/swaps` and
`vm.overcommit_memory`.

---

### Landed 2026-08-25 — packaging + release pipeline (off-chain, no story)

**Implemented last time (2026-08-25):** the toolchain became installable
by a stranger. `VERSION` as the single source (0.1.0, asserted against
both binaries by `scripts/mkdist.sh`), `just dist` producing
`writeonce-<ver>-linux-amd64.tar.gz` + `.sha256` with
`scripts/install-readme.tmpl.md` inside it, `just install-accept`
proving a from-scratch project builds against the extracted tarball's
own binaries, and `.github/workflows/release.yml` publishing on a `v*`
tag push. Runbook: [`guides/releasing.md`](../guides/releasing.md).

**Key findings (measured, not asserted):** the build host's glibc caps
what the shipped binaries can import, and that cap becomes every user's
floor — so `runs-on` is `ubuntu-22.04` (2.35) deliberately, not
`ubuntu-latest`; built on this dev machine the binaries need
`GLIBC_2.38`, which would silently exclude Ubuntu 22.04, Debian 12 and
RHEL 9. `ocaml/setup-ocaml@v3` gives a compiler and opam but **not**
dune, and pinning `dune.3.14.0` is a downgrade the solver refuses — take
whatever it provides, since any dune ≥ 3.14 satisfies `(lang dune 3.14)`.

**Learned:** the asset filename is load-bearing. `/install` links one
exact URL, so the workflow asserts tag = `VERSION` = asset name and
fails rather than publishing a download button that 404s. A measurement
that only prints is not a gate — the glibc floor is printed from the
artefact about to ship, so the claim on the page can be checked against
a build log instead of trusted.

**Dependencies unblocked:** nothing in the chain; this is the
distribution seam. It does make `docs/examples/site`'s `/install` page
truthful, which iteration 37's site restructure had left pointing at an
asset nobody had built.

**Next steps:** the live slice is iteration 24, untouched by this. CI is
release-only — no workflow runs the gates per change, which remains the
open half of iteration 30 (observability — now
[runtime-v2 7](runtime-v2/07-observability.md), moved there 2026-09-06;
CI and fuzz are tooling, split out).

**`.dev/reference` used:** none — GitHub Actions' own docs and the
runner images' glibc versions were the only sources.

---

### Landed 2026-08-25 — iteration 37, wo-html components (off-chain)

**Implemented last time (2026-08-25):** iteration 37 CLOSED, both
halves. The grammar half (2026-08-24) added the backtick raw text
literal — content verbatim, common margin removed at lex time, `${ }`
raw and `{{ }}` compiling to a call on the `esc` in scope — plus
WO-E004/WO-E005. The library half (2026-08-25) added `Component`,
`render_all` and `Layout` to wo-html, moved `ok_html` into
`framework/http` beside `ok_text`/`ok_json`, and migrated BOTH HTML
samples onto the component layer.

**Key findings (measured, not asserted):** `multi Component` holds a
heterogeneous list DIRECTLY — no wrapper record — so the framework's
`Mw`/`Aw` shape is a local choice, not a language requirement; that is
what made page components able to own their children. The whole
escaping desugar needed zero compiler knowledge of HTML: `{{ e }}` is a
`Call` on an ordinary in-scope `esc`, so typecheck, ownership, codegen,
the `.wob` format and the VM were all untouched. `{{ }}` was proven
byte-identical to the hand-written `esc()` calls it replaced, hostile
input (`< > & "`) included, across all eight migrated builders.

**Learned:** an interface that nothing consumes as a TYPE is
decoration — `Component` only started earning its place once
`render_all` and the page components held `multi Component`. And the
shop template DOES build and run — an earlier note in this repo had that
wrong, and wrong again about why: gap #1 (`pub` + `@table`) constrains
neither the build NOR the layout. A class crosses module lines without
export; only a free `fn` is module-scoped (`WO-E210`).

**Dependencies unblocked:** shop README gap #3 ("no multi-line
expression or literal") is closed. Separate `.html` templates, if ever
wanted, now have exactly one honest shape — a COMPILE-TIME include
feeding the raw-literal machinery; a per-request file read is the
already-rejected engine.

**Next steps:** the concurrency chain below is untouched by this and
remains the live queue.

**`.dev/reference` used:** none this slice (Angular's component format
was studied from its public docs during the 2026-08-23 story write-up;
no reference project was consulted for the implementation).

---

**The concurrency + fiber chain — ✅ stage 3 → ✅ 22 → 🔄 24 (absorbing
31 + 34) → 23 → 32.** The chain's original order put 31 before 24; the
2026-08-23 directive absorbed 31 INTO 24, and 34 resolved with it, so
those three are one slice. **Iteration 24 is nine of ten tasks landed and MERGED TO MASTER
on 2026-08-27** (fast-forward, `ed5334d`): T1 crypto, T2 bounded mailboxes,
T3 call/reply, T4 `monitor` + T5 `time.after` (ids 89/90 — the reserved holes
are now filled), T6 ws upgrade, T7 frame codec, T8 chat sample, T9 the chat
gate. Verified on master: chat 11 checks 0 failures at the full 1000-client
soak, runtime battery 36 suites 0 fail, compiler 556 checks 0 fail, corpus
119 checks 0 fail. Only **T10 closeout** remains — which is what still holds
stories 24/31/34 open. Finishing T9 exposed and fixed a real runtime bug,
split out as [40](language-runtime-database/40-shutdown-drain-guarantee.md). Its running state is the marker doc
(the marker doc, deleted at closeout per the convention),
which is the file to read for what is done and what is next; stories
[31](language-runtime-database/31-actor-lifecycle.md) and
[34](language-runtime-database/34-crypto-builtins.md) keep
`status: refine` until 24's T10 closeout sets all three to `status: done`
together.

**Implemented last time (2026-08-21):** **iteration 22 landed — the
measurement backbone exists and every performance claim is now
sourced.** `docs/examples/db-bench` + `scripts/db-bench.py` +
`bench/baseline.json` (74 metrics, tolerance-tuned by a two-run
repeatability check) + `just db-bench`/`db-bench-quick`; `time.ticks`
(µs monotonic clock, builtin 84) as the one runtime addition. Restart
proof + 3× kill -9 battery per shard count all green; the gate bites
(doctored results fail on exactly the doctored metric).

**Landed 2026-08-22 — the read-path index slice** (born from the
postgres study + 22's numbers, commit 6a306a7): engine `wo_idx_probe`
(bucket lookup, scan-identical verify) + emitter index selection
(`where var.col == key` lowers to DB_PROBE; guards stay the arbiter).
Reads 1.3k → **1.3M ops/s**, p50 600µs → 1µs (~×850); mixread 21 →
~1.9k ops/s multi-shard. Baseline refreshed; tolerance policy moved
into the driver (refresh-proof); gate proven to bite on both classes.
Pinned by corpus `query-index-probe` + a `wo_idx_probe` unit suite.

**Key findings (measured, not asserted):** durable seed ≈4.5k
inserts/s vs ram ≈297k/s — the 66× fsync gap IS iteration 23's case;
point lookups WERE O(table) (fixed 2026-08-22, above); mixread was 1,280 ops/s single-shard vs 21
ops/s multi-shard — the DB-actor price under O(table) probes and owner
serialization; msgrate 13.4M msgs/s same-heap vs 2.45M cross-shard —
deviation 4's mutex-inbox number (rings stay unearned until this is
the bottleneck). Standing bug found: hand-built `multi <TableClass>`
SEGVs on drop (elements classed OWNED; refs are scalar ids).

**Learned:** benchmark tolerances must be per-class — mix* spreads 50%
run-to-run (scheduling), read/query jitter ~25%, seed/write/msgrate
hold at 15%; a RAM store dies with its process, so throughput modes
share one run (`all`); WO_DATA on tmpfs makes fsync free — durable
numbers need a real disk.

**Dependencies unblocked:** 23 (has its fsync baseline to beat), 31
(has the mutex-inbox number), 32 (has the restart/replay timing
machinery), and every future optimization (the gate that catches
regressions is live).

**Next steps:** finish 24 (T4 `monitor` id 89, T5 `time.after` id 90 —
both still literal holes in `wob.h`'s builtin enum; then T8 the chat
sample, T9 its gate, T10 closeout setting 24/31/34 to `status: done`) → 23
(io_uring group-commit — target: close the 4.5k→297k durable gap) →
32 (WAL checkpoint). Held tail resumes on its own precedence notes.
> (**Superseded 2026-08-28:** 24 landed, and 23's part A landed with it —
> "close the 4.5k→297k durable gap" turned out to be the wrong target; see
> the databasev2 4 row.)

**`.dev/reference` used:** none this slice (the LW_SOAK discipline and
linkcheck.py precedent came from in-repo scripts).

---

### Landed 2026-08-20 — framework v1 (the previous NEXT PLAN)

**Framework v1 — a polished micro-framework (routing, middleware,
`Req`/`Resp`), nothing MVC-scale.** Directive 2026-08-20: iteration 17
(library kind + `internal/`) is **parked** — spec + plan approved and ready
on branch `library-internal` — and the framework itself is the work. The
v1-polish slice landed the same day (branch `framework-v1`): registration
helpers `get/post/put/delete_` (the take-Handler shape, probe-proven),
405 + `Allow` on wrong-method hits, HEAD served as GET with the body
suppressed, a `Logging` middleware, `set_header`; `just web-app` grew to
16/0. En route it exposed and fixed a real emitter bug: a Text-typed
single-segment interpolation of a place (`"${r.method}"`, `r` a loop
borrow) crossed `let`/assignment boundaries uncopied — aliased the field,
crashed the release build; `copy_place_text` now sees through `Interp`
exactly as `drop_fresh_text` does, pinned by
`tests/corpus/run/interp-borrowed-field`.

**Auth-in-core landed 2026-08-20** (same branch): `http/auth.wo` — header
parsing, pure-`.wo` base64, constant-time `ct_eq`, `req.principal` as the
blessed principal slot (Middleware.before takes `mut req`), `BearerAuth` +
`BasicAuth` middlewares; policy stays app-side. Probe matrix 26/26
ASan-clean; web-app dogfoods BearerAuth. The framework README carries the
core checklist (✅ / candidate / parked-by-design rows).

**Form-encoded bodies landed 2026-08-20** (same branch): `media_type(req)`
+ `form_values(req)` (nil on any other content-type; '+'/%XX decoded);
CreateProduct accepts form OR JSON into one insert path.

**Multipart landed 2026-08-20** (same branch): `http/multipart.wo` —
RFC 7578 fields + file parts, strict malformed-is-nil, `part_named`;
whole-body within BODY_MAX (streaming parks behind 8/11). CreateProduct
takes multipart/form/JSON; `just web-app` **21/0**. Surfaced + fixed the
RETURN flavor of the interp-of-borrowed-place emitter bug (emit_return now
sees through Interp; same corpus pin). Body-parsing hooks: all three ✅.

Scope split (2026-08-20): the surface above plus the remaining transport/
routing/security gaps is **framework v1**, tracked item-by-item in the
[framework README's status ledger](../examples/porch/README.md)
(✅/🔶/⬜/⏸/🔧 per feature — timeouts and Unix sockets need `net` runtime
seams, crypto hashes need C builtins since the language has no bitwise
operators, streaming/cancellation park behind 8/11). The memory-rich
features are **framework v2** = iteration 18 (⏸ HELD 2026-08-21 with spec
approved + plan authored intact): TTL cache, @table flags, durable job
queue with drain-on-request, `transaction { }` over the WAL's staged
batch. The pending order is now the concurrency chain (see *Pending*
below). Edges: [00-dependency-graph.md](../00-dependency-graph.md).

---

### Landed 2026-08-15 — the executable milestone (the previous NEXT PLAN)

"Make log-watcher executable" — the difference between "it runs" and "you
can leave it running". Every item came from a measurement on the sample
itself, and all six landed:

1. ~~The ownership pass does not know what the stdlib returns~~ — **done
   2026-08-14**. The root cause was deeper than the table: `Text` was
   classified Copy, so no Text local was ever dropped. `Text` is now an owned
   heap value that is **copied at every ownership boundary** (container, field,
   return, binding, loop cursor), the ownership pass reads the stdlib, builtin
   and static tables, and `fs.read_all`/`net.read` no longer mis-size a short
   read's buffer. Measured: `run` **1 051 040 B → 2 112 B**, `watch`
   **128 B → 64 B**; what remains is items 2 and 3 below, by stack.
2. ~~A projected temporary is never dropped~~ — **done 2026-08-14**. The
   projection was one of six shapes with no owner: a call result compared
   against `nil`, an argument the callee only borrows, a container read's
   copy, a loop's iterable, a projected record, and any of those escaped by a
   `return` from inside the statement that built them. Measured: `run`
   **2 112 B → 64 B** and flat from 8 s to 20 s, the full MCP mix
   **21 312 B / 63 → 64 B / 1**, every handler flat from 2 to 6 requests. The
   64 bytes left are item 3, on every path.
3. ~~The runtime leaks its own argv container~~ — **done 2026-08-14**. The
   entry only borrows its arguments, so `main.c` releases the container it
   built, after the entry returns and after a trap alike. **All three modes
   now report ZERO leaks under ASan** — `watch`, `run`, and the full MCP mix —
   which is the clean baseline item 6's soak needs to read against.
4. ~~A stopping program does not stop~~ — **done 2026-08-14**. A blocking
   call that parks (`net.accept`, socket read/write, `time.sleep`, a child
   wait) now ends the program when it is interrupted with the stop flag set,
   instead of restarting the syscall. A stop is not a trap: `try` cannot
   swallow it, and the stack unwinds through the same drop machinery, so the
   exit is clean and leak-free in every mode. It also uncovered a real
   double-free: an **assignment** of a Text place was a move, not a copy, so
   `api_key = j.mcp.apiKey` aliased the record — `let` copied, assignment now
   does too. `just log-watcher` is 7 checks; the seventh is the stop.
5. ~~The MCP server never closes an accepted connection~~ — **done
   2026-08-15**. `net.close` on every path out of a serve iteration (and the
   listener on stop). Measured: 4 → 4 descriptors across 200 requests, was
   one leaked per request.
6. ~~Nothing soaks~~ — **done 2026-08-15**. `LW_SOAK=<seconds>` drives all
   three modes under load and fails on resident growth past 256 KiB or any
   descriptor growth. The soak immediately caught what every seconds-long
   check missed: ~1.6 MiB/min of **in-arena** leaks the ASan report cannot
   see (the arena is one allocation to LeakSanitizer). Five bugs fell out:
   json decode's worst-case string sizing (free lists poisoned by relabeled
   lengths), `!=` never dropping fresh operands, Int interpolation segments
   mistaken for borrows, `json.encode(Ctor{...})`'s unowned argument, and
   discarded statement results (`pop(lines);`). After: release soak 30 s per
   mode — watch 0, run 0, mcp +20 KiB, descriptors flat; ASan build flat at
   14 600 KiB across 601 686 requests in 90 s once past its ~1200-request
   quarantine warm-up.

Plan: [`plan/compiler/2026-08-14-logwatcher-executable.md`](../plan/compiler/2026-08-14-logwatcher-executable.md) ·
Story slice: [`docs/stories/language-runtime-database/07-logwatcher-proof.md`](language-runtime-database/07-logwatcher-proof.md)

**Deferred by name, with the measurement that says so:**

- Iteration 5's *strictness* half — **`?T` forced handling landed 2026-08-18**
  (WO-E211/212/213 + local narrowing; the samples were updated to the
  bind-then-narrow idiom and stay green), **reject rows landed 2026-08-18**
  (WO-E105 doctrine diagnostics — `super.f()` used to compile clean). Still
  open: `pub(read)` write enforcement, `using`, `#if`. Plan 8 stays open.
- Everything `@gc`: iteration 7b, `set`'s `@gc` retention gap, iteration 4's
  `gc/held-cycle` leak. The sample declares **no `@gc` class** — 35 classes,
  none with the gc flag, 0 `RC_INC`/`RC_DEC` against 78 `DROP`s — so none of it
  can affect this workload.
- Iterations 8–12 (shard-actor runtime, database engine, `@table`/query, HTTP
  layer, fibers, blue-green): unchanged, and unblocked by this plan.

The **language track**'s first goal — iterations 3 → 4 → 5 → 6 → 7, _compile
and run log-watcher_ — is met; the database engine (9/9b), deps (15), and the
web framework (16) landed on top of it. The goal is now the framework as a
polished micro-framework (17 parked; see the NEXT PLAN above and
"Implementation order" under Pending). (The prior Rust `wo` runtime was
removed from the repo 2026-08-18 — see [`discarded.md`](../plan/discarded.md).)

---

## Stories

[`docs/stories/language-runtime-database/`](language-runtime-database/00-story.md)
— one language, one runtime, one database, one binary. Twelve iterations, each
an unsplittable slice with Given/When/Then acceptance and a pointer to the plan
that sequences its tasks. Read one, approve, then the next starts.

| #   | Iteration                                                                                    | State                        |
| --- | -------------------------------------------------------------------------------------------- | ---------------------------- | ---- |
| 1   | [Principles doc](language-runtime-database/01-principles-doc.md)                     | ✅                           |
| 2   | [VM core (`wovm`)](language-runtime-database/02-vm-core.md)                          | ✅                           |
| 3   | [Compiler front (`woc`)](language-runtime-database/03-compiler-front.md)             | ✅ (known gaps below)        |
| 4   | [Single binary end-to-end](language-runtime-database/04-single-binary-e2e.md)        | ✅ (known gaps below)        |
| 5   | [Language surface](language-runtime-database/05-language-surface.md)                 | 🔄 grammar done; **`?T` forced handling ✅ + reject rows ✅ + WO-E205 ✅ (2026-08-18)**; `pub(read)`/`using`/`#if` still ⏸ |
| 6   | [Program mode + stdlib](language-runtime-database/06-program-mode-stdlib.md)         | ✅ (the surface log-watcher uses) |
| 7   | [log-watcher proof](language-runtime-database/07-logwatcher-proof.md)                | ✅ **landed 2026-08-15** — executable, not merely compilable: zero ASan leaks in all three modes, SIGTERM ends parked syscalls, fds flat, `LW_SOAK` gate; `just log-watcher` 7/0 |
| 7b  | [Inferred GC + mark-sweep](language-runtime-database/07b-inferred-gc-mark-sweep.md)  | ✅ **landed 2026-08-18** — `@gc` gone (WO-E104), GC-ness inferred, RC replaced by incremental mark-sweep, `.wob` v4; supersedes iteration 2's RC memory model |
| 8   | [Shard-actor runtime](language-runtime-database/08-shard-actor-runtime.md)           | ✅ **landed 2026-08-21** — the arc complete: stages 1+2 (fibers/budget/actors/io_uring plane, shards, envelopes, WO-E222) + stage 3's transparent DB actor (`just db-actor` 8/0, ASan/TSan clean, WAL replay pair) |
| 9   | [Database engine](language-runtime-database/09-database-engine.md)                   | 🔄 engine complete (storage/WAL/indexes/insert-update-delete); reads land with 9b |
| 9b  | [`@table`, relations, query](language-runtime-database/09b-table-relations-query.md) | 🔄 query surface + relations + FK done (branch query-surface); group-by parked |
| 19  | [Float + Bytes](language-runtime-database/19-missing-scalar-types.md) | ✅ **landed 2026-08-20** — `.wob` v5: Float constant tag, field kinds 6/7, opcodes 34-41 (IEEE-quiet f64), builtins 70-83. Full stack: literals, arithmetic, `@table` column, WAL bit-exact replay, json fractions in / shortest-round-trip out, `?Float` reserved-NaN nil, total-order index (NaN last, `-0.0` == `+0.0`), Bytes + base64. No implicit Int/Float mixing (WO-E201); `float`/`trunc` are the only bridges. Proof: web-app price is a real Float (`{"price":9.99}`), `just web-app` 23/0; corpus 103/0 |
| 11  | [Fibers](language-runtime-database/11-fibers.md)                                     | ✅ **landed 2026-08-21** with the arc (`just fibers` 10/0); fs-park re-scoped out of v1, disclosed in the story |
| 22  | [Durability, throughput, scale](language-runtime-database/22-durability-throughput-scale.md) | ✅ **landed 2026-08-21** — db-bench + baseline.json (74 metrics) + restart/kill -9 proofs both shard counts; durable 4.5k vs ram 297k inserts/s, reads O(table), msgrate 13.4M/2.45M |
| 31  | [Actor lifecycle](language-runtime-database/31-actor-lifecycle.md) | ✅ **LANDED 2026-08-27 inside 24** (directive 2026-08-23). All four mechanisms: `call`/reply with a typed scalar reply (`WO_B_CALL = 88`, WO-E226), bounded mailboxes (`WO_MAILBOX`, cap 1024, catchable `WO_T_ACTOR`), actor death that traps callers instead of hanging them, **`monitor` (89)** and **`time.after` (90)** — the reserved holes in `wob.h` are filled. A fifth mechanism it did not anticipate came out of proving the gate: the shutdown drain guarantee, [40](language-runtime-database/40-shutdown-drain-guarantee.md). Supervision trees stay out of v1 |
| 24  | [chat: WebSocket workload](language-runtime-database/24-chat-websocket-workload.md) | ✅ **LANDED 2026-08-27** (absorbing 31 + 34) — all ten tasks; merged to master `ed5334d`. `just chat` **11 checks, 0 failures** at the full 1000-client soak: handshake, functional matrix on both `WO_IO` backends and on one shard, the soak, the fd invariant, the SIGTERM drain, `WO_MAILBOX=8` backpressure, ASan clean. Finishing its gate found a real runtime bug, split out as [40](language-runtime-database/40-shutdown-drain-guarantee.md) |
| 23  | [io_uring group-commit](databasev2/04-io-uring-commit.md) | ✅ **part A LANDED 2026-08-28 — group commit**, one barrier per drain instead of one per statement (the engine was fsync-per-STATEMENT, not per commit; the story's premise was wrong). Shard 0 holds each reply, commits once when its queue empties, releases all — so a writer is acked after the barrier carrying ITS record. **≈2.9× durable write throughput, ≈2.1× lower p50**, two measurement methods agreeing (2.9× controlled, 3.5× s1-vs-sN); mean batch 5.43, peak 57. A durability failure is now **fatal (exit 74), not a catchable `WO_T_IO`** — replacing three behaviours that disagreed, two of which admitted leaving RAM ahead of disk. **What it did NOT do:** `durable.sN.mixwrite` 480→492 (unchanged — that workload does 20 writes at C=4, mean batch 1.01) and `seed` unchanged (serial writers have nothing to batch with). **This row used to say "close the 66× gap"; that target was mis-stated** — the gap is two problems and part A fixes only the concurrent one. ⬜ part B (io_uring) **needs re-brainstorming**, not starting on the old premise |
| 32  | [WAL checkpoint](databasev2/03-wal-checkpoint.md) | ✅ **LANDED 2026-08-29 — the chain's last link.** Compaction rewrites the log as one record per live row and swaps it in with `rename`, so **recovery is completely unchanged** and crash safety comes from the filesystem rather than from code. **2.16× space reclaimed** (1 962 358 → 907 094 B), **boot 114 → 64 ms**, stop-the-world pause **2 651 µs** against a stated 50 ms budget. Read `.dev/reference/postgresql` for it: PG *never* compacts its WAL — its records are page deltas, so it needs heap files, a control file, a redo pointer and a separate process. Ours are full row images, so a compacted log IS a store, which deletes all of that. `kill -9` during compaction: 40 rounds/run, 10 clean runs, and **mutation-proven** — against in-place rewrite instead of `rename` the battery fails every time. Outstanding: the **`resident: keys` offset map** (compaction moves every record; the obligation is recorded at the compactor) and the O(live rows) pause, ~5.5 s at 1 GB, which is what an incremental design must be bought against |
| 33  | [Single-file store](databasev2/07-single-file-db.md)            | ⬜ off-chain, small — `WO_DATA=<path>.db` file form; driver-only (story written 2026-08-22) |
| 34  | [Crypto builtins](language-runtime-database/34-crypto-builtins.md)            | 🔄 **code landed** as 24's T1 (`d14fa9f`): `sha1`/`sha256`/`hmac_sha256`, ids 85–87 in `wob.h`, `runtime/src/crypto.c`, RFC/FIPS vectors 18/0, corpus pin. The 24 gate that once needed it is cleared. Frontmatter keeps `status: refine` only until 24's T10 closeout sets it to `done` |
| 38  | [Content platform capabilities](language-runtime-database/38-content-platform-capabilities.md) | ⬜ off-chain, needs a spec — the two capability families no iteration owns, confirmed against `runtime/src/wob.h`: `fs` mutation verbs (six fs builtins, ids 40–45; `append` creates-if-absent, so nothing is ever replaced, truncated, deleted or renamed) and `net.connect` (ids 51–55 + 91–95, no connect, and no `connect()` anywhere in `runtime/src/` — so no OIDC/SMTP/object-store/webhook/federation). Driven by a `docs/examples/vault` content-collaboration workload, in 28's mould. New builtins from 96 (89/90 reserved for 31); no `.wob` bump (`WOB_VERSION 6u`, last moved by 36). Story written 2026-08-26 from the "can it build a Nextcloud?" ask |
| 39  | [Web framework parity](language-runtime-database/39-web-framework-parity.md) | ⬜ off-chain, needs a spec — from [the Fiber v3.5.0 study](../plan/exploration/fiber/00-fiber-parity.md) (all 32 of its middleware read against `porch`; **nine already have a counterpart**). Leads with a **random-bytes builtin**: the framework ledger claimed CSRF/sessions were unblocked by iteration 34's HMAC, but HMAC authenticates a token and cannot mint one — there is no RNG anywhere in the runtime. Then cookies (absent both ways; `Resp.headers` being a map cannot carry two `Set-Cookie` lines), then limiter/idempotency (cheapest wins — `@table` + `time.ticks`, nothing new), sessions, CSRF, and the routing/response sugar. Streaming/SSE/compression, `@derive` binding, TTL cache, `proxy` and metrics all excluded with owners named |
| 40  | [Shutdown drain guarantee](language-runtime-database/40-shutdown-drain-guarantee.md) | ✅ **LANDED 2026-08-27 — chain 3, with 31; split out of 24.** One rule: **a message sent before the stop flag is observed must be delivered and run before the engine stops.** Found by measurement, not review: making the chat gate's drain leg start its OWN (cold) server exposed that **5 of 16** fresh-server SIGTERM drains left a WebSocket client at EOF with no close frame and no diagnostic. Traced to `shard_main` — `NEXT_RUNNABLE()` already stated the contract ("a WORKER on stop keeps DRAINING … close frames!") but the IDLE branch reaped and broke, abandoning its inbox for teardown to free. An actor between messages is exactly that idle case, which is why a WARM soak server hid it for so long. Fix is one branch honouring the primary's drain window, yielding on an empty poll. **20 of 20 clean after**; `just chat` 11 checks 0 failures at the full 1000-client soak (which also settled the fd question: 1000 connections left the count at 44); runtime battery 36 suites 0 fail, compiler 556 checks 0 fail. Ruled out: a bigger spin (a 1 s wall-clock deadline still failed 2 of 12) and spawn-during-shutdown. Outstanding: a pin below the gate — nothing in `runtime/test/` drives the engine start/stop and no corpus fixture can trigger a stop |
| 37  | [wo-html components](language-runtime-database/37-wo-html-components.md) | ✅ off-chain — LANDED 2026-08-25. Raw text literal (backtick, margin stripped at lex time, `{{ }}` auto-escapes) + the component layer: `Component`/`render_all`/`Layout` in wo-html, `ok_html` moved into the framework, site and shop both migrated |
| 35  | [net runtime seams](language-runtime-database/35-net-runtime-seams.md)            | ⬜ off-chain — fd deadlines on the park plane, Unix sockets, peer address; owns the ledger's three 🔧 rows (story written 2026-08-22) |
| 25  | [HTTP service layer](../superpowers/plans/2026-08-01-http-service-layer.md)                   | ⏸ hold (2026-08-21) — story file removed; the plan doc remains |
| 26  | [Blue-green deploy](language-runtime-database/26-blue-green-deploy.md)               | ⏸ hold (2026-08-21)          |
| 28  | [skillhost host workload](language-runtime-database/28-skillhost-host-workload.md) | ⏸ hold (2026-08-21); gaps recorded (branch query-grammar found skillhost needs no new query grammar) |
| 29  | [Compile-time metaprogramming](language-runtime-database/29-compile-time-metaprogramming.md) | ⏸ hold (2026-08-21)          |
| 15  | [deps: `wo.toml [deps]`](language-runtime-database/15-deps-package-manager.md) | ✅ **landed 2026-08-18** (branch web-framework): [deps] inline tables, git-binary fetch, wo.lock pinning, offline-when-locked, --update-deps, WO-E106/E107; `just deps-accept` 8/0 |
| 16  | [web framework](language-runtime-database/16-web-framework.md) | ✅ **landed 2026-08-19** — writeonce-framework (HTTP/1.1 + router + Handler/Middleware) consumed by web-app through [deps]; h2c parked (§C) behind 8/23/11. **v1 polish landed 2026-08-20** (branch framework-v1): get/post/put/delete_ helpers, 405+Allow, HEAD, Logging middleware, set_header; `just web-app` 16/0; fixed the interp-borrowed-field emitter crash en route. **Auth-in-core landed 2026-08-20**: http/auth.wo (Bearer/Basic, ct_eq, req.principal), web-app dogfoods BearerAuth, gate 17/0 |
| 17  | [library projects + `internal/`](language-runtime-database/17-library-projects-internal.md) | ✅ **landed 2026-08-20** — `kind = "library"` in `wo.toml` (default `program`, so every existing manifest is byte-identical; unknown value = WO-E109 exit 2); `woc <dir>` on a library runs the FULL pipeline entry-less and writes nothing, retiring iteration 16's `--emit` workaround; the no-entry build error names the kind; lib+bin dual works. Go's `internal/` rule as **WO-E108** at the consumer's own `use`, dep-boundary-only — the library imports its own interior freely. Framework reorganized: `internal/{parse,serve}.wo` behind the line, `http/form.wo` split out to keep `media_type`/`form_values` public. Driver-only change; VM/`.wob`/GC untouched. `just web-app` **26/0** (3 new checks), every standing gate unchanged |
| 18  | [framework v2: memory-rich features](language-runtime-database/18-memory-db-features.md) | ⏸ hold (2026-08-21); spec approved + plan authored, both held intact ([spec](../superpowers/specs/2026-08-20-memory-db-features-design.md), [plan](../superpowers/plans/2026-08-20-framework-v2-memory-features.md)): TTL cache + @table flags + durable job queue (drain-on-request) + `transaction { }` over the WAL's staged batch; pub/sub rejection expired with the arc (8/11 landed 2026-08-21) — revisit on unhold |

---

## In progress

| Track    | Item                                                                        | Where                                                      |
| -------- | --------------------------------------------------------------------------- | ---------------------------------------------------------- |
| Language | 🔄 [iteration 36 — operator parity](language-runtime-database/36-operator-parity.md): `not`, bitwise `& \| ^ << >>`, hex/binary/`_` literals, compound assigns — CODE LANDED 2026-08-22 (branch operator-parity, `.wob` v6, all gates green; reference project `.dev/reference/go` drove the design). Awaiting the developer's MANUAL pass on `docs/examples/operators/` (no test fixtures by directive); unblocks story 34's pure-`.wo` HMAC question | [plan](../superpowers/plans/2026-08-22-operator-parity.md) |
| Language | the framework v1-polish slice landed 2026-08-20 (branch framework-v1, awaiting merge); next per the order: brainstorm 20/21's forks | [order](#implementation-order-re-sequenced-2026-08-21--concurrency-chain) |
| Runtime  | ✅ **iteration 35 landed 2026-08-23** (branch `framework-v1b`, with framework v1 slice 2 + the serving slice): net deadlines/unix/peer (ids 91–95), fiber pooling, serve_conn + web-app fiber-per-connection — web-app gate 41/0, both WO_IO backends | [design](../superpowers/specs/2026-08-23-net-seams-park-design.md) |

**No slice is active.** Iteration 24 landed 2026-08-27 and its marker doc was
deleted per the convention. Everything pending is the concurrency chain (see
*Pending* below) — **the chain's next link is
[databasev2 4](databasev2/04-io-uring-commit.md)** (chain 5, the io_uring
group-commit write path, `was_language_iteration: 23`), which now has iteration
22's fsync-per-commit numbers in hand, plus databasev2 1's finding that the
write path is *not* where memory pressure bites (appending under a cap costs
~1%, random reads 273×). The held tail is every story whose frontmatter reads
`status: hold`.

### Landed 2026-08-14 — the compile-and-run milestone

One session, driven end to end by compiling `docs/examples/log-watcher` and
watching its diagnostic count fall (481 → 0). In order:

- **let annotations, container literals, statics, `pub(read)`** — `let x: multi
  Text = []`, `map<K, V>`, `?T`; `[]`/`[a, b]`/`{}` as expressions; `static
  const`/`static fn` with `Cls.fn(...)` calls; a `;` ends a statement so
  one-line guard bodies parse.
- **try/catch over the trap system** (plan 8 Task 5) — VM catch frames
  (`TRY`/`ENDTRY`), unwind-to-handler with the try region's own values
  released, `err_fill` for the `{code, line, method, msg}` record, expression
  and block catch arms. Uncaught traps unchanged.
- **`nil` + 23 text/container builtins** — len, byte_at, print_err,
  starts_with/ends_with, index_of/last_index_of, substr, trim, to_lower,
  char_of, parse_int, split/split_ws, join, slice, pop/shift, sort, reverse,
  remove, key_at/val_at, multi_set.
- **`for k, v in m`** over a map, and `m[i] = v` for a `multi`.
- **the systems stdlib's OS half** (`runtime/src/sysio.c`) — fs, time, env,
  net, proc behind the reserved module names, with predeclared `Stat`,
  `TimeParts` and `Proc` records and the new `WO_T_IO` trap.
- **json** (`runtime/src/json.c`) + **`.wob` v2** — per-field names, referenced
  classes and element kinds in the class table, so encode/decode are one
  metadata-driven implementation; `json.decode(t) as T` is the language's only
  cast, yielding `?T`.
- **program mode** — `fn main(args: multi Text) -> Int`, argv delivered by the
  runtime, return value as the exit code.
- **two safety fixes found by running it**: `+` on `Text` was lowering to ADD
  on two heap pointers (now WO-E201 pointing at `..`; seven sites in the sample
  were corrected), and `x == nil` was lowering to EQS, which dereferences the
  zero word (now EQ).

Gates at the end of that session: corpus 71/0, `woc` runtest 565/0, every
`wovm` unit gate green in both dispatch flavors.

---

## Done

### Language track — compiler + VM (OOP track)

| Status | Item                                 | Doc                                                              | What actually landed                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| ------ | ------------------------------------ | ---------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| ✅ | iteration 37 — wo-html components | [37](language-runtime-database/37-wo-html-components.md) | Two halves. **Grammar (2026-08-24):** the backtick raw text literal — content verbatim, common margin removed at lex time, `${ }` raw and `{{ }}` auto-escaping to a call on the `esc` in scope; WO-E004/WO-E005 added; lexer + one parser desugar only, nothing downstream. **Library (2026-08-25):** `Component`/`render_all`/`Layout` in wo-html, `ok_html` moved into `framework/http` beside `ok_text`/`ok_json`, site migrated onto `Layout` + a reused `ChapterNav`, shop onto `AppShell` + `multi Component` children with queries in the controllers; the site was then restructured onto the program template's MVC layout (model / layout / view modules / one controller per feature / bootstrap-only main). `multi Component` needs no wrapper record — the framework's Mw/Aw shape is not a language requirement. Gates: `just site` 11/0, `just web-app` 46/0, `woc-test` 556/0, `oop-e2e` 116/0 |
| ✅     | Principles                           | [`../00-principles.md`](../00-principles.md)                        | 13 principles, each with a why and a link to the doc that enforces it                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| ✅     | `wovm` VM core                       | [plan 1](../superpowers/plans/2026-08-01-wob-format-and-vm-core.md) | `.wob` v1 loader with full static validation, register interpreter (computed-goto + ISO-C fallback), arena with size-class free lists, borrow word, RC + budgeted Bacon–Rajan cycle collector, drop-map trap unwinding, containers, builtins, ICALL, CLI. 13 suites × 2 dispatch flavors + CLI smoke, ASan/UBSan clean                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| ✅     | `.wob` format contract               | [`oop-vm/00-wob-format.md`](../plan/oop-vm/00-wob-format.md)        | Normative; twinned with `runtime/src/wob.h`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| ✅     | `woc` compiler front                 | [plan 2](../plan/compiler/2026-08-01-woc-compiler-front.md)         | Tasks 1–8: dune scaffold, `diag` (WO-E codes, two-site related errors, ordered dedup), newline-significant lexer at rt parity, declaration + statement/expression parser with skip-on-block and multi-error recovery, typechecker (field kinds, `?T` plumbing, W201, E225, E214), MVS ownership pass with the four emitter tables, driver with directory discovery + cross-file programs. 14 + 264 checks                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| ✅     | Error catalog                        | [`oop-vm/01-error-catalog.md`](../plan/oop-vm/01-error-catalog.md)  | 14 emitted codes + 10 reserved, each with the reason it is not yet emitted                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| ✅     | log-watcher `.wo` sample             | [`../examples/log-watcher/`](../examples/log-watcher/README.md)     | Eight-file port authored docs-first with its `.hx` mapping table; compiles for real in iteration 7                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| ✅     | Scalar cleanup                       | [`discarded.md`](../plan/discarded.md)                              | `Money`/`SKU`/`Float` and the abstract allowlist removed; `abstract` flipped adopt → reject                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| ✅     | `woc` emitter, corpus, single binary | [plan 3](../plan/compiler/2026-08-01-wob-emit-e2e-single-binary.md) | Tasks 1–6 + 8 (Task 7, a parity harness against the Rust runtime, **deferred by explicit user decision** — the two stacks diverge by design). Bytecode emitter (`emit.ml`) + disassembler (`disasm.ml`, `--dump-bc`); three-kind conformance harness (`scripts/oop-e2e.sh`, `just oop-e2e`) over `tests/corpus/{run,compile-fail,trap,gc}`; pricing-demo + ownership/trap corpora (19 fixtures); `@gc` cycle collector's post-exit pump (`WO_GC_BUDGET`/`WO_GC_TRACE`) + 2 gc fixtures (`gc/held-cycle` retired — see criterion-3 closure below); `woc build` single-binary output + relocation/corrupt-trailer smoke; `WO-E405` closing criterion 3's ASan leak (entry must return `Int`); `just oop-accept` wiring all five spec criteria + both unit gates into one command. 14 + 399 compiler checks; `oop-e2e` 25/25 against the release `wovm`. **Milestone-1 acceptance gate is fully green — all five criteria met** (see the dated acceptance note in `docs/superpowers/specs/2026-08-01-oop-compiler-vm-design.md`) |

**Known gaps carried out of iteration 3** — recorded, not silently owed:

- **`?T` is plumbed but unenforced.** Lexer/token/AST/parser/dump all handle
  `?T`; the semantics do not exist (`WO-E211`/`E212`/`E213` declared, never
  emitted — a probe returning `?Int` as `Int` exits 0). Owned by iteration 5,
  plan 8 Task 6, which is that iteration's first task because it blocks the
  log-watcher port. See [`compiler/nullable-types-implementation.md`](../plan/compiler/nullable-types-implementation.md).
- **Structural interface satisfaction is not checked** (`WO-E205` dead), along
  with type mismatch, bad arity, and unknown-fn (`E201`/`E203`/`E204`) — all
  named in plan 2 Task 6's own must-fail list. Gaps in shipped work, catalogued
  as reserved.
- Six further narrowings (W201 heuristic, E225 reach, dead code after `return`,
  unresolved-callee drops, RC table ordering, residual b-side role) are listed
  in the plan-2 SDD ledger and in the affected files' own comments.

**Known gaps carried out of iteration 4** — recorded, not silently owed:

- ~~`WO-E205` (unsatisfied interface) reachable but unenforced~~ — **closed
  2026-08-18** (branch `type-enforcement`): structural satisfaction is checked
  at call arguments, annotated `let`s, and returns; the pinned fixture moved
  to `compile-fail/unsatisfied-interface` with `fixture.code WO-E205` in the
  same change, as its comment demanded. The hybrid boundary is restored.
- **`set(m, k, v)`'s `@gc` retention gap on map keys/values is open** — the
  twin of the `push` bug Task 5 fixed for `multi`. `set` has no equivalent
  special case in `owner.ml`'s `analyze_call`, so a `@gc` key or value handed
  to `set` is under-counted and the collector can free it while the map still
  points at it. Nothing in the corpus exercises this yet. See
  [`oop-vm/08-builtin-surface.md`](../plan/oop-vm/08-builtin-surface.md).

**Known gaps carried out of the 2026-08-14 compile-and-run milestone** —
recorded, not silently owed:

- **Optionals are lenient.** `?T` has its representation (the zero word) and
  its comparisons, but `WO-E211`–`E213` are still dead: a `?T` may be used
  where `T` is required, and nothing narrows inside an `if x != nil` branch.
  The workload leans on that leniency today.
- **`pub(read)` is parsed, not enforced.** The marker rides on the field
  (`Ast.field.pub_read`); no check refuses a write from outside the declaring
  class yet.
- **`using` extensions and `#if` build flags are absent**, and the reject rows
  (`extends`/`cast`/`Dynamic`/…) still have no doctrine-citing diagnostics —
  plan 8 Tasks 7–8's remainder.
- ~~A borrowed non-constant Text pushed into a container is a double-free
  hazard~~ — **closed 2026-08-14 by copy-on-push**: `push`/`set`/`m[i] = v`
  copy a TEXT element, key or value into the container, and the compiler drops
  a *freshly built* Text right after the call (a value read out of a place
  keeps its owner). The failure it fixed was real: a `tools/call` of `tail_log`
  used to answer `{"isError":true,"text":"tool failed: not a text value"}`; all
  four MCP tools now return `isError:false` with correct payloads.
  `OWNED`/`GCREF` elements still move, and `set`'s `@gc` retention gap is still
  open (see [`oop-vm/08-builtin-surface.md`](../plan/oop-vm/08-builtin-surface.md)).
- **A blocking `accept`/`read` swallows SIGTERM.** `env.stopping()` installs a
  handler that only sets a flag, and `net.accept`/`net.read` retry on `EINTR`,
  so a server parked in `accept` never observes it: a plain TERM does not stop
  the process (`timeout -k` / `kill -9` does). Graceful shutdown needs an
  interruptible wait — the shard-actor runtime's event loop (iteration 8) is
  where that belongs, not a patch to the blocking calls.
- **A temporary record whose field is iterated is never dropped** —
  `for e in parse_dir(dir).entries` keeps the entries alive (good) but leaks
  the `ParseResult` shell (its drop is recorded for no register). Found in the
  same disassembly; a leak, not a corruption.
- ~~json's two documented limits~~ — **closed 2026-08-18** (branch
  `json-fidelity`): a `Bool` field encodes `true`/`false` (WOB_FIELD_BOOL /
  WOB_FIELD_NIL_BOOL in the field metadata), and a fraction/exponent is
  malformed for an Int field — the checked decode yields nil instead of
  truncating (floats stay representable via a raw `json.Value` field).
- **`net` fd lifetime is the program's problem.** `net.close` exists; the
  sample's MCP server never calls it, so a long-running `mcp` session leaks
  descriptors. That is the sample's bug to fix, not the runtime's.
- **The workload has never run under ASan**, and iteration 4's `gc/held-cycle`
  leak (above) is still open. The corpus itself stays ASan-clean.
- ~~`json.encode` Bool/nil-scalar asymmetry~~ — **closed 2026-08-18** with the
  same change: `Bool` encodes `true`/`false`, `?Bool` nil encodes `null`.
- **No corpus fixtures cover the new surface.** By explicit direction
  (2026-08-14) the acceptance for this work is the log-watcher program itself,
  not fixture pairs; `tests/corpus/` still gates every pre-existing behavior
  (71 checks, 0 failures).
- **E201/E203 and seven other `WO-E2xx` codes remain declared but unemitted**
  — see [`oop-vm/01-error-catalog.md`](../plan/oop-vm/01-error-catalog.md).
- **CLOSED — milestone-1's ASan gate (`just oop-accept`) failing on
  `gc/held-cycle`.** Root cause (Task 8's finding, restated): `main.c`'s
  entry-method return value (`uint64_t ret`, `src/main.c:158`) is stored
  but never released, so `gc/held-cycle`'s "permanent external hold" was
  actually a permanent refcount inflation — LeakSanitizer's "definite
  leak" (1184 bytes / 3 allocations) was correctly reporting exactly
  that, not a false positive. Fixing it by releasing `ret` was rejected:
  the `.wob` method table carries no return-type/kind metadata, so
  `main.c` has no way to know `ret` is a pointer rather than a scalar,
  and adding that metadata is a format change out of scope here. Fixed
  instead at the source: the systems-track spec already requires the
  entry to return `Int` (its return value is the process exit code), so
  a class-returning `main` was never legal — `WO-E405`
  (`compiler/src/emit.ml`, `01-error-catalog.md`) now rejects it at
  compile time, and `gc/held-cycle` is retired because its premise (an
  externally-held cycle survives a _post-exit_ pump) is no longer
  expressible — see `oop-vm/02-corpus.md`'s "Retired" note for why, and
  for where the scenario it meant to cover is actually proven
  (`runtime/test/test_cycle.c`, plus a proper in-flight fixture scheduled
  for story iteration 7b). Spec success criterion 3 is now **MET**;
  `just oop-accept` passes all five criteria.

The C proving-ground work (`exploration/c-runtime/`, phases A–F: 859k reads/s,
618k durable commits/s) fed the current C runtime and remains as an
[exploration study](../plan/exploration/c-runtime/00-plan.md).

---

## Pending

Measured optimization candidates live in
[`docs/plan/perf-targets.md`](../plan/perf-targets.md) — a register
like discarded/learnings: a target enters with a number, leaves by
landing (baseline delta) or by rejection into discarded.md.

### Implementation order (re-sequenced 2026-08-21 — concurrency chain)

Everything still pending IS the runtime-concurrency chain. Basis: the
2026-08-20 code-review pass (measure before optimizing, close correctness
holes before adding surface), amended 2026-08-21 by developer decision:
**stage 3 before 22** — correctness first, then one benchmark campaign
covers single- and multi-shard. The authoritative table with per-row
reasoning is [`00-story.md`](language-runtime-database/00-story.md).

Dependency rules that force the shape: 23 after stage 3 + 22 (the ring is
the arc's, the baseline is 22's); 24 after 31 (chat is dishonest without
lifecycle); h2c stays parked behind the chain; the held tail keeps its own
precedence notes for resumption.

1. ✅ **8+11 stage 3** — landed 2026-08-21 (`just db-actor` 8/0; arc
   complete, stories in done/). Was: transparent DB actor. A correctness fix, not an
   optimization: worker VMs are zero-initialized, so a DB statement off
   the primary traps `WO_T_DB` — a multi-shard program touching the
   database is broken today. Plan of record:
   [`2026-08-20-shard-fiber-arc.md`](../superpowers/plans/2026-08-20-shard-fiber-arc.md)
   (stages 1+2 landed 2026-08-20, branch `concurrency-arc`).
2. ✅ **22** — LANDED 2026-08-21 (`just db-bench`, baseline committed,
   gate bites; headline: durable 4.5k vs ram 297k inserts/s, reads
   O(table), mixread 21 ops/s multi-shard, msgrate 2.45M cross-shard).
   Was: the measurement backbone: restart-persistence proof + baseline
   benchmark (durable + RAM-only), single- AND multi-shard in one
   campaign, plus the stage-2 mutex-inbox number (rings only if the mutex
   costs). It has never run — no `bench/baseline.json`, no `just db-bench`;
   the arc's stages 1+2 delta is recorded retroactively.
3. **31** — actor lifecycle
   ([story](language-runtime-database/31-actor-lifecycle.md),
   written 2026-08-21): request/response (`send` is one-way and callers
   `sleep` to await), bounded mailboxes (the FIFO only grows), actor
   death/supervision, timers beyond `time.sleep`.
4. **24** — chat, the arc's acceptance; honest only after 31 (19 landed
   2026-08-20 — Bytes carries the frames).
5. **23** — io_uring group-commit; the WAL's WRITE+FSYNC chains ride the
   arc's per-shard ring (T4); after 22's baseline — the payoff, measured.
6. **32** — WAL checkpoint
   ([story](databasev2/03-wal-checkpoint.md),
   written 2026-08-21): the WAL is append-only forever — snapshot +
   truncate reclaims disk and bounds replay; after 23 (composes with
   group-commit), policy set by 22's aged-store numbers.

**30** — observability, CI, fuzz: named 2026-08-20, still row-only (no
story file); slots in when scheduled — nothing in the chain depends on it.

### ▸ databasev2 — the database beyond RAM

New 2026-08-26. **The problem:** rows were resident unconditionally and nothing
declares a budget. Rows live in `malloc`'d slabs whose addresses are stable
forever; there is no eviction, spill or paging anywhere in `database/src/`; the
WAL never checkpoints so boot replays all history; and durability is one
process-global `WO_DATA`, so no table can say it matters more than another. An
allocation failure is a clean catchable `WO_T_OOM` **only in the VM arena** —
table storage has no ceiling and is SIGKILLed instead (measured, databasev2 1).
Where swap exists the ceiling may never announce itself at all: an append-mostly
900k-row run finished *at uncapped speed* inside a 64 MiB cap (148 s vs 150 s),
serving from disk with no error signal.

**The lever** is per-table storage modes, which is why this track has a grammar
iteration. Six pending iterations moved here from the language track (their old
ids in the rows below); four are new. Done database work — 9, 9b, 22 — stays in
the language arc as v1 history.

| # | Iteration | State |
| --- | --- | --- |
| 1 | [RAM ceiling: measure the breaking point](databasev2/01-ram-ceiling-measurement.md) | ✅ **MEASURED 2026-08-27** — `readiness: ready`, `status: done`; forks settled, harness landed (**148 checks**). Footprint **96.5–100 B/row** Int vs **320.6–324 B/row** text = **3.3×** (not the "order of magnitude" three docs claimed), read as median-of-marginals because doublings swing a two-point slope 2×. **Both predicted exits were wrong:** table storage has no checked ceiling and is **SIGKILLed** (overcommit lets `malloc` succeed, kernel kills on page touch), and swap is not latency collapse — 900k rows finished **148 s capped-with-swap vs 150 s uncapped**, ~1%, returning 0 while serving from disk. **Ack-after-fsync survives an OOM kill:** ~40 000 rows recovered as an intact prefix, gated as the `ceiling` leg. Also measured: **random reads over an oversized table collapse 273×** (1.85M vs 6 771 reads/s, p99 1 µs vs 487 µs) — so the two access patterns sit ~270× apart under the same pressure, and departure is a **step, not a curve**. Replay measured too: **≈5.5 µs/record, 1.9× history penalty** (10M records ≈ 55 s of boot) — iteration 3's missing "before", now gated. Iteration 2's budget dependency is **removed, not satisfied** — there is no "swap onset" to derive it from |
| 2 | [per-table storage: `durable` and `resident`](databasev2/02-table-storage-modes.md) | 🔄 **the language enrichment — the `durable` half is DONE and usable.** Two optional `@table` keys, `durable: true\|false` and `resident: all\|keys`, both defaulting to today's behaviour (all 28 existing declarations compile unchanged, no golden moved). Landed: the grammar, WO-E224 (a durable `ref` into a volatile table is refused), `.wob` v7 carrying both properties in spare `flags` bits, `durable: false` actually skipping the WAL (measured: 50 inserts → 1500 bytes durable, **0** volatile) with a mode-mismatch startup refusal, plus offset capture and read-a-row-from-an-offset. Outstanding: 5c/5d (the id→offset map and rewiring `wo_row_ptr`'s 11 call sites, slab scans and `@unique`/FK across the boundary — not yet written up), the two runtime refusals, and closeout. [spec](../superpowers/specs/2026-08-26-table-residency-design.md) · [plan](../superpowers/plans/2026-08-26-table-residency.md) |
| 3 | [WAL checkpoint](databasev2/03-wal-checkpoint.md) *(was 32)* | ✅ **LANDED 2026-08-29 — the chain's last link.** Compaction rewrites the log as one record per live row and swaps it in with `rename`, so **recovery is completely unchanged** and crash safety comes from the filesystem rather than from code. **2.16× space reclaimed** (1 962 358 → 907 094 B), **boot 114 → 64 ms**, stop-the-world pause **2 651 µs** against a stated 50 ms budget. Read `.dev/reference/postgresql` for it: PG *never* compacts its WAL — its records are page deltas, so it needs heap files, a control file, a redo pointer and a separate process. Ours are full row images, so a compacted log IS a store, which deletes all of that. `kill -9` during compaction: 40 rounds/run, 10 clean runs, and **mutation-proven** — against in-place rewrite instead of `rename` the battery fails every time. Outstanding: the **`resident: keys` offset map** (compaction moves every record; the obligation is recorded at the compactor) and the O(live rows) pause, ~5.5 s at 1 GB, which is what an incremental design must be bought against |
| 4 | [io_uring group commit](databasev2/04-io-uring-commit.md) *(was 23)* | ✅ **part A LANDED 2026-08-28 — group commit**, one barrier per drain instead of one per statement (the engine was fsync-per-STATEMENT, not per commit; the story's premise was wrong). Shard 0 holds each reply, commits once when its queue empties, releases all — so a writer is acked after the barrier carrying ITS record. **≈2.9× durable write throughput, ≈2.1× lower p50**, two measurement methods agreeing (2.9× controlled, 3.5× s1-vs-sN); mean batch 5.43, peak 57. A durability failure is now **fatal (exit 74), not a catchable `WO_T_IO`** — replacing three behaviours that disagreed, two of which admitted leaving RAM ahead of disk. **What it did NOT do:** `durable.sN.mixwrite` 480→492 (unchanged — that workload does 20 writes at C=4, mean batch 1.01) and `seed` unchanged (serial writers have nothing to batch with). **This row used to say "close the 66× gap"; that target was mis-stated** — the gap is two problems and part A fixes only the concurrent one. ⬜ part B (io_uring) **needs re-brainstorming**, not starting on the old premise |
| 5 | [Bounded tables and eviction](databasev2/05-bounded-tables-eviction.md) | ⬜ a declared capacity + refuse/evict/back-pressure, and a process-level pressure signal that sheds **before** the allocator or OS gets involved — turning the invisible failure into a managed one |
| 6 | [Cold tiering](databasev2/06-cold-tiering.md) | ⚠ **largely superseded by 2** — `resident: keys` took the ceiling-raising role; its user-space-working-set premise was rejected for the kernel page cache. Mostly forks: which shape, whether the index itself fits, whether the *language* surfaces the fault cost, and whether `@unique` on a cold table is refused outright. A paged B-tree stays rejected — if tiering needs one, reject tiering |
| 7 | [Single-file store](databasev2/07-single-file-db.md) *(was 33)* | ⬜ `WO_DATA=<path>.db`; driver-only, independent |
| 8 | [Query grammar from corpora](databasev2/08-query-grammar-corpus.md) *(was 27)* | ⬜ whole-query `count`, `exists`; independent |
| 9 | [Cross-program tables](databasev2/09-cross-program-tables.md) *(was 20)* | ⏸ hold — attach to a running program's database over local IPC |
| 10 | [Keypair attach auth](databasev2/10-keypair-attach-auth.md) *(was 21)* | ⏸ hold — program identity as a keypair; needs 9 |
| 11 | [Bounded delta chains](databasev2/11-bounded-delta-chains.md) | ✅ **LANDED 2026-08-30.** A `resident: keys` row's delta chain is bounded in the UPDATE path, because the checkpoint is blind to per-row chain length — it thresholds on whole-log bytes, so one hot row can grow an unbounded chain inside a log that never trips compaction. The fold now reports hop count (free — the walk already visited every hop), and past `WO_DELTA_MAX_HOPS` (16) the update writes a full row image instead of a delta, resetting depth to 0. **Two things the tests corrected.** The flattened image is a `WO_WAL_UPDATE`, not an `INSERT`: the row's original INSERT is already in a live log, so a second one for the same id is a duplicate that replay correctly refuses as corruption — INSERT is right only for compaction, which builds a *fresh* log. And the **proportional ceiling was removed as dead code**: with the absolute term at 64 MiB, garbage large enough to reach a 256 MiB ceiling has already tripped it, so the branch was unreachable. Borrowing both constants from postgres was the wrong inference — PG needs two because it thresholds on *tuples* with its pair at opposite ends (base 50, max 1e8); this thresholds on *bytes*, where one constant does both jobs. Found by trying to write a test for the ceiling and finding no input could reach it. Four tests: depth stays bounded across 2K+2 updates, a flattened chain replays, a delta on an **indexed** column composes with flattening (checked at every step across the bound and after restart — found no product defect), and the policy's absolute term with its boundary. `test_wal` **5700 pass / 0 fail**; `wovm-test` and `woc-test` green. **One criterion is weaker than written:** the replay check asserts an expected value, not a `resident: all` oracle table. [spec](../superpowers/specs/2026-08-30-bounded-delta-chains-design.md) |
| 12 | [Schema migrations](databasev2/12-schema-migrations.md) | ✅ **LANDED 2026-08-31.** A `@table` class is the schema, the log is the database, and boot now compares them — before this, an added or deleted field turned a healthy `WO_DATA` into "corruption" and reordering declarations silently decoded rows into the wrong class. Landed: `WO_WAL_SCHEMA` head record (written LAZILY ahead of the first real record — an eager head broke `durable: false`'s documented zero-bytes contract by 75 bytes and the gate caught it), a name-keyed diff whose refusals are per-class POISONS that bite only when a record of the class is met, and a record-level TRANSCODE: cids remap by name including inside stored owned values, deleted values freed, added fields zero-filled, delta back-pointers rewritten through an offset map with deltas on deleted fields SPLICED out; temp+fsync+rename, compaction's crash discipline. **Two bugs the tests forced out:** a poisoned class skipped plan identity so the retype refusal fell through to generic "corruption" (the message this iteration exists to replace), and early `goto corrupt` freed uninitialized memory. End-to-end: `migrating \`Note\`: +flag` then `flag=0`; retype refuses naming `val`, exit 2, old binary still boots the refused log. 21 new tests, `test_wal` **5966/0**; wovm/woc/site/residency gates green. v2 holds rename (`@renamed_from`), retypes, and data/seed migrations. [spec](../superpowers/specs/2026-08-31-schema-migrations-design.md) |

---

### ▸ porch — the web framework track

New 2026-08-26, from [the Fiber v3.5.0 parity study](../plan/exploration/fiber/00-fiber-parity.md).
Supersedes language iteration 39, now a pointer. **The whole track (2–8) is
`ready`** (brainstormed 2026-09-06, forks locked, validated against
`.dev/reference/fiber`). The three language touches the track needs are now
explicit and small, each a builtin with a named consumer: `random_bytes` (2),
`deflate`/`crc32` (7), `time.utc` (8). Ordered by dependency; the
first slice is deliberately the cheapest so the store pattern and gate shape are
proven before the runtime and `Resp` are touched.

| # | Iteration | State |
| --- | --- | --- |
| 1 | [Store-backed middleware](porch/01-store-backed-middleware.md) | ✅ **DONE 2026-08-30** — rate limiter + idempotency serialized through a per-key actor pool, both durable in a `@table`. Gate-proven end to end: threshold + restart + exact concurrent counts (limiter), byte-identical replay + digest refusal + concurrent duplicates + no-5xx-replay (idempotency), and pool saturation failing closed (503, never a bypass) |
| 2 | [Randomness and cookies](porch/02-randomness-and-cookies.md) | ✅ **`ready` 2026-09-06** — the foundation; the reference read settled that **exactly one language enhancement is needed**. Phase A is that language work: a bare-name `random_bytes(n) -> Bytes` builtin in the compiler's crypto-family table (`emit.ml` `b_*` + `types.ml` function list — **not** `wob.h`'s module enum; next free id `84`/`90`, confirm before use), `getrandom(2)`-sourced, refuses loudly. Then `Resp` gains `cookies: multi SetCookie` beside the unchanged `headers` map (the map can't emit two `Set-Cookie` lines; `multi` already exists), `Cookie:` parsing (structural 400 in `parse_request`, on-demand `cookie()` helper), and signed cookies (`base64(value).base64(mac)`, app-supplied key) |
| 3 | [Sessions](porch/03-sessions.md) | ✅ **`ready` 2026-09-06** — after 2. Six decisions locked: row is a pure auth primitive (id/principal/created_at/last_seen, no payload bag); **wall-clock `time.now`, not monotonic `time.ticks`** (sessions survive restart); rotation = login always mints a fresh id (no anon-session model); throttled `last_seen` touch at `idle/20` (not a WAL write per request); `Session` writes `req.principal`; config refuses absolute < idle. Pure `.wo` on iteration 2 + the `@table` engine — no new runtime work |
| 4 | [CSRF](porch/04-csrf.md) | ✅ **`ready` 2026-09-06** — after 2 + 3. Five decisions locked: fiber's **hybrid** transport (session-stored `CsrfToken` @table keyed by token + double-submit cookie, both must pass; no CSRF for sessionless apps); opt-in single-use (checkout the example, admin multi-use); a double-click yields a distinct `SPENT` refusal with **no coupling to the lang-41-blocked idempotency**; trusted origin/referer/`Sec-Fetch-Site` as the second layer; refusal classes distinguishable in logs, opaque in body. Plain `@table` CRUD — no actor pool, not blocked on lang-41 |
| 5 | [Routing + response ergonomics](porch/05-routing-response-ergonomics.md) | ✅ **`ready` 2026-09-06** — **independent, any time; NO upstream dependency (not even iteration 2)**. Five decisions: `head` auto-registers with an opt-out (+ `patch`/`options`/`all`); request ids mirror the limiter's trust model with a **non-crypto** source (so no CSPRNG dependency); per-route `body_limit` is a **second check after routing** (global `BODY_MAX` stays the pre-routing ceiling, over-limit = 413); adding `name`/`body_limit` to `Route` is corpus-free; `Vary` accumulates by comma-join. Plus named routes + runtime-checked URL building, q-value ranking (retires the 🔶) |
| 6 | [Streaming core](porch/06-streaming-core.md) | ✅ **`ready` 2026-09-06** — riskiest/highest-leverage; **re-scoped to outbound only**. Three decisions: separate `StreamHandler`/`BodyProducer` parallel path (the `Resp` path untouched → existing responses byte-identical); streaming routes **opt out** of the after-chain, framework **refuses at registration** to combine with header-mutating middleware (loud, never silent), handlers stamp headers via a `security_headers()` helper; chunked **REQUEST** bodies **split into their own future iteration** (parse.wo refusal stays). No language enhancement; rides the fiber loop, not the actor pool (not lang-41-exposed) |
| 7 | [SSE + compression](porch/07-sse-and-compression.md) | ✅ **`ready` 2026-09-06** — after 6 (+ 5 for q-ranking/comma-join Vary; NOT 2). Five decisions: refuse an incoherent heartbeat/`idle_ms` pair at construction; **codec = two C builtins `deflate`+`crc32`** (perf over pure-`.wo`; hand-rolled, no zlib dep; gzip framing in `.wo`) — the track's **second language dependency** after iteration 2; ETag over uncompressed bytes + `Vary`; `Last-Event-ID` explicitly unsupported (not silently ignored); Vary via comma-join. Codec is pure compute — not lang-41-exposed |
| 8 | [Static files + lifecycle](porch/08-static-and-lifecycle.md) | ✅ **`ready` 2026-09-06** — static half after 6 (+ 5's Download helper). Four decisions: three hooks (on-listen/on-shutdown/on-route-registered); healthcheck ships **both** `/livez`+`/readyz`; directory listing **off by default**, documented; **`Last-Modified` needs a small `time.utc(ms)->TimeParts` builtin** (gmtime sibling of time.local — the track's third, smallest language touch; time.local is local-tz, time.iso is UTC-but-ISO), IMS by string-equality (no parser). Byte ranges via `fs.read_at`+iteration 6 writer. Not lang-41-exposed |

---

⏸ **Held** (2026-08-21, developer decision): 18, 20, 21, 25, 26, 27, 28,
29 — every story carrying `status: hold` in its frontmatter (25's story
file removed; its
[plan doc](../superpowers/plans/2026-08-01-http-service-layer.md)
remains). Half-done branches (ipc-attach, keypair-auth) keep their
manifests.

✅ **17** — landed 2026-08-20 (unparked and executed): `kind = "library"`,
check mode, and the `internal/` dep boundary (WO-E108). Driver-only.
✅ **19** — landed 2026-08-20: Float + Bytes, `.wob` v5.

### ▸ runtime-v2 — the runtime beyond sockets

New 2026-09-01. The I/O plane learned sockets in 8/11/35 and files in 6;
this track adds the missing third — **processes, terminals, signals** —
first five builtin-sized seams, each `runtime/src/` work with a `types.ml`
row as its whole compiler cost (the iteration 42 precedent); the track then
grew a 6th (terminal measurement) and, 2026-09-06, a 7th and 8th (observability,
moved from language 30; a symmetric cipher) both `refine`. Iteration 42
(bounded subprocess, ✅ on `master` 2026-09-01) opened the arc from the
language track before it had a name. **All five `readiness: ready`** —
one track-wide brainstorm settled every fork
([spec](../superpowers/specs/2026-09-01-runtime-v2-design.md),
2026-09-01). The keystone decision: PULL transport — a child is fds and
the existing net verbs drive them, so no new transport machinery exists
anywhere in the track, and the old 1→2→3 chain broke. Only 1 → 2
chained; 3, 4, 5 startable alone. Plan per iteration, written when it
starts. Edges in [dependency graph section 6](../00-dependency-graph.md).

| # | Iteration | State |
| --- | --- | --- |
| 1 | [streaming subprocess](runtime-v2/01-streaming-subprocess.md) | ✅ **DONE 2026-09-02** — `proc.spawn -> Child{id,stdin,stdout,stderr}` (fds driven by the net verbs; kernel pipe = backpressure), `proc.wait_dl` (nil at deadline, one waiter), `proc.signal`; actor-owned lifecycle |
| 2 | [PTY](runtime-v2/02-pty.md) | ✅ **DONE 2026-09-02** — `proc.spawn_pty` via posix_openpt (no -lutil), `proc.resize`; `test -t` and live `stty size` legs |
| 3 | [signals as events](runtime-v2/03-signals-as-events.md) | ✅ **DONE 2026-09-02** — `signal.on(sig, addr)` delivering a fresh Signal record (scalar payloads crash by construction — spec amendment); handler-latch + wake eventfd instead of signalfd (amendment); TERM/INT refused by name |
| 4 | [termios adoption](runtime-v2/04-termios.md) | ✅ **DONE 2026-09-02** — `term.raw/restore`; restore proven a runtime obligation twice (DIV0 while raw, and the double-raw refusal itself) |
| 5 | [fd passing](runtime-v2/05-fd-passing.md) | ✅ **DONE 2026-09-02** — `net.send_fd`/`recv_fd`/`connect_unix`; a tty crossed the socket, was raw'd through the received copy and restored at destroy — the wmux handover in miniature |
| 6 | [term.size + term.width](runtime-v2/06-term-size-width.md) | ✅ **DONE 2026-09-02** — TIOCGWINSZ read twin (nil = not a tty) and libc wcwidth under C.UTF-8; the only runtime work the whole wmux parity ladder needs |
| 7 | [observability](runtime-v2/07-observability.md) | ⬜ `refine` — **moved here 2026-09-06** from language iteration 30 (`was_language_iteration: 30`). Runtime metrics/gauges, a `pprof`-equivalent profile, stack-trace-on-trap; consumers named (porch [8](porch/08-static-and-lifecycle.md)/[39](language-runtime-database/39-web-framework-parity.md), databasev2 [5](databasev2/05-bounded-tables-eviction.md), the limiter's lazy expiry). Forks: counters-only vs profiling, exposition format, pull vs push, trace-on-trap as a separable first slice. Stretches the track's charter (instrumentation, not processes/terminals/signals) — noted in the story |
| 8 | [symmetric cipher (AEAD)](runtime-v2/08-symmetric-cipher.md) | 🔄 **in-progress** — the **first rung of the TLS ladder** (gates rv2 9). **Phase A (ChaCha20-Poly1305) LANDED 2026-09-08**: `chacha20poly1305_seal`/`open` (ids 111/112), hand-rolled, matches RFC 8439 §2.8.2 byte-for-byte, KAT-gated in test_crypto (24/0), ASan/UBSan clean. Remaining: B hardware AES-GCM → C software AES → D cookie wrapper → E gate. Also locked: both ciphers, AES-NI+bitslice fallback, caller-supplied nonce, raw key. Consumers: rv2 9 TLS + porch encrypted cookies |
| 9 | [in-process TLS](runtime-v2/09-in-process-tls.md) | ✅ **`ready` 2026-09-07** — TLS **both directions**, **retiring the "TLS is the proxy's job" doctrine** (34/38/porch). Locked: **hand-roll TLS 1.3** (no vendored lib — keeps the zero-dep binary, raises the risk), **1.3-only**, **RSA+ECDSA+full X.509** cert verification (to reach real APIs). Decomposed into a bottom-up **phase ladder**: A AEAD (=rv2 8, forces AES-GCM there) → B HKDF → C X25519 → D signatures/RSA → E ASN.1/X.509 → F record+FSM client → G server. The project's **highest-risk** work; mandatory reference-tested/constant-time/negative-test gates. `net.connect` (110) landed; C/D/E may each split into own iterations |

### ▸ wmux — the terminal multiplexer track

New 2026-09-01, from [the tmux parity study](../plan/exploration/tmux/00-tmux-parity.md);
**re-scoped 2026-09-02 to FULL tmux parity** as a nine-rung ladder
([ladder spec](../superpowers/specs/2026-09-02-wmux-ladder-design.md)),
serving the recorded goal: acceptance by Linux-based developers. Runtime
prerequisites ALL landed (runtime-v2 1–6); every rung past 1 is pure
`.wo`. Durability is the ladder-wide differentiator — every rung's
state replays after a server restart, which tmux loses by design.

| # | Iteration | State |
| --- | --- | --- |
| 1 | [foundation](wmux/01-wmux.md) *(was language 43)* | ✅ **DONE 2026-09-02** — sessions, attach by fd-handover, durable capped chunk log, restart replay; `just wmux` 19/0 under a real PTY harness incl. the beyond-tmux restart-replay leg |
| 2 | [the screen](wmux/02-the-screen.md) | ✅ **DONE 2026-09-02** — VTE grid (vte.wo); reattach/restart paint the screen, proven grid-specific in the gate |
| 3 | [windows + status](wmux/03-windows-and-status.md) | ✅ **DONE 2026-09-02** — actor-per-window, status line, C-b c/n/p/digit; windows durable across restart |
| 4 | [split panes](wmux/04-split-panes.md) | ✅ **DONE 2026-09-02** — vertical 2-pane split, focus, composite grid render (N-way/horizontal deferred) |
| 5 | [copy mode](wmux/05-copy-mode.md) | ✅ **DONE 2026-09-02** — scrollback, copy-mode paging, yank to a durable paste buffer, paste |
| 6 | [multi-client](wmux/06-multi-client.md) | ✅ **DONE 2026-09-02** — multi-client mirroring (min-size/live-resize/mouse deferred) |
| 7 | [command system](wmux/07-command-system.md) | ✅ **DONE 2026-09-02** — C-b : prompt (neww/split/next/prev/killw) + config file (session directive) |
| 8 | [hooks + control](wmux/08-hooks-and-control.md) | ✅ **DONE 2026-09-02** — control-mode line protocol + session-created hooks delivered as actor messages |
| 9 | [parity audit](wmux/09-parity-audit.md) | ✅ **DONE 2026-09-02** — the tmux-vs-wmux catalog: every gap a named follow-up or a refusal by name |
| 10 | [layout tree](wmux/10-layout-tree.md) | 🟡 `refine` — **first slice DONE 2026-09-03**: horizontal `split-window -h`, directional `select-pane -L/R/U/D` (h/j/k/l), zoom (`resize-pane -Z`); rung-22 added the active-pane border marker. Still 2-pane max — N-way (3+), swap/break-pane, presets, durable layout pending |
| 11 | [formats + options + keys](wmux/11-formats-options-keys.md) | ✅ **DONE 2026-09-02** — durable options + `bind-key` tables, `#{...}` status format, one `run_command` dispatcher behind CLI/control/prompt/keys/config; folded rung 14's prompt-race fix. Fixed two baseline bugs: a PTY-EIO reader 100%-CPU spin and a concurrent kill-session chunk race. `just wmux` 36/0 |
| 12 | [resize + mouse](wmux/12-resize-and-mouse.md) | 🟡 `refine` — **attach-time sizing + SGR mouse DONE 2026-09-03/04**: sizes to the client's terminal via `term.size` (was fixed 80×23); wheel→copy-scroll, click→select-pane, status-row click→window; mouse re-armed after a full-screen app. Live SIGWINCH resize + multi-client min-size still pending |
| 13 | [copy selection + search](wmux/13-copy-selection-search.md) | 🟡 `refine` — **char-range selection DONE 2026-09-03**: vi `v`/`y`, highlighted, multi-line yank → buffer + OSC 52. Only incremental search + rectangle select pending |
| 14 | [control surface + prompt fix](wmux/14-control-and-prompt.md) | ⬜ `refine` — broaden control mode. **Prompt-race fix DONE in rung 11**; this rung narrows to the control-mode command surface + `%notifications` |
| 15 | [terminfo](wmux/15-terminfo.md) | 🟡 `refine` — **terminfo-lite DONE 2026-09-03**: a TERM allowlist (xterm/screen/tmux/alacritty/kitty/…; refuses only `dumb`) retired the foreign-`TERM` refusal. Full compiled-terminfo parsing still pending |
| 16 | [durability polish](wmux/16-durability-polish.md) | 🟡 `refine` — pane/layout persistence, killw compaction, buffers STILL pending. **First slice DONE 2026-09-02**: the Window owns + reaps its panes (spawns them in-actor so `wait_dl` works on its shard), fixing a `<defunct>` zombie leak; gate leg `attach-zombie`, 37/0 |
| 17 | [formats v2](wmux/17-formats.md) | ✅ **DONE 2026-09-03** — the full status format engine: `#(shell)` (cached, timer-refreshed), recursive `#{...}` with `#{?cond,a,b}` conditionals, `#{b:}`/`#{d:}` modifiers, `#{time}`/`#{host_short}`/real `#{window_name}` |
| 18 | [key tables](wmux/18-key-tables.md) *(new, from the config audit)* | 🟡 `refine` — **first slice DONE 2026-09-03**: no-prefix root table (`bind-key -n`), Meta + named keys (`key_code`), a tty key decoder in the Input actor; gate `bind-key -n M-h` fires without prefix, 42/0. `copy-mode-vi` table, `-r` repeat still pending |
| 19 | [mouse-driven UX](wmux/19-mouse-ux.md) *(new)* | 🟡 `refine` — **active-pane border highlight DONE 2026-09-04** (the split divider is the focus border + a direction marker; click flips it) plus **status-row click → select window** (2026-09-04). Still pending: drag-resize, drag-select. Forks open — brainstorm the rest before build |
| 20 | [display-popup](wmux/20-display-popup.md) | ✅ **DONE 2026-09-03/04** — session-owned modal float (`-E`), `-B` borderless (flush app frame), rounded gray border, popup mouse-wheel forwarding; drove the VTE OSC-swallow + popup frame-coalesce fixes. (Committed as "rung 19" then renumbered) |
| 21 | [sesh + switch-client](wmux/21-sesh-switch-client.md) | ✅ **DONE 2026-09-04** — in-session `switch-client -t B` / `-l`: sync `call` hand-off (Input exits only on success), `reg` threaded into sessions, fds handed off without close, B adopts + spawns a fresh Input; B-occupied refuses, missing→error. Plus a native **`choose-session` picker** (prefix `o`) — lists sessions on the status row, a digit switches. Fixed a `?actor RMsg` nullable-wrapper schema reorder (spurious restart migration) with a non-nilable `me`. Gate 54/0. (Full fuzzy `sesh connect` needs the tmux-compat CLI — rung 23) |
| 22 | [theming](wmux/22-theming.md) | ✅ **DONE 2026-09-04** — a tmux-style `style_sgr` engine (fg/bg named/bright/colourN + attrs) read from durable options; applied to `status-style`, `window-status-current-style`, `pane-active-border-style` (active border). Plus **automatic-rename** (window name follows the pane's OSC title). Later surface (`mode-style`, per-window format styling, `message-style`) can extend it |
| 23 | [plugin ports](wmux/23-plugin-ports.md) | ⬜ `refine` — run the developer's tmux plugins (thumbs, fzf, fzf-url) under wmux via `capture-pane` + a popup picker. Forks open (port each vs a `tmux` compat shim, which plugins first, capture scope). Rides rung 20 + rung 12 |

### Language track — sequenced, on the critical path

| #   | Item                                                                                                                                                                           | Plan                                                                                               |
| --- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------- |
| 5   | Haxe-parity language surface — **`?T` forced handling first**, then switch expressions, records, enum payloads, try/catch, statics, `using`, modules, `is`, `pub(read)`, `#if` | [plan 8](../plan/compiler/2026-08-01-haxe-parity-language.md)                                         |
| 6   | Program mode + systems stdlib — `fn main`, exit codes, `fs`/`proc`/`net`/`time`/`json`                                                                                         | [plan 9](../superpowers/plans/2026-08-01-program-mode-stdlib.md)                                      |
| 7   | log-watcher proof — the sample compiles and detects a silent death live                                                                                                        | [plan 10](../superpowers/plans/2026-08-01-log-watcher-sample.md)                                      |
| 8   | Shard-actor runtime                                                                                                                                                            | [arc plan](../superpowers/plans/2026-08-20-shard-fiber-arc.md) (plan 4 ✖ discarded 2026-08-21 — epoll-based) |
| 9   | Database engine binding                                                                                                                                                        | [plan 5](../superpowers/plans/2026-08-01-db-engine-binding.md)                                        |
| 9b  | `@table` + relations + language-integrated query — comprehension queries, `ref`/`backlink` navigation, GroupBy aggregates; acceptance: new `docs/examples/employee` sample     | [spec](../superpowers/specs/2026-08-15-table-relations-query-design.md) · [plan](../plan/compiler/2026-08-15-employee-relations-query.md) |
| 20  | Cross-program tables — attach to a running program's database (IPC string in wo.toml, manifest-granted rights, owner stays the single writer)                                  | **no spec yet** — four open forks recorded in the iteration; brainstorm before planning            |
| 21  | Keypair attach auth — mutual challenge–response, grants name public keys, uid superseded                                                                                       | **no spec yet** — four forks recorded; plan folds into 20's                                        |
| 22  | Durability + throughput + scale — restart-persistence, read/write benchmark, ~1M rows; the gate every later optimization re-runs                                              | **no spec yet** — four forks recorded; the measurement backbone                                    |
| 23  | io_uring group-commit write path — batched durability overlapped on shard threads, fsync fallback                                                                             | **no spec yet** — brainstorm after iterations 8 + 22                                               |
| 27  | Query grammar from real embedded-DB corpora — whole-query count + correlated exists, driven by the skillhost SQL catalogue; add only what a corpus uses | **no spec yet** — three forks; may collapse to "confirm len(query) + add exists" |
| 14  | skillhost host workload — port skillhost (MCP host + confined script runner) to writeonce; drives the missing host capabilities into the open (bounded subprocess, stdin/stdout transport, fs metadata, FFI-vs-out-of-process) | **no spec yet** — gaps recorded in the iteration; each gap brainstormed on demand, bounded-subprocess first |
| 42  | [Bounded subprocess](language-runtime-database/42-bounded-subprocess.md) — `proc.run` bounded in place (deadline, output caps, per-shard ceiling, owner-bound reaping via pidfd, fiber parked) + `proc.run_dl`; streaming form deferred by name | ✅ **DONE 2026-09-01, on `master`** — [spec](../superpowers/specs/2026-09-01-bounded-subprocess-design.md) · [plan](../superpowers/plans/2026-09-01-bounded-subprocess.md); test_proc 128/0, `just subprocess` 12/0; see NEXT PLAN |
| 17  | library projects + dependency privacy — `wo.toml` kind = "library" (checkable without entry, dual lib+bin) + Go-style `internal/` at the [deps] boundary; framework reorg demonstrates both | ✅ **landed 2026-08-20** — [spec](../superpowers/specs/2026-08-20-library-kind-internal-design.md) · [plan](../superpowers/plans/2026-08-20-library-kind-internal.md) |
| 10  | HTTP service layer                                                                                                                                                             | [plan 6](../superpowers/plans/2026-08-01-http-service-layer.md)                                       |
| 11  | Fibers                                                                                                                                                                         | vision §3, [blue-green exploration](../plan/exploration/blue-green-vm/00-vision.md)                   |
| 12  | Blue-green deploy                                                                                                                                                              | [spec](../superpowers/specs/2026-08-03-blue-green-vm-design.md) — plan authored after iterations 9 + 25 |

### Language track — parked until after iteration 26

Recorded 2026-08-08 by scope directive; nothing here lands before the
log-watcher proof.

- `WO-W201` `@gc`-suggestion refinement beyond the self-reference heuristic
- `WO-E225` broadened to `ref`/`multi`/`map` element types and fn signatures
- ADT container roster adoption (Stack, Queue, Set, Tree, Graph, …) — see the
  roster in [`compiler/nullable-types-implementation.md`](../plan/compiler/nullable-types-implementation.md)
- Web framework as a `.wo` library; UI (`##ui` SSR + live patches);
  script-based destructive migrations; MCP/agent wrapper over the management plane
- `throw` (explicit raise) — cut 2026-08-10, 0 uses in the driving workload
  (log-watcher); catch frames ship without it
- `time.mono` — cut 2026-08-10, 0 uses in the driving workload; returns when a
  workload needs monotonic math
- `is` — cut 2026-08-10, 0 uses in the driving workload; emptied plan 8's old
  Task 7, which is deleted rather than deferred

### Frontend — removed as stale (2026-08-17)

The `##ui` / `.htmlx` LiveView frontend track — 13d pricing UI, the 14-MVC-UI
implementation plan, the 7-of-7 `ui-htmlx-live` plan, and the 9-doc
`plan/exploration/ui/` design set — was **removed**. It was built entirely on
the non-advancing Rust runtime (`.dev/reference/crates/wo-htmlx`, `cargo run`,
WebSocket live-patches) and contradicts the current woc/wovm direction. Recorded
in [`discarded.md`](../plan/discarded.md).

---

## Discarded

Settled rejections with their reasons live in [`discarded.md`](../plan/discarded.md) —
inheritance, `abstract` newtypes, `Money`/`SKU`/`Float`, `Dynamic`/`cast`/
`macro`/`extern`, AOT-to-C, Menhir, shared mutable engine state, external
deployer daemon, destructive migrations in v1, and more. Argue against the
recorded reason rather than re-opening an entry as new.

## Learnings

What attempts taught, shipped or not, in [`learnings.md`](../plan/learnings.md) —
plumbed-is-not-enforced, vacuously-passing goldens, exit-0-with-wrong-output,
the malloc-path ASan trick, deferred checks that never reach the runtime,
validate-once-at-the-boundary, and reference-implement-in-C-first.

---
track: runtime-v2
iteration: "5"
status: pending
readiness: refine
---

# runtime-v2 5 — fd passing: SCM_RIGHTS over the unix socket

> Part of [Story — runtime-v2: the runtime beyond sockets](00-story.md).
> No incoming edges — startable alone, any time. Promoted from the
> alacritty study's Wayland stage by the
> [tmux study](../../plan/exploration/tmux/00-tmux-parity.md): the
> multiplexer needs it FIRST — a wmux client hands its tty fd to the
> server over the unix socket, the server writes escape sequences
> directly to the client's terminal, and detach is just the client
> process dying. That handover IS the tmux architecture
> (`.dev/reference/tmux` `compat/imsg-buffer.c`, `proc.c`).
>
> **The problem.** `net.listen_unix` exists (iteration 35) but a byte
> stream is all it carries; an fd cannot cross it. `sendmsg` with
> SCM_RIGHTS ancillary data is the only mechanism, and it is runtime
> work by nature.

## Info — the forks (open)

1. **Surface.** `net.send_fd(conn, fd)` / `net.recv_fd(conn)` — two
   verbs, fd travels alone (the lean; tmux sends its imsg header as
   ordinary bytes beside it) — versus fd-attached-to-a-message framing
   in the runtime.
2. **What arrives.** The received fd as an opaque scalar the existing
   `net.read`/`net.write`/termios verbs accept (lean — every fd verb
   already takes an Int-shaped conn) versus a new wrapped type.
3. **The unix-socket client side.** Iteration 38's `net.connect` covers
   outbound TCP; the CLIENT half of a unix-socket connection may or may
   not exist by then — this iteration carries `net.connect_unix` if 38
   has not landed it first. Verify at brainstorm, not assumed.
4. **Bounds.** One fd per message, refusal by name past it (lean),
   versus SCM_RIGHTS' multi-fd arrays. YAGNI: no consumer sends two.

## Acceptance sketch

- A test parent and child (via [1](01-streaming-subprocess.md)) pass an
  open pipe fd across a unix socket; bytes written on one side arrive on
  the other through the RECEIVED fd.
- A tty fd crosses and [4](04-termios.md)'s verbs work on it — the wmux
  handover in miniature.
- Refusals: passing on a non-unix socket, receiving where none was sent
  — both by name, neither a hang.
- fd hygiene: the churn leg, passing edition — counts flat.

## Consumers

wmux 1 (detach/attach), the alacritty study's stage D (Wayland needs
SCM_RIGHTS for shm buffers), and — the board's porch 2 aside — nothing
else yet, which is exactly why the surface stays two verbs.

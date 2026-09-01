---
track: runtime-v2
iteration: "5"
status: pending
readiness: ready
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

## Info — the forks, settled (brainstorm 2026-09-01; spec:
[`2026-09-01-runtime-v2-design.md`](../../superpowers/specs/2026-09-01-runtime-v2-design.md))

1. **Two verbs, fd travels alone**: `net.send_fd(conn, fd) -> Bool` /
   `net.recv_fd(conn) -> ?Int` — `sendmsg` + SCM_RIGHTS, fixed
   ancillary buffer; framing stays the caller's ordinary bytes (the
   tmux imsg shape, composed in `.wo`).
2. **A received fd is a plain Int** every existing fd verb accepts —
   net reads/writes, termios adoption included.
3. **This iteration CARRIES `net.connect_unix(path) -> Int`** —
   iteration 38 verified pending at brainstorm time, not assumed.
4. **One fd per message**, refusal by name past it; non-unix sockets
   refuse by name; multi-fd arrays wait for a consumer.

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

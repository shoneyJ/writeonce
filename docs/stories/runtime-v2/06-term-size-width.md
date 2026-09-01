---
track: runtime-v2
iteration: "6"
status: done
readiness: ready
---

# runtime-v2 6 — term.size and term.width: the two verbs the wmux ladder asked for

> Part of [Story — runtime-v2: the runtime beyond sockets](00-story.md).
> Named by the wmux parity-ladder brainstorm (2026-09-02) as the ONLY
> runtime work all nine wmux rungs need beyond the landed track.
>
> **DONE 2026-09-02, same day.** `term.size(fd) -> ?TermSize {cols,
> rows}` — TIOCGWINSZ, the resize verb's read twin; nil = not a tty (an
> expected answer, never a trap). `term.width(cp) -> Int` — libc
> `wcwidth` under `C.UTF-8` (first use sets `LC_CTYPE`; falls back to
> the host locale): -1 control, 0 combining, 1, 2. Ids 108/109;
> `TermSize` joins the predeclared records. Consumers: wmux 1 (attach
> at the client's real size), wmux 2 (grid column math), wmux 6
> (multi-client min-size, SIGWINCH forwarding).
>
> Legs in `test_term.c`: a PTY sized 77×33 from outside answers exactly
> that; a pipe answers nil; widths a=1, U+4E2D=2, U+0301=0, BEL=-1.

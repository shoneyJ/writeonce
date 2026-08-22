# site — how it is put together

Written 2026-08-23, with the sample's landing. Three files, one binary.

| file | what it owns |
| --- | --- |
| `main.wo` | the `Chapter` table, seed-if-empty, the HTML shell (header/nav), four handlers (Home, ShowChapter, AdminEdit, Health), `main` |
| `content.wo` | the nine chapter bodies as functions returning HTML fragments, and `seed_chapters()` — same directory, so it shares `main.wo`'s declarations without `use` |
| `wo.toml` | the two [deps]: `framework` (serving) and `html` (markup) |

Decisions that are not obvious from the code:

- **Chapters are rows, not constants.** `seed_if_empty()` inserts them only
  when the table answers empty, so a WAL restart keeps admin edits instead
  of reseeding over them — the sample's own proof of chapter 6's claim.
  The seed bodies are BUILT with wo-html's builders at boot; after that
  the table is the truth and the builders are never consulted again.
- **Auth is handler-side by doctrine.** The framework ships mechanism
  (`bearer_token`, constant-time `ct_eq`); which routes are gated and by
  which token is policy, so `AdminEdit` checks its own field. No global
  middleware — the public pages stay public.
- **`\$` in chapter code samples.** Chapter sources show interpolation
  (`${port}`) inside string literals of a language that interpolates —
  the lexer's `\$` escape keeps them literal; `code_block()` then
  HTML-escapes the result.
- **One-line concat chains.** `..` does not straddle newlines (Go-style
  implicit statement ends), so long fragments build accumulator-style
  (`b = b .. "...";` per line) — the same shape serve.wo uses for
  response heads.
- **wo-html's sheet is static.** Tailwind's class NAMES, one hand-written
  CSS string inlined per page by `page()` — self-contained responses, no
  toolchain; growing the sheet is appending a line in `tw_css()`.

Gate: `just site` — see scripts/site-accept.sh (11 checks; the restart
leg polls `/health` instead of sleeping, so it does not share
web-app-accept's 0.5s boot race).

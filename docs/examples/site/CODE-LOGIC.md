# site — how it is put together

Written 2026-08-23 with the sample's landing; restructured 2026-08-25
onto the program template's MVC layout (`docs/examples/shop`), so the two
samples now read the same way.

## The layout

| file | layer | what it owns |
| --- | --- | --- |
| `types.wo` | MODEL | the `Chapter` `@table`, the `ChapterLink` projection, `Chapters.links()`, and `seed_if_empty()` |
| `content.wo` | MODEL (content) | the ten chapter bodies as fragment-returning functions, plus `seed_chapters()` |
| `layout/app.wo` | VIEW (chrome) | `AppShell` — the component that fills writeonce-view's `Layout` — the two named widths, and `html_error` |
| `layout/header.wo`, `layout/footer.wo` | VIEW (chrome) | the shared nav bar (brand = mark + wordmark) and footer |
| `layout/logo.wo` | VIEW (chrome) | the mark as inline SVG, plus the `<head>` links |
| `install/view.wo`, `install/controller.wo` | VIEW + CONTROLLER | `/install` — the toolchain guide. Static copy, so `InstallPage` has no fields |
| `packages/view.wo`, `packages/controller.wo` | VIEW + CONTROLLER | `/packages` and `/packages/:name` — the catalogue, its cards, and per-package usage |
| `favicon/controller.wo` | CONTROLLER | `/favicon.svg` — builds its own `Resp` (image/svg+xml) |
| `home/view.wo` | VIEW | `HomePage` and the homepage's code showcase |
| `home/controller.wo` | CONTROLLER | `Home` — the `/` handler |
| `chapter/view.wo` | VIEW | `ChapterNav`, `ChapterPage` |
| `chapter/controller.wo` | CONTROLLER | `ShowChapter` — the `/ch/:slug` handler |
| `admin/controller.wo` | CONTROLLER | `AdminEdit` — bearer-gated edit, answers a redirect (no view: it redirects) |
| `health/controller.wo` | CONTROLLER | `Health` — the liveness probe (no view: it answers text) |
| `main.wo` | BOOTSTRAP | seed, routes, serve. Nothing else |
| `wo.toml` | — | the two `[deps]`: `framework` (serving) and `html` (markup) |

One feature = one directory = one module, holding that feature's view
and its controller together. A module sees its own declarations plus
what it `use`s, so `home/` reaching the chapter nav has to say `use
chapter`.

The model stays at the root and is reachable from everywhere: a CLASS
crosses module lines without being exported, and only a free `fn` is
module-scoped (`WO-E210`). That single rule explains the whole layout —
`Chapter` and `ChapterLink` are classes, so the feature modules just
name them; the shared query would have been a free fn, so it is a
`static fn` on `Chapters` instead. (`pub` cannot prefix an `@table`
class — recorded gap #1 — but nothing needs it to.)

## Decisions that are not obvious from the code

- **Chapters are rows, not constants.** `seed_if_empty()` inserts them
  only when the table answers empty, so a WAL restart keeps admin edits
  instead of reseeding over them — the sample's own proof of chapter 6's
  claim. The seed bodies are BUILT with writeonce-view's builders at boot; after
  that the table is the truth and the builders are never consulted again.
- **The seam is enforced by where the query sits.** `Chapters.links()`
  lives with the MODEL and hands the view a `multi ChapterLink` —
  a projection, not a cursor. No component in this sample touches the
  database, which is what lets `ChapterNav` be the same component on the
  homepage and on every chapter page, differing only by `current`.
- **`HomePage` and `ChapterPage` hold a `Component`, not chapter data.**
  The nav arrives as an already-built child component in a slot, so
  neither page knows what a chapter is. That is content projection —
  Angular's `<ng-content>`, with the slot as an ordinary field.
- **Two widths, named once.** `AppShell` carries a `container` field and
  `layout/app.wo` exports `reading_shell` / `wide_shell`. The Tailwind
  class strings appear in exactly one place instead of being repeated at
  every call site.
- **Auth is handler-side by doctrine.** The framework ships mechanism
  (`bearer_token`, constant-time `ct_eq`); which routes are gated and by
  which token is policy, so `AdminEdit` checks its own field. No global
  middleware — the public pages stay public.
- **`ok_html` is the framework's**, beside `ok_text`/`ok_json`: a status
  line plus a content-type is transport, not rendering.
- **`\$` in chapter code samples.** Chapter sources show interpolation
  (`${port}`) inside string literals of a language that interpolates —
  the lexer's `\$` escape keeps them literal; `code_block()` then
  HTML-escapes the result. This is also why those two samples stay
  escaped `"..."` strings rather than becoming raw literals: a raw
  literal has no escape character, so it cannot spell a literal `${`.
- **Concat spans lines two ways now.** A line ENDING in `..` continues on
  the next (the one newline suppression in the language) — it never works
  at the START of a line. For markup, prefer the backtick raw literal:
  real newlines, real double-quoted attributes, source indentation
  removed at compile time, `${ }` raw and `{{ }}` auto-escaping. The old
  "`..` does not straddle newlines, so build accumulator-style" note is
  obsolete and was removed.
- **The logo is inline SVG, authored once.** `logo_svg(px)` goes in the
  nav brand and `favicon_svg()` is served at `/favicon.svg` — a dark tile
  with a two-stroke "W", white then accent blue. No asset pipeline, no
  binary in the repo, and it stays legible at 16px. The `<head>` link
  reaches the document through `Layout`'s `head` slot.
- **A raw literal cannot contain a literal `{{`.** The packages page has
  prose ABOUT `{{ }}` holes, and writing it directly would have made it a
  hole; it is written with `&#123;` entities instead. This is the same
  limitation the chapter code samples hit with `${`, and the reason both
  doors exist.
- **Downloads are the framework's, not the site's.** `/dl/*path` is
  `StaticFiles` mounted in `main.wo` with a 16 MiB ceiling — no
  controller, because there is no decision to make. `WO_DIST` says where
  the tarballs are (default `./dist`). The install page offers the GitHub
  release as primary and this as a mirror, with the `.sha256` beside it.
- **The supported-systems list is read off the binaries**, not off a
  wish list: `file` gives the triple, and the highest `GLIBC_` symbol
  version they import gives the libc floor (2.38 today). Overstating
  support costs a reader an afternoon.
- **`SITE_HOST` picks the interface.** Loopback by default — right behind
  a proxy — with the env var for reaching a dev instance across the LAN.
  The bound address is printed at startup.
- **writeonce-view's sheet is static.** Tailwind's class NAMES, one hand-written
  CSS string inlined per page by `page()` — self-contained responses, no
  toolchain; growing the sheet is appending a line in `tw_css()`.

Gate: `just site` — see `scripts/site-accept.sh` (11 checks; the restart
leg polls `/health` instead of sleeping, so it does not share
web-app-accept's 0.5s boot race).

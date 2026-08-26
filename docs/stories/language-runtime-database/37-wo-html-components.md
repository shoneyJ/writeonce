---
iteration: "37"
status: done
---

# Iteration 37 — wo-html components: an MVC-shaped view layer (Angular's format, studied)

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).
>
> **Inserted 2026-08-23** (developer ask: "enhance wo-html like MVC;
> understand Angular format"). Grows the wo-html LIBRARY, never the
> framework — the 2026-08-20 micro-framework directive stands: routing/
> middleware/`Req`/`Resp` stay MVC-free, and the view layer lives in its
> own dependency (the site sample's two-dep lesson). Unscheduled —
> independent of the concurrency chain; needs its spec brainstormed
> first.
>
> **REDIRECTED 2026-08-23** (developer review of the shop template
> against a Vue SFC): render()-as-string-concatenation failed the DX
> referendum — markup must be markup-FIRST. **RE-POINTED 2026-08-24**
> (developer counter-proposal): the leaning is now **in-class raw
> template literals**, not separate `.html` files — `render()` RETURNS
> a raw multi-line string literal whose `{{ expr }}` holes are
> auto-escaped, compile-time-checked interpolations in class scope (a
> typo'd field is a compile error). Compiler surface shrinks to one
> lexer addition (the raw literal — a general language win: multi-line
> text without `\"` noise) plus one desugar; no file pairing, no
> `w:component` sections. Structural control stays the language's own
> `if`/`for` composing literals; `w:if`/`w:for` attributes and separate
> `.html` files are DEMOTED to a later option that can layer on without
> breaking this form. Escaping: `{{ }}` always escapes; the raw/slot
> spelling is the one greppable door. A runtime mustache-lite stays
> REJECTED (reflection-free means untyped `map<Text, Text>`); client-
> side reactivity (`ref`, `@click`, `v-model`) stays out under the
> no-JS posture — forms and POSTs instead. Still a COMPILER iteration,
> parked behind the standing "no compiler/VM/database changes yet"
> directive. The shop template
> ([`docs/examples/shop`](../../examples/shop/README.md)) is the
> consumer: each `render()` body becomes one raw template literal, and
> its README's recorded gaps ride along (gap #1: `pub` + `@table`
> cannot combine — blocks a shared model module; gap #2: `@view`
> projection classes).
>
> **CODE LANDED 2026-08-24** — the compiler slice only. Forks #1 and #3
> are settled and shipped: the raw literal is BACKTICK-delimited with
> verbatim content, `{{ }}` always HTML-escapes and `${ }` stays raw,
> and the common source margin is removed at lex time. `{!! !!}` as the
> raw slot is DISCARDED — `${ }` already was the raw spelling and a
> second one would have been a synonym. The escaping desugar turned out
> to need no compiler knowledge of HTML at all: `{{ e }}` becomes a
> `Call` on whatever `esc` is in scope, so typecheck, ownership,
> codegen, the `.wob` format and the VM are all untouched, and the
> standing "no VM/database changes" directive was never in play. Two new
> lexing diagnostics ride along (WO-E004 unterminated raw literal,
> WO-E005 newline inside a quoted string — the latter closing a silent
> file-swallowing hole that existed since Task 3). The COMPONENT half of
> this story — the `Component` interface, `Layout` with slots, the site
> migrating onto them — is untouched and still pending.
>
> **LANDED 2026-08-25** — the component half, and with it the iteration.
> wo-html gained `Component` (structural, the view twin of the
> framework's `Handler`), `render_all` and `Layout`; fork #4 is settled
> by MOVING `ok_html` into the framework beside `ok_text`/`ok_json`
> (transport, not rendering — both HTML samples had hand-rolled the same
> four lines), and fork #5 by migrating BOTH samples. Fork #2 stands as
> written: no directive surface ships. Layout slots are pre-rendered
> `Text`, not `multi Component` — the compositional variant was proven
> possible and deliberately not taken for the layout, though the PAGE
> components do use it.

## Why this iteration exists

wo-html today is element builders + one utility sheet: pages are
functions concatenating Text. That works (the site proves it) but has
no unit of reuse bigger than a function — no way to say "this fragment
owns its data, its markup, and its place in a layout" and hand it
around. Angular's component FORMAT — a class declaring its inputs, a
template rendering them, composition by nesting, structural directives
for repetition and choice — is the studied precedent: the FORMAT
translates to server-rendered `.wo`; the client-side half (change
detection, event bindings, SPA router) deliberately does not.

## What Angular's format maps to (the study, summarized)

| Angular | wo-html translation | doctrine fit |
| --- | --- | --- |
| `@Component` class with `@Input()`s | a class whose FIELDS are the inputs, satisfying a structural `Component` interface (`fn render() -> Text`) | behavior-as-class; no closures needed |
| template (`{{ expr }}`) | the render method's interpolation — `.wo` already has `${...}` in Text | no template dialect: templates ARE code |
| `*ngFor` / `*ngIf` | explicit `for`/`if` in render() building Text — the language's own control flow | no structural-directive mini-language |
| content projection (`<ng-content>`) | a layout component taking pre-rendered `Text` slots as fields | slots are ordinary values |
| services/DI | no translation — a component reads its fields; queries stay in handlers (M and V stay separate) | rejected: DI needs function values |
| event bindings `(click)` / two-way `[(ngModel)]` | no translation — server-rendered, no JS doctrine; forms stay `form_post` round trips | rejected surface |

## Goals

- **A `Component` interface in wo-html**: structural (`fn render() ->
  Text`), so any class with fields + render satisfies it — the view
  twin of the framework's `Handler`. Composition is nesting: a parent's
  render calls children's render.
- **The MVC seam stated**: Model = `@table` rows queried in the HANDLER,
  moved into component fields; View = components rendering Text;
  Controller = the framework handler wiring them. The library documents
  the seam; it never queries.
- **Layout components with slots**: the site's nav/shell/footer become
  the proof — a `Layout { title, nav, content, footer }` component
  replacing today's `shell()` functions, chapter pages and homepage
  composing it.
- **The site sample migrates** as acceptance: same rendered bytes (or
  deliberately better), gate stays green — the library grew a floor, not
  a rewrite.

## Acceptance Criteria (draft — the spec refines)

- **Given** a class with fields and `fn render() -> Text`, **when** a
  handler moves data in and calls render, **then** the page it serves is
  byte-deterministic from the fields — no hidden state, no globals.
- **Given** nested components (layout → section → card), **when** the
  outer render runs, **then** children render through the same
  structural interface, and escaping stays the caller-explicit `esc()`
  rule at every level.
- **Given** the migrated site sample, **when** `just site` runs,
  **then** 11/0 — the gate is the proof the component layer reproduces
  the existing pages.
- **Given** a component reused across two pages (the chapters card on
  home and chapter pages), **when** either page changes its data,
  **then** the other's markup is untouched — reuse is real, not copied.

## Out Of Scope

- Client-side anything: change detection, event/two-way bindings, SPA
  routing, hydration — the no-JS posture stands; interactivity is form
  round trips until a directive says otherwise.
- A RUNTIME template engine (files parsed per request, mustache-style) —
  rejected 2026-08-23: reflection-free means untyped `map<Text, Text>`
  values. Templates compile to code at build time or they don't exist.
- Dependency injection / services — components are data-in, Text-out.
- Moving wo-html into the framework — settled 2026-08-23: separate
  libraries, composed via `[deps]`.
- CSS componentization (scoped styles) — the utility sheet stays one
  static string; measure pain first.

## Info

Forks the spec must settle (REVISED 2026-08-23 for the compiled-template
direction):

1. ~~**The raw-literal spelling**~~ — SETTLED 2026-08-24 and landed:
   backticks, content verbatim (no escape processing), common margin
   removed at lex time, `${ }` raw and `{{ }}` escaping. A literal
   backtick, `${` or `{{` is written by concatenating a `"..."` string
   with `..` — one greppable door instead of an escape character in the
   one form whose point is not having any.
2. ~~**Directive surface**~~ — SETTLED 2026-08-25 as written: v1 ships
   NONE. Structural control is `if`/`for` composing literals, and two
   migrated samples produced no case that wanted more. `w:if`/`w:for`
   and separate `.html` files remain the later option — and a
   COMPILE-TIME include is the only shape either could honestly take,
   since a file read per request is the rejected engine.
3. ~~**Escaping default**~~ — SETTLED 2026-08-24 and landed: `{{ }}`
   escapes ALWAYS, through whatever `esc` is in scope (wo-html's, or a
   local one that shadows it deliberately). `${ }` is the raw door.
   Proven byte-identical against the hand-written `esc()` calls it
   replaces, hostile input included.
4. ~~**The framework seam**~~ — SETTLED 2026-08-25: `ok_html` MOVED
   into `framework/http/types.wo`; no `respond(c)` sugar, because
   `ok_html(c.render())` already is it.
5. ~~**Migration depth**~~ — SETTLED 2026-08-25: BOTH, site gated and
   shop hand-driven; web-app stays HTML-less as the counter-example.

Study sources: Angular's component/`@Input`/`ng-content` docs (format
only; no Angular code enters the repo — candidate `.dev/reference`
addition if deeper study is wanted), Go's `html/template` as the
server-side contrast, and the site sample as the living consumer.

## Landed 2026-08-24 — the raw text literal

The compiler slice, and nothing else. `compiler/src/lexer.ml` gained one
branch: a backtick opens a literal whose content is verbatim to the
closing backtick, newlines included, with two hole forms — `${ }` raw
and `{{ }}` escaped — and the common source margin removed before the
token is emitted (Java's text-block rule). `token.ml` gained one
`str_part` variant to carry the escaped hole; `parser.ml`'s
`desugar_interp` wraps it in a call to `esc`; `dump.ml` labels it. That
is the entire compiler surface. Nothing in typecheck, ownership,
codegen, the `.wob` format or the VM changed, because the literal emits
the same `Str`/`InterpStr` token a `"..."` string always did.

Proven: `woc-test` 554/0 with new token and AST goldens; `oop-e2e`
115/0 with a run fixture for the literal and a compile-fail fixture for
WO-E004; `just site` 11/0 and `just web-app` 46/0 after wo-html's
builders and the site's two chapter snippets migrated onto the form; a
parity program comparing every migrated wo-html builder's old and new
output on hostile input (`<`, `>`, `&`, `"`) — byte-identical in all
eight. `page()` is the one deliberate byte change: four newlines now
sit inside `<head>`, where whitespace is insignificant, and none inside
`<body>`. Every `.wo` in the repo was re-lexed: no file gained a
diagnostic.

The shop template migrated too — five view files, every `render()` body
now one literal with real double-quoted attributes and no `esc()` calls
— verified by building it against local `file://` dep remotes and driving
every route, since no `just` recipe gates it.

## Landed 2026-08-25 — the component half

`wo-html` grew three things and no more: `pub interface Component { fn
render() -> Text }`, `pub fn render_all(cs: multi Component) -> Text`,
and `pub class Layout { title, nav, content, footer }` whose slots are
pre-rendered Text. The MVC seam is stated in the library's own header
and README, and the library still contains no query.

A finding that shaped the design: **`multi Component` holds a
heterogeneous list directly** — no wrapper record. The framework's
`Mw`/`Aw` wrappers had suggested otherwise; they are not a language
requirement. That is what let a page component hold its children as
`multi Component` and call `render_all` on them, which is where the
interface actually earns its place — an interface nothing consumes as a
TYPE would have been decoration.

Fork #4 settled by moving: `ok_html` now lives in `framework/http/types.wo`
beside `ok_text`/`ok_json`, and both duplicate copies are deleted. No
`respond(c: Component)` sugar — `ok_html(c.render())` is already the
whole thing, and a second spelling would earn nothing.

The site migrated: `shell`/`shell_wide` fill `Layout` (byte-identical —
`Layout.render()` is `page(title, nav .. content .. footer)`, exactly
what the old functions built), and `chapter_nav` became a `ChapterNav`
component whose query moved out into `chapter_links()`, called by the
handlers. The homepage and every chapter page now render the SAME
component with a different `current` — acceptance criterion 4, met by
construction rather than by inspection.

The shop migrated further than planned, because its views already had
`render()`: `app_shell()` became an `AppShell` component, and
`ProductListPage`/`OrdersPage` now hold `multi Component` children
instead of a concatenated Text blob, with the queries in the
controllers. `AppShell` is deliberately NOT `Layout` — the template
links a real stylesheet where `Layout` inlines `tw_css()`, and that
difference is the point of it having its own shell.

**Restructured 2026-08-25 (follow-on):** the site sample was then laid
out like the program template — `types.wo` for the model, a `layout/`
module for the chrome, `home/` and `chapter/` view modules, one
`*.controller.wo` per feature in the root module, and a `main.wo` that
is bootstrap and nothing else. `main.wo` went from 231 lines holding
everything to 44 lines holding routes. Both samples now read the same
way, which was the point: the template teaches a shape, and the site
should not contradict it.

Proven: `just site` 11/0 (the gate, and the escape check still passes),
`just web-app` 46/0 (the framework gained a function), `woc-test` and
`oop-e2e` untouched and green. The shop was built against local `file://`
dep remotes and driven through every route — product grid (4 cards
rendered as child components), product page with its form, buy,
`/orders` (rows as child components), the empty-orders branch, 404, 409,
and `/assets/*`.

## Proposed Solution

Brainstorm → spec → plan (the superpowers path): settle the four forks,
grow wo-html by the `Component` interface + a `Layout` proof, migrate
the site sample as acceptance, keep the framework untouched. Ships
independently of the concurrency chain; slots wherever the developer
schedules it.

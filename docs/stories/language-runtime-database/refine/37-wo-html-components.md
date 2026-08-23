---
iteration: "37"
status: refine
---

# Iteration 37 — wo-html components: an MVC-shaped view layer (Angular's format, studied)

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](../00-story.md).
>
> **Inserted 2026-08-23** (developer ask: "enhance wo-html like MVC;
> understand Angular format"). Grows the wo-html LIBRARY, never the
> framework — the 2026-08-20 micro-framework directive stands: routing/
> middleware/`Req`/`Resp` stay MVC-free, and the view layer lives in its
> own dependency (the site sample's two-dep lesson). Unscheduled —
> independent of the concurrency chain; needs its spec brainstormed
> first.

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
- A template LANGUAGE (files parsed at build or runtime) — templates are
  `.wo` code by doctrine (no closures also means no template lambdas).
- Dependency injection / services — components are data-in, Text-out.
- Moving wo-html into the framework — settled 2026-08-23: separate
  libraries, composed via `[deps]`.
- CSS componentization (scoped styles) — the utility sheet stays one
  static string; measure pain first.

## Info

Forks the spec must settle:

1. **Interface shape** — `render() -> Text` alone, or `render(ctx) ->
   Text` with a context record (e.g. the request's principal for
   view-level decisions)? Leaning: bare `render()` — context smells like
   DI; whatever the view needs arrives as a field.
2. **The framework seam** — does `ok_html(body)` move into the framework
   beside `ok_text`/`ok_json` (transport, not rendering — flagged
   2026-08-23), and does wo-html gain `respond(c: Component)` sugar?
3. **Slot typing** — slots as pre-rendered `Text` fields (simple, order
   forced by the parent) vs slots as `Component` fields (uniform, but
   ownership of nested components needs care under move semantics).
4. **Migration depth** — site only, or web-app's HTML-less JSON pages
   deliberately untouched as the counter-example (a framework consumer
   that never links the view layer)?

Study sources: Angular's component/`@Input`/`ng-content` docs (format
only; no Angular code enters the repo — candidate `.dev/reference`
addition if deeper study is wanted), Go's `html/template` as the
server-side contrast, and the site sample as the living consumer.

## Proposed Solution

Brainstorm → spec → plan (the superpowers path): settle the four forks,
grow wo-html by the `Component` interface + a `Layout` proof, migrate
the site sample as acceptance, keep the framework untouched. Ships
independently of the concurrency chain; slots wherever the developer
schedules it.

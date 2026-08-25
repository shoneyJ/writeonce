# shop — the writeonce program template

A small store you can buy from, structured the way a real writeonce web
app should be. **Copy this directory to start a new app**; every file
has one concern, and the module system (one directory = one module,
`pub` = the export line) enforces the separation the layout promises.

## Run it

```
cd docs/examples/shop
woc . && WO_DATA=./data ./target/shop 8080     # durable store
./target/shop 8080                             # RAM-only (dev)
```

Browse http://127.0.0.1:8080/ — products → product page → buy (stock
checked and decremented) → confirmation → /orders. With `WO_DATA`, kill
it and restart: the orders are still there (WAL replay).

The template builds and runs as written — the two `[deps]` resolve, the
seed lands, and every route answers.

## The view form

A `render()` body is one backtick raw text literal. Markup is markup:
real newlines, real double-quoted attributes, and the method's source
indentation removed at compile time, so the served bytes carry the
markup's own nesting and not the code's.

```
fn render() -> Text {
  return `
    <div class="card">
      <h3><a href="/p/{{ self.sku }}">{{ self.name }}</a></h3>
      <p class="price">€ ${self.price}</p>
      ${stock}
    </div>`;
}
```

Every `render()` makes its class a **component** — wo-html's structural
`Component` interface, satisfied by having the method, never declared.
Parent components hold children directly (`cards: multi Component`) and
render them with `render_all`, so `ProductListPage` knows nothing about
`ProductCard` beyond `render()`. The document itself is a component too:
`AppShell { title, content }` in `layout/app.wo`, which links a real
stylesheet rather than inlining one — that is why it is its own shell and
not wo-html's `Layout`.

Two holes, and the difference is the whole escaping story:

- `{{ expr }}` **HTML-escapes** — it compiles to a call to the `esc` in
  scope (wo-html's, unless the app declares its own). Display data goes
  here; a typo'd field is a compile error, not a broken page.
- `${ expr }` is **raw** — for markup you built yourself, like the
  `${stock}` fragment above or `${content}` in the app shell.

Nothing is parsed at request time. The literal is a compile-time form:
it produces exactly the string constant and concatenation chain the old
hand-written version did, so there is no template engine to ship, warm
up, or sandbox.

## The file map (Angular equivalents)

| this template | concern | Angular analog |
| --- | --- | --- |
| `types.wo` | MODEL — `@table` classes ARE the WAL database | `models/*.ts` (+ the entire database) |
| `layout/app.wo` | app shell: document, header+footer composition, `ok_html`/`html_error` transport helpers | `app.component.html` |
| `layout/header.wo` / `footer.wo` | shared chrome fragments | `header.html` / `footer.html` |
| `product_list/view.wo` | VIEW — classes with `fn render() -> Text`, fields = exactly what is displayed | `product-list/view.html` |
| `product_list/controller.wo` | CONTROLLER — query the model, fill the view, answer a `Resp`; beside its view in the same module | component `.ts` + service |
| `product_page/`, `orders/` | one module per feature: `view.wo` + `controller.wo` | feature folders |
| `static_files/controller.wo` | `/assets/*` from disk, traversal-safe, typed | `angular.json` assets |
| `assets/style.css` | ONE real stylesheet, sectioned per feature | the `.scss` files |
| the `render()` bodies | ONE raw text literal each: real newlines, real double-quoted attributes, source indentation removed at compile time, `${}` raw holes and `{{ }}` auto-escaping ones | Vue `<template>` (in-SFC) |
| `layout/app.wo` → `AppShell` | the document, as a two-slot component | `app.component.html` |
| `main.wo` | bootstrap: seed, routes, serve — nothing else | `app-routing.module.ts` + `main.ts` |

Separation is compiler-enforced: one feature = one directory = one
module, holding that feature's view AND its controller. A module sees
only its own declarations plus what it `use`s, so a controller reaching
into another feature has to say so. The `@table` classes sit in
`types.wo` at the root and are reachable from every feature module
without export — see gap #1 below for why that is not the contradiction
it looks like.

## What is deliberately different (doctrine)

- **Templates compile or they don't exist (story 37).** See *The view
  form* above: the markup is a compile-time literal, never a file
  parsed per request. Gap #3 ("the language has NO multi-line
  expression or literal", recorded while writing this template) is
  CLOSED — the raw literal landed with story 37's compiler slice, and
  this directory was its consumer. Styles stay a real CSS file, served
  statically (there is no scss preprocessor).
- **No closures, no DI.** A view is a class with fields + `render()`
  (wo-html's `Component`); a controller is a class satisfying `Handler`.
  Capture = a field.
- **The MVC seam is enforced by where the query sits.** Controllers hold
  every `from … select`; a view receives VALUES — for the list page, a
  `multi Component` of already-filled cards. No view in this template
  touches the database, and wo-html contains no query at all.
- **No sessions/cart yet.** Buying is per-product (qty → order). A cart
  needs a session story that does not exist yet.
- **No client-side JS.** Every interaction is a form round trip.
- **`pub` + `@table` cannot combine (recorded gap #1) — and it does not
  matter.** `pub` in front of an annotated class is a parse error
  (`WO-E101`), but no `@table` needs it: a CLASS is reachable across
  module lines without being exported. Verified 2026-08-25 by building
  and running all three shapes — a feature module querying a root
  `@table`, a feature module using a root plain class, and an `@table`
  declared inside a `types/` module and queried from the root. What IS
  module-scoped is a free `fn` (`WO-E210`), so a query shared by two
  features belongs on a class as a `static fn`. An earlier revision of
  this file claimed the gap forced controllers into the root module and
  made a `types/` module impossible; both were wrong, and the layout
  below is what the language actually allows.
- **`@view` projection classes (recorded gap #2):** today controllers
  copy row fields into view classes by hand. The wished-for form —
  `class ProductCard @view { ... }` filled by
  `from p in Product select p.name, p.price` — needs projection
  queries; recorded, not worked around.

## Judging the DX — what to look at

1. `types.wo` — the entire persistence layer is 20 lines.
2. `orders.controller.wo` — the whole buying flow (validate, stock
   check, decrement, durable insert, render) with no framework magic.
3. `orders/view.wo` — the referendum, now answered: three render()
   bodies, each one literal, no concatenation and no `esc()` calls, and
   `OrdersPage` holding its rows as child components.
4. `main.wo` — the app at a glance: five routes, one middleware, serve.

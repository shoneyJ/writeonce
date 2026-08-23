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

## The file map (Angular equivalents)

| this template | concern | Angular analog |
| --- | --- | --- |
| `types.wo` | MODEL — `@table` classes ARE the WAL database | `models/*.ts` (+ the entire database) |
| `layout/app.wo` | app shell: document, header+footer composition, `ok_html`/`html_error` transport helpers | `app.component.html` |
| `layout/header.wo` / `footer.wo` | shared chrome fragments | `header.html` / `footer.html` |
| `product_list/view.wo` | VIEW — classes with `fn render() -> Text`, fields = exactly what is displayed | `product-list/view.html` |
| `product_list.controller.wo` | CONTROLLER — query the model, fill the view, answer a `Resp` (one file per feature, root module) | component `.ts` + service |
| `product_page/`, `orders/` | one view module per feature + its root controller file | feature folders |
| `static_files/controller.wo` | `/assets/*` from disk, traversal-safe, typed | `angular.json` assets |
| `assets/style.css` | ONE real stylesheet, sectioned per feature | the `.scss` files |
| the `-- 37 target:` block above each `render()` | STORY 37's TARGET — an in-class raw template literal ({{ }} auto-escaped + type-checked, {!! !!} raw slots) replacing the hand-built body below it | Vue `<template>` (in-SFC) |
| `main.wo` | bootstrap: seed, routes, serve — nothing else | `app-routing.module.ts` + `main.ts` |

Separation is compiler-enforced where the language allows it today:
each feature's VIEW directory is a module — the root controllers see
only its `pub` classes and `layout`'s exports. Controllers themselves
sit in the root module beside `types.wo`, because of gap #1 below.

## What is deliberately different (doctrine)

- **Templates compile or they don't exist (story 37).** The target is
  an IN-CLASS raw template literal: `render()` returns one multi-line
  literal whose `{{ expr }}` holes are auto-escaped, compile-time-
  checked interpolations (typo'd field = compile error); `{!! !!}` is
  the raw slot for prebuilt fragments; structural control stays the
  language's `if`/`for` composing literals. Every `render()` here
  carries that target as the `-- 37 target:` comment above it — the
  body below is today's hand-lowering. NO template engine runs at
  request time — ever. Styles stay a real CSS file, served statically
  (there is no scss preprocessor).
- **No closures, no DI.** A view is a class with fields + `render()`;
  a controller is a class satisfying `Handler`. Capture = a field.
- **No sessions/cart yet.** Buying is per-product (qty → order). A cart
  needs a session story that does not exist yet.
- **No client-side JS.** Every interaction is a form round trip.
- **`pub` + `@table` cannot combine yet (recorded gap #1).** An
  annotated class cannot be exported, so a shared `types/` MODULE is
  impossible today — which is why the controllers live in the root
  module with `types.wo` instead of inside their feature folders. A
  one-clause grammar fix closes this; until then the template shows the
  honest layout.
- **`@view` projection classes (recorded gap #2):** today controllers
  copy row fields into view classes by hand. The wished-for form —
  `class ProductCard @view { ... }` filled by
  `from p in Product select p.name, p.price` — needs projection
  queries; recorded, not worked around.

## Judging the DX — what to look at

1. `types.wo` — the entire persistence layer is 20 lines.
2. `orders.controller.wo` — the whole buying flow (validate, stock
   check, decrement, durable insert, render) with no framework magic.
3. `orders/view.wo` — each `-- 37 target:` literal vs the hand-built
   body under it. The pair is the referendum: the literal is what
   writing a view will feel like, the body is what it costs today.
4. `main.wo` — the app at a glance: five routes, one middleware, serve.

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
| `*/view.html`, `layout/*.html` | STORY 37's TARGET — markup-first templates ({{ }} auto-escaped, w:if/w:for, {!! !!} slots) that woc will COMPILE against the view classes; inert today | `.component.html` / Vue `<template>` |
| `main.wo` | bootstrap: seed, routes, serve — nothing else | `app-routing.module.ts` + `main.ts` |

Separation is compiler-enforced where the language allows it today:
each feature's VIEW directory is a module — the root controllers see
only its `pub` classes and `layout`'s exports. Controllers themselves
sit in the root module beside `types.wo`, because of gap #1 below.

## What is deliberately different (doctrine)

- **Templates compile or they don't exist (story 37).** The `.html`
  files here are the TARGET: when 37 lands, `woc` compiles each against
  its view class (typo'd field = compile error) and the hand-written
  `render()` methods in `view.wo` disappear. Until then the `.wo` files
  are the running hand-lowering of exactly that markup, and NO template
  engine runs at request time — ever. Styles stay a real CSS file,
  served statically (there is no scss preprocessor).
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
3. `product_list/view.html` NEXT TO `product_list/view.wo` — the
   template 37 will compile vs today's hand-lowering. The pair is the
   referendum: the `.html` is what writing a view will feel like, the
   `.wo` is what it costs today.
4. `main.wo` — the app at a glance: five routes, one middleware, serve.

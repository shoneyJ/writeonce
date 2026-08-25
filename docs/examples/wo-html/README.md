# wo-html — server-rendered HTML as plain Text

A view library, not a framework and not a template engine. Everything in
it is a pure function or a class with a `render()`; nothing here opens a
socket, reads a file, or touches the database.

```
[deps]
html = { git = "https://github.com/shoneyj/wo-html", rev = "v0.1.0" }
```

## The four layers

| layer | what it is |
| --- | --- |
| `esc()` | HTML-escape `& < > "`. A `{{ }}` hole in a raw text literal compiles to a call to this, so display data is escaped by construction; calling it by hand is the fallback, not the norm |
| `el()`, `link()`, `card()`, `form_post()`, … | element builders — `el("h1", "text-3xl font-bold", t)` |
| `Component` / `Layout` / `render_all()` | the view unit and its composition |
| `tw_css()` / `page()` | a hand-written Tailwind-style utility sheet, inlined into one self-contained document — no CDN, no build step, no JS |

## Components

A component is a class with fields and `fn render() -> Text`. Nothing
declares that it implements `Component` — satisfaction is **structural**,
exactly like the framework's `Handler`. Its fields ARE its inputs; the
language has no closures, so a field is the capture.

```
class ProductCard {
  sku:  Text
  name: Text
  fn render() -> Text {
    return `
      <div class="card">
        <h3><a href="/p/{{ self.sku }}">{{ self.name }}</a></h3>
      </div>`;
  }
}
```

Composition is nesting — a parent holds children and calls their render:

```
pub class ProductListPage {
  cards: multi Component
  fn render() -> Text {
    return `<div class="grid">${render_all(self.cards)}</div>`;
  }
}
```

`multi Component` holds a heterogeneous list **directly**; no wrapper
record is needed (the framework's `Mw`/`Aw` wrappers are not a language
requirement). `render_all(cs)` renders children in order.

`Layout { title, nav, content, footer }` is content projection —
Angular's `<ng-content>` with the slots as ordinary pre-rendered `Text`.
The caller passes `child.render()`, a raw literal, or a builder's output;
the layout never learns which, which is precisely why it never needs the
child's type. A page wanting a fixed-width column wraps its content
before handing it over — deliberately no container knob here.

`Layout` renders through `page()`, so it inlines the utility sheet. An
app that links a real stylesheet instead writes its own two-slot shell
component — `docs/examples/shop/layout/app.wo` is that case, and it is a
component like any other.

## The MVC seam

| | where it lives |
| --- | --- |
| **Model** | `@table` rows, queried in the HANDLER. This library contains no `from … select` anywhere and must not grow one |
| **View** | components: fields in, Text out. No hidden state, no globals — a page is byte-deterministic from its fields |
| **Controller** | the framework's `Handler`: it queries, fills the component's fields, and answers `ok_html(c.render())` |

`ok_html` is the **framework's** (`framework/http`, beside `ok_text` and
`ok_json`): a status line plus a content-type is transport, not
rendering, so wo-html never learns what a `Resp` is.

The seam is what makes a view testable without a server and a query
testable without markup. Breaking it looks like one convenience — a
component that queries "just this once" — and costs both.

## Deliberately absent

Client-side anything (change detection, event bindings, two-way binding,
SPA routing, hydration): the no-JS posture stands, and interactivity is
form round trips. A runtime template engine: reflection-free means an
untyped `map<Text, Text>`, so templates compile to code at build time or
they do not exist. Dependency injection: components are data-in,
Text-out. Structural directives (`w:if` / `w:for`): the language's own
`if` and `for` compose literals, and no sample has yet proven the need
for a second control-flow dialect. Scoped CSS: the utility sheet stays
one static string until the pain is measured.

## Consumers

- [`docs/examples/site`](../site) — the tutorial site: `Layout` +
  a `ChapterNav` component reused on the homepage and every chapter page.
  Gated by `just site`.
- [`docs/examples/shop`](../shop/README.md) — the program template: its
  own `AppShell` component, page components holding child components.

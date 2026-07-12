scaffold sibling crates, multi-app ecommerce, REST + concurrency docs

- scaffold 14 placeholder crates (app, db, engine, gen, http, logic,
  policy, ql, service, sub, txn, ui, value, wal) — empty Cargo.toml +
  src/lib.rs to receive code phase-by-phase from `rt`
- restructure docs/examples/ecommerce into multi-app layout: apps/admin
  and apps/storefront, with shared/ types/logic/components, per-app
  app.wo + wo.toml, and reusable .htmlx components (layout, money,
  order-row)
- add reference/rest/{blog,ecommerce}.rest — VS Code/JetBrains HTTP
  request files driving the running prototype, including 501/404/405
  expectations for stubbed endpoints
- add docs/plan/09-concurrency-scaleout.md and docs/plan/ui/00-overview.md;
  refine docs/plan/assembly/02-writeonce-stance.md
- refresh templates (about, article, header/footer, home, layout, styles)
  and add static favicon/logo
- add infra/sync.sh and tighten .gitignore for reference/ symlinks

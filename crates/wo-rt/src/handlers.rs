use wo_htmlx::{self, Value};
use wo_http::response::Response;
use wo_route::RouteParams;
use wo_store::Store;
use wo_htmlx::TemplateRegistry;

/// Render the homepage: list of published articles.
pub fn handle_home(store: &Store, templates: &TemplateRegistry) -> Response {
    let articles = match store.list_published(0, 20) {
        Ok(a) => a,
        Err(e) => return Response::internal_error(&e.to_string()),
    };

    let articles_val: Vec<Value> = articles
        .iter()
        .map(|a| {
            let json = serde_json::to_value(a).unwrap_or_default();
            Value::from_json(&json)
        })
        .collect();

    let mut ctx = std::collections::BTreeMap::new();
    ctx.insert("articles".into(), Value::List(articles_val));
    ctx.insert("page_title".into(), Value::String("writeonce".into()));
    let context = Value::Object(ctx);

    render_page("home", &context, templates)
}

/// Render a single article page.
pub fn handle_article(params: &RouteParams, store: &Store, templates: &TemplateRegistry) -> Response {
    let sys_title = match params.get("sys_title") {
        Some(t) => t,
        None => return Response::not_found(),
    };

    let article = match store.get_by_title(sys_title) {
        Ok(Some(a)) => a,
        Ok(None) => return Response::not_found(),
        Err(e) => return Response::internal_error(&e.to_string()),
    };

    let json = serde_json::to_value(&article).unwrap_or_default();
    let mut ctx = std::collections::BTreeMap::new();
    ctx.insert("article".into(), Value::from_json(&json));
    ctx.insert("page_title".into(), Value::String(article.title.clone()));
    let context = Value::Object(ctx);

    render_page("article", &context, templates)
}

/// Render a tag listing page.
pub fn handle_tag(params: &RouteParams, store: &Store, templates: &TemplateRegistry) -> Response {
    let tag = match params.get("tag") {
        Some(t) => t,
        None => return Response::not_found(),
    };

    let articles = match store.list_by_tag(tag) {
        Ok(a) => a,
        Err(e) => return Response::internal_error(&e.to_string()),
    };

    let articles_val: Vec<Value> = articles
        .iter()
        .map(|a| {
            let json = serde_json::to_value(a).unwrap_or_default();
            Value::from_json(&json)
        })
        .collect();

    let mut ctx = std::collections::BTreeMap::new();
    ctx.insert("articles".into(), Value::List(articles_val));
    ctx.insert("page_title".into(), Value::String(format!("tag: {}", tag)));
    ctx.insert("tag".into(), Value::String(tag.to_string()));
    let context = Value::Object(ctx);

    render_page("home", &context, templates)
}

/// Render a static page (about, contact).
pub fn handle_static_page(name: &str, templates: &TemplateRegistry) -> Response {
    let mut ctx = std::collections::BTreeMap::new();
    ctx.insert("page_title".into(), Value::String(name.to_string()));
    let context = Value::Object(ctx);

    render_page(name, &context, templates)
}

/// Render a page template composed with layout, header, and footer.
fn render_page(template_name: &str, context: &Value, templates: &TemplateRegistry) -> Response {
    let page_nodes = match templates.get(template_name) {
        Some(nodes) => nodes,
        None => return Response::not_found(),
    };

    let partials = templates.partials();

    // Render the page content.
    let page_html = wo_htmlx::render(page_nodes, context, partials);

    // Compose with layout if it exists.
    let html = if let Some(layout_nodes) = templates.get("layout") {
        // Inject page content and header/footer.
        let header_html = templates
            .get("header")
            .map(|n| wo_htmlx::render(n, context, partials))
            .unwrap_or_default();
        let footer_html = templates
            .get("footer")
            .map(|n| wo_htmlx::render(n, context, partials))
            .unwrap_or_default();

        let mut layout_ctx = std::collections::BTreeMap::new();
        layout_ctx.insert("content".into(), Value::String(page_html));
        layout_ctx.insert("header".into(), Value::String(header_html));
        layout_ctx.insert("footer".into(), Value::String(footer_html));
        if let Value::Object(map) = context {
            for (k, v) in map {
                layout_ctx.insert(k.clone(), v.clone());
            }
        }
        let layout_context = Value::Object(layout_ctx);
        wo_htmlx::render(layout_nodes, &layout_context, partials)
    } else {
        page_html
    };

    Response::html(html)
}

# Markdown File Rendering

## Current State (writeonce-articles-s3)

Each article is a directory containing a JSON metadata file and one or more `.md` files:

```
auto-scale-gitlab-runner-using-aws-spot-instance/
  docker-machine-test-with-t2.md
  gitlab-runner-config.md
  stop-test-gitlab-docker-machine.md

gitlab-runner-with-kubernetes-executor/
  gitlab-runner-with-kubernetes-executor.json
  deploy.md
  permission.md
  role-binding.md
  role-defination.md
  gitlab-runnergitlab-runner-deploy.md
```

The JSON metadata currently defines the full article structure — sections, headings, paragraphs, and code snippet references. Markdown files are limited to code blocks referenced via the `codes[].snippet` field.

## Problem

The JSON metadata carries too much content. Headings, paragraphs, prose — all of this is duplicated as JSON strings inside `content.content.sections`. The markdown files only hold code snippets, referenced by `sectionIndex` and `paragraphIndex`.

This is backwards. The markdown file should be the content. The JSON should be minimal metadata.

## Target: Markdown-First Content Model

**The markdown file is the article.** All prose, headings, code blocks, and inline formatting live in the `.md` file. The JSON metadata file holds only what markdown cannot express: system fields, tags, publication state, and author.

### Minimal JSON Metadata

```json
{
  "sys_title": "gitlab-runner-with-kubernetes-executor",
  "title": "Gitlab Runner with Kubernetes Executor",
  "published": true,
  "author": "Shoney Arickathil",
  "tags": ["kubernetes", "gitlab", "ci-cd"],
  "published_on": 1740950884
}
```

No `content.content.sections`. No `content.content.codes`. No `paragraphs[]` arrays. No `sectionIndex`/`paragraphIndex` mapping.

### Markdown File = Full Article Content

````markdown
# Introduction

Deploying a Gitlab runner using kubernetes is a great option to overcome
the limitations of other gitlab runner executor such as docker and docker machine.

## Running Gitlab Runner in gitlab namespace

Create the namespace and apply the deployment:

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: gitlab-runner
  namespace: gitlab
```
````

## Permissions

The runner needs RBAC permissions to create pods:

```yaml
apiVersion: rbac.authorization.k8s.io/v1
kind: Role
metadata:
  name: gitlab-runner
```

Everything is in the markdown — headings, paragraphs, code blocks with language hints, links, images. The rendering pipeline parses the markdown directly.

### Directory Structure

```
content/
  gitlab-runner-with-kubernetes-executor/
    gitlab-runner-with-kubernetes-executor.json    # minimal metadata
    gitlab-runner-with-kubernetes-executor.md      # full article content
  linux-misc/
    linux-misc.json
    linux-misc.md
```

One JSON for metadata. One markdown for content. No scattered `.md` files per code snippet.

## What Changes

| Before                                                         | After                                                                   |
| -------------------------------------------------------------- | ----------------------------------------------------------------------- |
| JSON holds sections, headings, paragraphs as structured arrays | JSON holds only sys_title, title, published, author, tags, published_on |
| Markdown files hold only code snippets                         | Markdown file holds the entire article                                  |
| `codes[].snippet` maps filename to sectionIndex/paragraphIndex | No mapping needed — headings and code blocks are inline in markdown     |
| Renderer reads JSON structure, injects code from .md files     | Renderer parses markdown directly into HTML                             |
| Multiple .md files per article (one per code snippet)          | One .md file per article                                                |

## Impact on the Data Layer

### wo-model

The `Article` struct simplifies:

```rust
pub struct Article {
    pub sys_title: String,
    pub title: String,
    pub published: bool,
    pub author: String,
    pub tags: Vec<String>,
    pub published_on: Option<i64>,
}
```

The nested `ArticleContent` / `ArticleBody` / `Section` / `CodeSnippet` hierarchy is no longer needed. Article content comes from parsing the `.md` file at render time, not from the JSON.

### wo-md

Currently handles only inline markdown (`**bold**`, `` `code` ``, links). Needs to become a full markdown-to-HTML renderer:

- Block elements: headings (`#`, `##`), paragraphs, code fences (` `lang ```), lists, blockquotes
- Inline elements: bold, italic, code, links, images
- Code fence language extraction for `wo-md::highlight()`
- The renderer reads `{sys_title}/{sys_title}.md`, parses it, and returns HTML

### wo-htmlx

The `article.htmlx` template simplifies. Instead of iterating `{{#each article.content.content.sections}}`, it renders the pre-parsed markdown HTML:

```html
<article>
  <h1>{{article.title}}</h1>
  <p class="meta">by {{article.author}} &middot; {{article.tags}}</p>
  {{article.content_html}}
</article>
```

Where `content_html` is the full HTML output from the markdown renderer.

### wo-store

`ContentLoader` reads the `.json` for metadata and the `.md` for content. The `.seg` file stores both. At query time, the markdown is either:

- Pre-rendered to HTML during ingestion (stored in .seg alongside metadata)
- Rendered on-demand at request time (read .md from disk)

Pre-rendering is preferred — it avoids parsing markdown on every HTTP request.

## Migration Path

1. Update `wo-model` with the simplified `Article` struct
2. Extend `wo-md` to handle full markdown (block-level parsing, code fences)
3. Update `ContentLoader` to read `.json` + `.md` pairs
4. Update `wo-store` to store pre-rendered HTML in the .seg file
5. Simplify `article.htmlx` template
6. Migrate existing articles: extract prose from JSON into `.md` files

Existing articles with the old JSON format can coexist during migration — `ContentLoader` checks for a `.md` file and falls back to the JSON structure if none exists.

## Blog Subscription — Live Content Reload

When a user visits `http://localhost:3000/blog/sample-rust-patterns`, the content should stay live. Any edit to `sample-content/sample-rust-patterns/sample-rust-patterns.md` must auto-reflect in the browser without a page refresh.

### How It Works

```
Browser visits /blog/sample-rust-patterns
       │
       ▼
1. Server renders article HTML from .seg (pre-rendered from .md)
2. Server writes HTML response to socket fd
3. Server registers socket fd in subscription table:
       register!(sub_manager, socket_fd, ByTitle("sample-rust-patterns"))
4. Connection transitions to Subscribed state (stays open)
       │
       │  (user edits sample-rust-patterns.md)
       │
       ▼
5. inotify fires IN_MODIFY on sample-rust-patterns.md
6. ContentWatcher maps file → sys_title "sample-rust-patterns"
7. Store rebuilds: re-reads .json + .md, re-renders markdown to HTML, updates .seg + indexes
8. SubscriptionManager::notify("sample-rust-patterns", ...) fires
9. For each subscribed fd: write(fd, diff_payload)
       │
       ▼
10. Browser receives payload on the open connection
11. Client-side script applies the update to the DOM
```

### What Needs to Work

| Component | Requirement |
|-----------|-------------|
| **inotify** (wo-watch) | Already watches `content/` directory. `.md` file changes must trigger `ContentChange::Modified(sys_title)` |
| **Store rebuild** (wo-store) | On `.md` change: re-read file, re-render markdown to HTML, update `.seg` and indexes |
| **Subscription table** (wo-sub) | Route handler registers the browser's socket fd via `register!` after sending initial HTML |
| **Notification** (wo-sub) | On content change, write updated `content_html` to all subscribed fds as JSON payload |
| **Event loop** (wo-rt) | After writing initial response, transition connection to `Subscribed` state. Keep fd on epoll for hangup detection. |
| **Client script** | Injected in the HTML. Reads payloads from the open connection. Replaces article content in the DOM. |

### Client-Side Script

Injected by the template renderer into every article page:

```html
<script>
  // Connection stays open after initial HTML.
  // Server writes length-prefixed JSON payloads when content changes.
  const decoder = new TextDecoder();
  const articleEl = document.querySelector('article');

  fetch(window.location.href, { headers: { 'X-Subscribe': '1' } })
    .then(r => r.body.getReader())
    .then(reader => {
      (function read() {
        reader.read().then(({ done, value }) => {
          if (done) return;
          try {
            const payload = JSON.parse(decoder.decode(value));
            if (payload.content_html) {
              articleEl.innerHTML = payload.content_html;
            }
          } catch (e) {}
          read();
        });
      })();
    });
</script>
```

### inotify and .md Files

The current `ContentWatcher` watches for `.json` file changes. It must also trigger on `.md` file changes:

- `IN_MODIFY` on `*.md` → `ContentChange::Modified(sys_title)`
- The sys_title is derived from the parent directory name (same as for JSON)
- Both `.json` and `.md` changes trigger a store rebuild and subscriber notification

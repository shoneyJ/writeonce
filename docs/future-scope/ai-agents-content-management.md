# AI Agents and Content Management

## Context

AI agents (Claude Code, Copilot, Cursor, custom agents) work within a specific project or working directory. Their sessions, context, and understanding are scoped to that directory. This works well when projects are completely different domains.

But writeonce content is not isolated — articles reference each other, share tags, build on concepts from other articles. An agent editing `gitlab-runner-with-kubernetes-executor.md` would benefit from knowing that `auto-scale-gitlab-runner-using-aws-spot-instance.md` exists and covers related infrastructure. Without explicit mappings, the agent treats each article as an island.

## Problem

1. **Agents lack cross-article awareness.** When asked to write or update an article about Kubernetes, the agent doesn't know that related articles about Docker, CI/CD, or AWS already exist in the content directory — unless it manually searches.

2. **No semantic grouping.** Tags provide flat categorization (`kubernetes`, `ci-cd`), but they don't express relationships: "this article is a prerequisite for that one", "these three articles form a series", "this article supersedes that one."

3. **Context window waste.** Without mappings, the agent must scan all articles to find related content. With explicit mappings, it can load exactly the relevant files.

## Solution: Metadata-Driven Content Mappings

Users define relationships between articles in the JSON metadata. These mappings serve two purposes:

1. **Human navigation** — rendered as "related articles" links on the site
2. **Agent context** — when an agent works on an article, it loads the mapped articles into its context for cross-referencing

### Mapping Fields in JSON Metadata

Per [06-markdown-render.md](./06-markdown-render.md), the JSON metadata is minimal. Add a `mappings` field:

```json
{
  "sys_title": "gitlab-runner-with-kubernetes-executor",
  "title": "Gitlab Runner with Kubernetes Executor",
  "published": true,
  "author": "Shoney Arickathil",
  "tags": ["kubernetes", "gitlab", "ci-cd"],
  "published_on": 1740950884,
  "mappings": {
    "related": ["auto-scale-gitlab-runner-using-aws-spot-instance"],
    "prerequisite": ["linux-misc"],
    "series": {
      "name": "gitlab-runner",
      "order": 2
    }
  }
}
```

### Mapping Types

| Type           | Meaning                                         | Agent Use                                                                               |
| -------------- | ----------------------------------------------- | --------------------------------------------------------------------------------------- |
| `related`      | Topically related articles                      | Agent loads these for cross-reference when editing                                      |
| `prerequisite` | Articles the reader should read first           | Agent ensures no concept duplication, references prerequisites instead of re-explaining |
| `series`       | Articles that form an ordered sequence          | Agent maintains narrative continuity across the series                                  |
| `supersedes`   | This article replaces an older one              | Agent can mark the old article as outdated or unpublished                               |
| `references`   | External articles or URLs the content builds on | Agent checks links are still valid, cites them properly                                 |

### Directory Structure with Mappings

```
content/
  gitlab-runner-with-kubernetes-executor/
    gitlab-runner-with-kubernetes-executor.json    # metadata + mappings
    gitlab-runner-with-kubernetes-executor.md      # full article
  auto-scale-gitlab-runner-using-aws-spot-instance/
    auto-scale-gitlab-runner-using-aws-spot-instance.json
    auto-scale-gitlab-runner-using-aws-spot-instance.md
  linux-misc/
    linux-misc.json
    linux-misc.md
```

## Agent Workflows

### 1. Writing a New Article

The author asks an agent: "Write an article about deploying GitLab Runner on ECS."

The agent:

1. Scans the content directory for existing articles with tags `gitlab`, `ci-cd`, `aws`
2. Finds `gitlab-runner-with-kubernetes-executor` and `auto-scale-gitlab-runner-using-aws-spot-instance`
3. Reads their `.md` files to understand what's already covered
4. Writes the new article, referencing existing articles rather than re-explaining shared concepts
5. Suggests `mappings.related` entries for the new article's JSON

### 2. Updating an Existing Article

The author asks: "Update the Kubernetes executor article with the new runner token format."

The agent:

1. Reads the article's JSON metadata and `.md` content
2. Reads the `mappings.related` articles to check for consistency
3. Makes the update in the `.md` file
4. Checks if the change affects any prerequisite or series articles
5. inotify detects the `.md` change → store rebuilds → subscribers notified

### 3. Content Audit

The author asks: "Which articles reference outdated AWS configurations?"

The agent:

1. Loads all article metadata (the Store already indexes everything)
2. Follows `mappings` to build a dependency graph
3. Reads the `.md` files of articles tagged with `aws`
4. Identifies outdated patterns (old SDK versions, deprecated services)
5. Reports findings with links to specific articles and line numbers

### 4. Series Management

The author asks: "Add a new part to the gitlab-runner series."

The agent:

1. Finds all articles with `mappings.series.name == "gitlab-runner"`
2. Reads them in order to understand the narrative arc
3. Writes the new article continuing from where the series left off
4. Sets `mappings.series.order` to the next number
5. Updates the previous article's mappings to reference the new one

## Integration with writeonce Architecture

### Store Index

Add a mappings index alongside the existing title, date, and tag indexes:

```
data/
  articles.seg
  index/
    title.idx
    date.idx
    tags.idx
    mappings.idx    # sys_title → related sys_titles
```

The mappings index allows efficient traversal: "give me all articles related to X" without scanning every article's JSON.

### Template Rendering

The `article.htmlx` template can render related articles:

```html
<article>
  <h1>{{article.title}}</h1>
  {{article.content_html}} {{#each article.related}}
  <aside class="related">
    <h3>Related</h3>
    <ul>
      <li><a href="/blog/{{sys_title}}">{{title}}</a></li>
    </ul>
  </aside>
  {{/each}}
</article>
```

### Subscription

When a mapped article changes, subscribers to related articles can optionally be notified. If article A lists article B in `mappings.related`, and article B is updated, subscribers to article A can receive a notification that related content changed.

## Agent Configuration

For agents to use the mappings effectively, the project can include an agent instruction file (e.g., `CLAUDE.md` or `.agent/instructions.md`):

```markdown
## Content Management

- Articles are in `content/{sys_title}/{sys_title}.md`
- Metadata is in `content/{sys_title}/{sys_title}.json`
- Before writing or editing an article, read its `mappings` field and load related articles for context
- When creating a new article, suggest appropriate `mappings` based on tags and content overlap
- Maintain narrative continuity within `series` mappings
- Do not duplicate explanations that exist in `prerequisite` articles — reference them instead
```

This turns the content directory into an agent-navigable knowledge graph where the metadata provides the edges and the markdown files provide the nodes.

## queryable graph database

- traversable knowledge graphs available on RAM.
- which linux kernels, develop in C ++. User wants to learn it.

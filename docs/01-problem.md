# Problem Statement

The current writeonce architecture works, but it carries weight that the project doesn't need. This document identifies the structural problems that motivate the redesign described in [02-recovery.md](./02-recovery.md).

## Too Many Moving Parts

A single article edit touches five systems before it reaches a reader:

```
local file  -->  S3 bucket  -->  Lambda  -->  Rust API  -->  PostgreSQL  -->  Angular app
```

Each hop is a failure point. Each system has its own deployment, its own logs, its own configuration. The Lambda needs AWS credentials and an API token. The API needs a DATABASE_URL and an AWS_INFRA_BASE_URL. The infrastructure API needs its own AWS SDK config. For a platform that serves markdown files, this is disproportionate complexity.

## External Database for Derived Data

PostgreSQL stores articles as JSONB — but the database is not the source of truth. The articles repository is. The database is a derived cache that requires:

- A running PostgreSQL 17 instance
- Diesel ORM with migrations
- Connection pooling (r2d2)
- A separate Rust API server to mediate access

If the database dies, recovery means re-syncing every article from S3 through the Lambda pipeline. The database adds operational burden without adding authority.

## Five Repositories for One Product

The project spans five git submodules across four languages:

| Repo | Language | Purpose |
|------|----------|---------|
| writeonce-articles-s3 | JSON + MD | Content |
| lambda-function | Go | Sync trigger |
| writeonce-api | Rust | API server |
| aws-infra | Rust | AWS bridge |
| writeonce-app | Angular/TS | Frontend |

Each has its own CI/CD pipeline, Dockerfile, and deployment target. Coordinating changes across repos (e.g., adding a new article field) requires touching multiple codebases, multiple pipelines, and multiple deploys.

## No Real-Time Content Updates

The current flow is request-response only. When an article is updated:

1. Author syncs files to S3
2. Lambda fires and upserts via API
3. The frontend knows nothing until the next page load

There is no mechanism for the client to learn that content has changed. No subscriptions, no push, no invalidation. The architecture explicitly avoids WebSocket, but offers no alternative for real-time awareness.

## AWS Dependency for a File-Based System

The content is markdown and JSON files — inherently local, portable, and simple. But the current pipeline requires:

- An S3 bucket to host them
- A Lambda function to watch for changes
- AWS SDK configuration in two Rust services
- Pulumi infrastructure-as-code to manage the Lambda + S3 setup
- IAM credentials across multiple components

The cloud infrastructure exists to shuttle files from one place to another. The files themselves don't need the cloud — they need to be read, indexed, and served.

## Summary

The core problems are **accidental complexity** and **infrastructure overhead**. The content model (markdown + JSON metadata) is sound. The content-as-code principle is right. But the delivery mechanism — five repos, three languages on the backend, an external database, a cloud event pipeline — is heavier than the problem requires.

The question `02-recovery.md` answers: what if the database, the server, and the client were one thing?

# Manage Articles API

<img src="https://writeonce.de/assets/images/logo.png " width="200" alt="writeonce">

The **Manage Articles API** allows you to create, retrieve, and manage blog articles. This API is built to help with managing articles on a blog platform, allowing operations like fetching articles by title, pagination, and article creation.

## Base URL

api.writeonce.de

## Tech Stack

The **Manage Articles API** is built using the following technologies:

- **Rust**: The backend of the API is built with the Rust programming language for high performance and reliability.
- **Diesel**: Diesel is used as the ORM (Object Relational Mapper) to interact with the database.
- **PostgreSQL**: The database used for storing articles and related data.

## Cross build

CROSS_CONTAINER_ENGINE=docker cross build \
            --bin writeonce-manage-article-api \
            --release \
            --target aarch64-unknown-linux-gnu \
            --verbose

# Declare ARG in global scope
ARG VERSION=1.82.0

FROM rust:${VERSION} AS base
WORKDIR /usr/src/app

FROM base as build

# Install build dependencies

RUN apt-get update && apt-get install -y \
    curl \
    build-essential \
    pkg-config \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*


# Install and configure sccache to speed up compilation
RUN cargo install sccache
ENV RUSTC_WRAPPER="sccache"


# Cache dependencies to speed up builds
COPY Cargo.toml ./
RUN mkdir src && echo "fn main() {}" > src/main.rs
RUN cargo build --release && rm -rf target/release/deps/*

COPY . .

RUN cargo build --release --bin writeonce-manage-article-api

# Stage 2: Create a minimal runtime image
FROM debian:bookworm-slim as runtime

# Install necessary runtime dependencies
RUN apt-get update && apt-get install -y \
    ca-certificates \
    libc6 \
    libpq5 \
    && rm -rf /var/lib/apt/lists/*


COPY --from=build /usr/src/app/target/release/writeonce-manage-article-api /usr/local/bin/app
COPY doc /usr/local/share/doc

ARG PORT
ARG AWS_INFRA_BASE_URL
ARG DATABASE_URL
ARG API_ACCESS_TOKEN
ARG API_ACCESS_ADMIN_TOKEN
RUN echo $PORT

ENV PORT=$PORT
ENV AWS_INFRA_BASE_URL=$AWS_INFRA_BASE_URL
ENV API_ACCESS_TOKEN=$API_ACCESS_TOKEN
ENV API_ACCESS_ADMIN_TOKEN=$API_ACCESS_ADMIN_TOKEN
ENV DATABASE_URL=$DATABASE_URL

# Expose the application port
EXPOSE ${PORT}

WORKDIR /usr/local/share

# Run the application
CMD ["app"]

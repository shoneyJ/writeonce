# writeonce — task runner. `just --list` shows all recipes.

# serve the hello example (docs/examples/hello/main.wo) on :8080
hello:
    cargo run --bin wo -- run docs/examples/hello

# C runtime reference (prototypes/wo-rt-c): build, serve, CRUD round-trip, shut down
# Phase A: thread-per-core — each connection hashes to one shard (SO_REUSEPORT),
# so a list may land on a different shard than the create. The counters on /
# show the spread. WO_THREADS=4 keeps the demo output readable.
rt-c-demo port="8085" threads="4":
    #!/usr/bin/env bash
    set -euo pipefail
    make -C prototypes/wo-rt-c
    data=$(mktemp -d /tmp/wo-demo-XXXXXX)
    WO_PORT={{port}} WO_THREADS={{threads}} WO_DATA=$data ./prototypes/wo-rt-c/wo-rt &
    server=$!
    trap 'kill $server 2>/dev/null; sleep 0.3; rm -rf $data' EXIT
    base=http://127.0.0.1:{{port}}
    for _ in $(seq 1 40); do curl -s "$base/healthz" >/dev/null && break; sleep 0.25; done
    echo
    echo "--- runtime:"; curl -s "$base/"; echo
    echo "--- create x4 (each connection may hash to a different shard):"
    for i in 1 2 3 4; do curl -s -X POST "$base/api/notes" -d '{"title":"note '$i'"}'; echo; done
    echo "--- list (the connection's own shard only — shared-nothing):"
    curl -s "$base/api/notes"; echo
    echo "--- spread:"; curl -s "$base/"; echo

# phase-F benchmark: reads, durable writes, 10k idle conns (scaled geometry)
rt-c-bench port="8085" threads="8" conns="64":
    #!/usr/bin/env bash
    set -euo pipefail
    make -C prototypes/wo-rt-c clean >/dev/null
    make -C prototypes/wo-rt-c CFLAGS="-O2 -Wall -Wextra -std=c11 -DSLOTS_PER_SHARD=262144" wo-rt bench >/dev/null
    data=$(mktemp -d /tmp/wo-bench-XXXXXX)
    WO_PORT={{port}} WO_THREADS={{threads}} WO_DATA=$data ./prototypes/wo-rt-c/wo-rt >/dev/null 2>&1 &
    server=$!
    trap 'kill $server 2>/dev/null; sleep 0.3; rm -rf $data; make -C prototypes/wo-rt-c clean >/dev/null; make -C prototypes/wo-rt-c wo-rt bench >/dev/null' EXIT
    base=127.0.0.1; for _ in $(seq 1 40); do curl -s "http://$base:{{port}}/healthz" >/dev/null && break; sleep 0.25; done
    B=./prototypes/wo-rt-c/bench/bench
    echo "wo-rt-c ({{threads}} shards, durable WAL):"
    $B $base {{port}} {{conns}} 5 /healthz
    $B $base {{port}} {{conns}} 5 /
    $B $base {{port}} {{conns}} 3 /api/notes '{"title":"bench"}'
    $B $base {{port}} 10000 0 /healthz

# serve the pricing demo (class model — docs/examples/pricing) on :8080
pricing:
    cargo run --bin wo -- run docs/examples/pricing

# class model in action: serve pricing, CRUD round-trip on Product, shut down
pricing-demo port="8092":
    #!/usr/bin/env bash
    set -euo pipefail
    cargo build --bin wo
    WO_LISTEN=127.0.0.1:{{port}} ./target/debug/wo run docs/examples/pricing &
    server=$!
    trap 'kill $server 2>/dev/null' EXIT
    base=http://127.0.0.1:{{port}}
    for _ in $(seq 1 40); do curl -s "$base/healthz" >/dev/null && break; sleep 0.25; done
    echo
    echo "--- create:";  curl -s -X POST "$base/api/products" -H 'Content-Type: application/json' -d '{"sku":"WO-001","name":"writeonce mug"}'; echo
    echo "--- list:";    curl -s "$base/api/products"; echo
    echo "--- patch 1:"; curl -s -X PATCH "$base/api/products/1" -d '{"name":"writeonce mug v2"}'; echo
    echo "--- live (13c pending, expect 501):"; curl -s -o /dev/null -w '%{http_code}\n' "$base/api/products/live"
    echo "--- delete 1 (expect 204):";          curl -s -X DELETE "$base/api/products/1" -o /dev/null -w '%{http_code}\n'

# main.wo in action: serve hello, run the full CRUD round-trip, shut down
hello-demo port="8090":
    #!/usr/bin/env bash
    set -euo pipefail
    cargo build --bin wo
    WO_LISTEN=127.0.0.1:{{port}} ./target/debug/wo run docs/examples/hello &
    server=$!
    trap 'kill $server 2>/dev/null' EXIT
    base=http://127.0.0.1:{{port}}
    for _ in $(seq 1 40); do curl -s "$base/healthz" >/dev/null && break; sleep 0.25; done
    echo
    echo "--- create:";            curl -s -X POST "$base/api/notes" -H 'Content-Type: application/json' -d '{"title":"hello","body":"# First note"}'; echo
    echo "--- list:";              curl -s "$base/api/notes"; echo
    echo "--- get 1:";             curl -s "$base/api/notes/1"; echo
    echo "--- patch 1:";           curl -s -X PATCH "$base/api/notes/1" -d '{"pinned":true}'; echo
    echo "--- live (Stage 3 stub, expect 501):"; curl -s -o /dev/null -w '%{http_code}\n' "$base/api/notes/live"
    echo "--- delete 1 (expect 204):";           curl -s -X DELETE "$base/api/notes/1" -o /dev/null -w '%{http_code}\n'
    echo "--- list after delete:"; curl -s "$base/api/notes"; echo

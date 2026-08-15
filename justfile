# writeonce — task runner. `just --list` shows all recipes.

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

# woc compiler front (compiler/): build the executable
woc-build:
    dune build --root compiler

# woc gate: unit tests (test_diag) + golden suite (runner, WOC_BLESS=1 to update)
woc-test:
    dune runtest --root compiler

# wovm (runtime/): build the plain, optimized binary — the release build,
# no sanitizer (wovm-test is the ASan-clean gate; `make -C runtime wovm-asan`
# builds a separate sanitized binary for the corpus, see oop-accept)
wovm-build:
    make -C runtime wovm

# wovm gate: unit suites (both dispatch flavors: computed-goto + ISO switch
# under -DWO_ISO_C, so neither rots) + CLI smoke, all ASan+UBSan
wovm-test:
    make -C runtime test
    make -C runtime test-iso
    bash runtime/test/cli_smoke.sh

# the sample workload's own recipes live beside it (build from wo.toml,
# the acceptance test, the soak): `just log-watcher` runs the acceptance,
# `just log-watcher::build` / `::soak 60` the rest — see the module's
# justfile for what each does. This is the test the whole track exists to
# pass; the corpus below gates the individual behaviors underneath it.
mod log-watcher "docs/examples/log-watcher"

# conformance harness (plan 3): walks tests/corpus/{run,compile-fail,trap},
# exact outcome per fixture kind — see docs/plan/oop-vm/02-corpus.md.
# Fails loudly (and names the recipe to run) if woc or wovm isn't built.
oop-e2e:
    ./scripts/oop-e2e.sh

# milestone-1 acceptance gate (docs/superpowers/specs/2026-08-01-oop-compiler-vm-design.md
# "Success criteria"): the five spec criteria plus both unit gates, one
# command, in the spec's order. Fails loudly on the first failing stage,
# names the criterion, exits nonzero — a measurement that only prints is
# not a gate.
oop-accept:
    #!/usr/bin/env bash
    set -uo pipefail
    ROOT="$(pwd)"
    fail() { echo "oop-accept: FAILED -- $1" >&2; exit 1; }
    WORK="$(mktemp -d "${TMPDIR:-/tmp}/oop-accept.XXXXXX")"
    trap 'rm -rf "$WORK"' EXIT

    echo "=== criterion 1: woc compile time, pricing subset (budget: under 100ms) ==="
    dune build --root compiler || fail "criterion 1: dune build --root compiler"
    WOC="$ROOT/compiler/_build/default/bin/woc"
    PRICING="tests/corpus/run/pricing-containers/fixture.wo tests/corpus/run/pricing-current-price/fixture.wo tests/corpus/run/pricing-discounted/fixture.wo tests/corpus/run/pricing-text/fixture.wo tests/corpus/trap/pricing-set-price-db-stub/fixture.wo"
    N=20
    total_ns=0; max_ns=0; min_ns=""
    for i in $(seq 1 "$N"); do
      start=$(date +%s%N)
      for f in $PRICING; do
        "$WOC" --emit "$f" -o "$WORK/pricing.wob" || fail "criterion 1: woc --emit $f"
      done
      end=$(date +%s%N)
      elapsed=$((end - start))
      total_ns=$((total_ns + elapsed))
      [[ -z "$min_ns" || elapsed -lt min_ns ]] && min_ns=$elapsed
      [[ elapsed -gt max_ns ]] && max_ns=$elapsed
    done
    avg_ms=$(awk -v t="$total_ns" -v n="$N" 'BEGIN{printf "%.3f", t/n/1000000}')
    min_ms=$(awk -v t="$min_ns" 'BEGIN{printf "%.3f", t/1000000}')
    max_ms=$(awk -v t="$max_ns" 'BEGIN{printf "%.3f", t/1000000}')
    echo "  woc --emit over all 5 pricing-subset fixtures, $N runs: min ${min_ms}ms avg ${avg_ms}ms max ${max_ms}ms"
    [[ "$max_ns" -lt 100000000 ]] || fail "criterion 1: worst case ${max_ms}ms meets/exceeds the 100ms budget"
    echo "  criterion 1: MET"

    echo "=== criteria 2-4: full conformance corpus under ASan (runtime/build/wovm_asan) ==="
    make -C runtime wovm || fail "criteria 2-4: make -C runtime wovm"
    make -C runtime wovm-asan || fail "criteria 2-4: make -C runtime wovm-asan"
    cp "$ROOT/runtime/wovm" "$WORK/wovm.release"
    restore_wovm() { cp "$WORK/wovm.release" "$ROOT/runtime/wovm"; }
    echo "  swapping runtime/wovm -> runtime/build/wovm_asan for this stage only (oop-e2e.sh has no --wovm override)"
    cp "$ROOT/runtime/build/wovm_asan" "$ROOT/runtime/wovm"
    if ./scripts/oop-e2e.sh; then
      restore_wovm
    else
      restore_wovm
      fail "criteria 2-4: conformance corpus failed under ASan (see output above)"
    fi
    echo "  runtime/wovm restored to the release binary"
    echo "  criteria 2-4: MET"

    echo "=== criterion 5: single-binary smoke (runtime/wovm, release binary) ==="
    ./scripts/single-binary-smoke.sh || fail "criterion 5: single-binary smoke"
    echo "  criterion 5: MET"

    echo "=== unit gate: runtime (both dispatch flavors + cli_smoke) ==="
    make -C runtime test || fail "runtime unit gate: make -C runtime test"
    make -C runtime test-iso || fail "runtime unit gate: make -C runtime test-iso"
    bash runtime/test/cli_smoke.sh || fail "runtime unit gate: cli_smoke"
    echo "  runtime unit gate: MET"

    echo "=== unit gate: compiler ==="
    dune runtest --root compiler || fail "compiler unit gate: dune runtest --root compiler"
    echo "  compiler unit gate: MET"

    echo
    echo "oop-accept: ALL CRITERIA MET"

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

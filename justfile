# writeonce — task runner. `just --list` shows all recipes.

# docs gate: every relative markdown link resolves, every #anchor exists.
# Report lands in docs/00-link-audit.md; this recipe is the re-check.
linkcheck:
    python3 scripts/linkcheck.py .

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

# dist: package the toolchain as an installable, Go-style tarball —
# writeonce-<ver>-linux-amd64.tar.gz whose `writeonce/bin/` holds woc + wovm.
# The version is single-sourced from ./VERSION and asserted against both
# binaries (drift guard). See scripts/mkdist.sh.
dist:
    ./scripts/mkdist.sh

# deps-accept: iteration 15's gate — wo.toml [deps] + git fetch + wo.lock,
# proven against local file:// remotes built at run time (network-free):
# cold fetch, offline rebuild, lock-beats-moved-tag, --update-deps, drift/
# transitive/collision diagnostics, dep-main-never-entry, manifest shape.
deps-accept:
    ./scripts/deps-accept.sh

# web-app: iteration 16's gate — the whole chain at run time, network-free:
# a temp git remote from docs/examples/porch, file:// URL
# substituted into a temp copy of docs/examples/web-app, then fetch -> lock
# -> build -> serve -> the storefront matrix (auth/CRUD/@unique/FK/404/400/
# pipelining) -> SIGTERM -> WAL restart persistence.
web-app:
    ./scripts/web-app-accept.sh

# site: the writeonce.de tutorial (docs/examples/site) — TWO deps
# (porch + view), server-rendered pages, page matrix -> 401/edit
# -> SIGTERM -> WAL restart persistence. `just site` runs it.
site:
    ./scripts/site-accept.sh

# fibers: the hybrid-scheduler demo (docs/examples/fibers) — part 1 byte-
# exact budget interleave, part 2 parked-sleeper-blocks-nobody, on the
# uring AND epoll backends plus an ASan run.
fibers:
    ./scripts/fibers-accept.sh

# chat: iteration 24's gate (docs/examples/chat) — rooms/presence/broadcast
# over WebSocket via actors: functional on both WO_IO backends, the
# 1k-clients-one-hot-room soak (fds/RSS accounted), SIGTERM drain with
# close frames, and an ASan leg. `just chat` runs it (CHAT_SOAK=N trims).
chat:
    ./scripts/chat-accept.sh

# db-actor: arc stage 3's gate (docs/examples/db-actor) — worker-shard
# actors read/write the database through the transparent DB actor; WAL
# replay pair included. `just db-actor` runs it.
db-actor:
    ./scripts/db-actor-accept.sh

# residency: databasev2 2's gate — per-table storage. The corpus covers the
# in-process half; this covers what one process cannot see: a volatile table
# empty after restart while its durable sibling replays, volatile inserts
# writing ZERO WAL bytes (measured against the fallocate'd file's non-zero
# prefix, since its size proves nothing), the mode-mismatch startup refusal,
# and the two compile-time refusals.
residency:
    ./scripts/residency-accept.sh

# subprocess: iteration 42's gate (docs/examples/subprocess) — proc.run
# from .wo end to end: defaults, deadline and cap traps caught with
# try/catch in the language, a parked run blocking no other request, and
# the SIGTERM drain leaving no child behind. Log: /tmp/subprocess.log.
subprocess:
    ./scripts/subprocess-accept.sh

# tls: runtime-v2 9 F3c-net's gate (docs/examples/tls-client) — net.connect_tls
# from .wo end to end against a local TLS 1.3 stub with a test CA: the
# hand-rolled handshake + chain/hostname validation + an app round-trip, plus
# the untrusted-chain and hostname-mismatch negatives refused. Log: /tmp/tls.log.
tls:
    ./scripts/tls-accept.sh

# tls-server: runtime-v2 9 phase G's gate (docs/examples/tls-server) —
# net.accept_tls from .wo terminating TLS 1.3 itself, proven by openssl
# s_client (EC + RSA server certs) validating the hand-rolled handshake and
# getting the reply. No front proxy. Log: /tmp/tls-server.log.
tls-server:
    ./scripts/tls-server-accept.sh

# db-bench: iteration 22's campaign (docs/examples/db-bench) — OFF the
# fast path, minutes long: ram+durable x 1/N shards, durability legs,
# gates vs bench/baseline.json. quick = seconds, floors only.
db-bench:
    ./scripts/db-bench.py

db-bench-quick:
    ./scripts/db-bench.py --quick

# install-accept: extract the dist tarball to a temp prefix, PATH it, and prove
# `woc version` + a from-scratch project build+run (self-located wovm) + the
# wo-constraint refusal all work — the "tarball install actually works" gate.
install-accept:
    ./scripts/install-accept.sh

# the sample workload's own recipes live beside it (build from wo.toml,
# the acceptance test, the soak): `just log-watcher` runs the acceptance,
# `just log-watcher::build` / `::soak 60` the rest — see the module's
# justfile for what each does. This is the test the whole track exists to
# pass; the corpus below gates the individual behaviors underneath it.
mod log-watcher "docs/examples/log-watcher"

# the database track's acceptance workload (iteration 9/9b): @table storage,
# ref/backlink relations + FK restrict, and the compiler-checked query surface
# (scan/where/select/order/take, update, delete). `just employee` runs it.
mod employee "docs/examples/employee"

# iteration 9g corpus: skillhost's embedded-SQLite catalog translated to the
# writeonce query surface (proves it needs no new grammar). `just skill-catalog`
mod skill-catalog "docs/examples/skill-catalog"

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
    PRICING="tests/corpus/run/pricing-containers/fixture.wo tests/corpus/run/pricing-current-price/fixture.wo tests/corpus/run/pricing-discounted/fixture.wo tests/corpus/run/pricing-text/fixture.wo tests/corpus/run/pricing-set-price-insert/fixture.wo"
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

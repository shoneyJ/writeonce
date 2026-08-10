#!/usr/bin/env bash
# cli_smoke — end-to-end check of the wovm CLI exit-code contract:
#   0 = success, 1 = trap (one stderr line: "trap CODE in METHOD at line N:
#   MESSAGE"), 2 = usage/load failure. Fixtures come from mkwob.
set -u
cd "$(dirname "$0")/.." || exit 1

make wovm build/mkwob >/dev/null || { echo "cli_smoke: build failed"; exit 1; }
./build/mkwob build || { echo "cli_smoke: mkwob failed"; exit 1; }

# success path: stdout diffed, exit 0
out=$(./wovm build/hello.wob)
rc=$?
[ "$rc" -eq 0 ] || { echo "cli_smoke: hello exit $rc, want 0"; exit 1; }
expected=$'hello, wovm\n42'
[ "$out" = "$expected" ] || { echo "cli_smoke: unexpected stdout: $out"; exit 1; }

# trap path: exit 1, fixed stderr shape
./wovm build/trap.wob >/dev/null 2>build/trap.err
rc=$?
[ "$rc" -eq 1 ] || { echo "cli_smoke: trap exit $rc, want 1"; exit 1; }
grep -q '^trap 1 in main at line 7: division by zero$' build/trap.err ||
    { echo "cli_smoke: bad trap line: $(cat build/trap.err)"; exit 1; }

# load-failure path: exit 2
./wovm build/does-not-exist.wob 2>/dev/null
[ $? -eq 2 ] || { echo "cli_smoke: missing-file exit not 2"; exit 1; }
./wovm 2>/dev/null
[ $? -eq 2 ] || { echo "cli_smoke: usage exit not 2"; exit 1; }

echo "cli_smoke: OK"

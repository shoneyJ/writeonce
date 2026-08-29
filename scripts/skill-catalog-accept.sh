#!/usr/bin/env bash
# scripts/skill-catalog-accept.sh — iteration 9g acceptance: every skillhost
# catalog operation, translated to writeonce, produces the expected result on
# the 9b query surface (no new grammar). WAL-durable; a second seed proves the
# @unique trap persists across a process restart.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WOC="$ROOT/compiler/_build/default/bin/woc"
WOVM="$ROOT/runtime/wovm"
pass=0; fail=0
ok() { echo "ok   $1"; pass=$((pass+1)); }
bad() { echo "FAIL $1 -- $2"; fail=$((fail+1)); }
[[ -x "$WOC" && -x "$WOVM" ]] || { echo "build woc+wovm first" >&2; exit 1; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/skillcat.XXXXXX")"; DATA="$WORK/d"; mkdir -p "$DATA"
IMG="$WORK/sc.wob"
trap '[[ -n "${SKILLCAT_KEEP:-}" ]] && echo "kept $WORK" || rm -rf "$WORK"' EXIT

if "$WOC" --emit "$ROOT/docs/examples/skill-catalog" -o "$IMG" >"$WORK/c.out" 2>&1; then
  ok "compile ($(stat -c%s "$IMG") bytes)"
else bad "compile" "$(head -1 "$WORK/c.out")"; echo; echo "skill-catalog-accept: $pass/$((pass+fail))"; exit 1; fi
run() { WO_DATA="$DATA" "$WOVM" "$IMG" "$@"; }

run seed | grep -q "SEEDED 4 skills" && ok "seed (insert)" || bad "seed" "no SEEDED"
[[ "$(run count)" == "4" ]] && ok "count (COUNT(*) -> count(query))" || bad "count" "not 4"
[[ "$(run list | head -1)" == docker* ]] && ok "list (ORDER BY name)" || bad "list" "not name-ordered"
if [[ "$(run roots | tr '\n' ',')" == "docker,git-commit,git-rebase," ]]; then
  ok "roots (NOT EXISTS -> backlink emptiness)"
else bad "roots" "got: $(run roots | tr '\n' ',')"; fi
run get git | grep -q "^git	version control" && ok "get by name (WHERE name = ?)" || bad "get" "wrong row"
out="$(run seed 2>&1)"; rc=$?
[[ "$out" == *SEED-DUP* && $rc -eq 3 ]] && ok "unique trap persists across restart" || bad "dup" "rc=$rc"
echo
printf 'skill-catalog-accept: %d checks, %d failures\n' "$((pass+fail))" "$fail"
[[ $fail -eq 0 ]]

#!/usr/bin/env bash
# scripts/employee-accept.sh — the database track's acceptance workload.
#
# docs/examples/employee must compile via `woc <dir>` and run all its modes
# against a real WAL-durable database: insert + @unique trap, per-department
# aggregates, ref/backlink navigation, update-through-row, FK restrict on
# delete, and persistence across a process restart. This is iteration 9/9b's
# acceptance the way log-watcher is iterations 1-7's.
#
# Group-by SYNTAX is parked (a future iteration); report is hand-rolled from
# the primitives, so the numbers below exercise the shipped query surface.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WOC="$ROOT/compiler/_build/default/bin/woc"
WOVM="$ROOT/runtime/wovm"
SAMPLE="$ROOT/docs/examples/employee"

pass=0
fail=0
ok() { echo "ok   $1"; pass=$((pass + 1)); }
bad() { echo "FAIL $1 -- $2"; fail=$((fail + 1)); }

if [[ ! -x "$WOC" || ! -x "$WOVM" ]]; then
  echo "employee-accept: build woc and wovm first (just woc-build; just wovm-build)" >&2
  exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/emp-accept.XXXXXX")"
DATA="$WORK/data"
mkdir -p "$DATA"
IMG="$WORK/employee.wob"
cleanup() { [[ -n "${EMP_ACCEPT_KEEP:-}" ]] && echo "kept $WORK" || rm -rf "$WORK"; }
trap cleanup EXIT

# ---- 1. compile ------------------------------------------------------
if "$WOC" --emit "$SAMPLE" -o "$IMG" >"$WORK/compile.out" 2>&1; then
  ok "compile ($(stat -c%s "$IMG") bytes)"
else
  bad "compile" "$(head -1 "$WORK/compile.out")"
  echo; printf 'employee-accept: %d checks, %d failures\n' "$((pass + fail))" "$fail"; exit 1
fi

run() { WO_DATA="$DATA" "$WOVM" "$IMG" "$@"; }

# ---- 2. seed (insert + WAL) ------------------------------------------
if run seed 2>&1 | grep -q "^SEEDED 3 departments, 6 employees"; then
  ok "seed (insert, WAL-durable)"
else
  bad "seed" "no SEEDED line"
fi

# ---- 3. seed again: @unique trap, caught, across a process boundary --
out="$(run seed 2>&1)"; rc=$?
if [[ "$out" == *"SEED-DUP"* && $rc -eq 3 ]]; then
  ok "unique violation caught on re-seed (persisted via replay)"
else
  bad "unique re-seed" "got rc=$rc: $(printf '%s' "$out" | tr '\n' '|' | cut -c1-100)"
fi

# ---- 4. report: per-department aggregates + payroll ------------------
rep="$(run report 2>&1)"
if [[ "$rep" == *"DEPT Engineering headcount=3 avg=8200000 min=7300000 max=9200000"* \
   && "$rep" == *"DEPT Operations headcount=2 avg=6150000 min=5900000 max=6400000"* \
   && "$rep" == *"PAYROLL 45700000"* ]]; then
  ok "report (aggregates + payroll)"
else
  bad "report" "$(printf '%s' "$rep" | tr '\n' '|' | cut -c1-160)"
fi

# ---- 5. staff: index probe + backlink + ref navigation --------------
st="$(run staff Engineering 2>&1)"
if [[ "$st" == *"STAFF Asha 9200000 (Engineering)"* \
   && "$st" == *"STAFF Chidi 7300000 (Engineering)"* ]]; then
  ok "staff (unique probe + backlink scan + ref nav)"
else
  bad "staff" "$(printf '%s' "$st" | tr '\n' '|' | cut -c1-160)"
fi

# ---- 6. raise: update-through-row, reflected in a re-report ----------
run raise Operations 5 >/dev/null 2>&1
if run report 2>&1 | grep -q "^DEPT Operations headcount=2 avg=6457500"; then
  ok "raise (update-through-row, durable)"
else
  bad "raise" "operations average did not move to 6457500"
fi

# ---- 7. drop: FK restrict (Engineering still has staff) --------------
out="$(run drop Engineering 2>&1)"; rc=$?
if [[ "$out" == *"restricted"* && $rc -eq 4 ]]; then
  ok "drop restricted by FK (department has staff)"
else
  bad "drop restrict" "got rc=$rc: $(printf '%s' "$out" | tr '\n' '|' | cut -c1-100)"
fi

# ---- 8. persistence: a fresh process still sees every acked write ----
if run report 2>&1 | grep -q "^PAYROLL 46315000"; then
  ok "persistence (replay: raised payroll survives restart)"
else
  bad "persistence" "payroll after restart not 46315000"
fi

echo
printf 'employee-accept: %d checks, %d failures\n' "$((pass + fail))" "$fail"
[[ $fail -eq 0 ]]

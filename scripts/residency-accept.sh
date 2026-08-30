#!/usr/bin/env bash
# scripts/residency-accept.sh — databasev2 2's gate: per-table storage.
#
# The corpus proves the in-process half (tests/corpus/run/table-volatile-inprocess,
# .../table-residency-legal, .../compile-fail/table-durable-ref-volatile). This
# script proves the half a single process cannot observe:
#   * a volatile table is EMPTY after a restart while its durable sibling replays
#   * volatile inserts write ZERO bytes to the WAL — measured, not asserted,
#     because the file is fallocate'd to 1 MiB up front so its SIZE proves
#     nothing; what is measured is the non-zero prefix actually written
#   * a mode mismatch (log holds records for a table the source now declares
#     volatile) REFUSES to start, exits 2, and names the class
#   * the compile-time refusals still fire
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
WOC="compiler/_build/default/bin/woc"
WOVM="runtime/wovm"
pass=0; fail=0
ok()  { echo "ok   $1"; pass=$((pass + 1)); }
bad() { echo "FAIL $1 -- $2"; fail=$((fail + 1)); }

if [[ ! -x "$WOC" || ! -x "$WOVM" ]]; then
  echo "residency-accept: build woc and wovm first (just woc-build && just wovm-build)" >&2
  exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/residency-accept.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# non-zero prefix of a fallocate'd WAL = bytes actually written
wal_bytes() {
  python3 -c "import sys;d=open(sys.argv[1],'rb').read();print(len(d.rstrip(b'\x00')))" "$1"
}

# ---- 1. restart: durable replays, volatile does not -----------------------
cat > "$WORK/mix.wo" <<'EOF'
@table(name: "kept", index: [k])
class Kept { k: Text }
@table(name: "scratch", durable: false, index: [k])
class Scratch { k: Text }
fn main(args: multi Text) -> Int {
  if len(args) > 0 and args[0] == "seed" {
    insert Kept { k: "a" };
    insert Scratch { k: "b" };
    return 0;
  }
  let nk = 0;
  for x in from r in Kept select r { nk = nk + 1; }
  let ns = 0;
  for x in from r in Scratch select r { ns = ns + 1; }
  print("kept=${nk} scratch=${ns}");
  return 0;
}
EOF
if "$WOC" --emit "$WORK/mix.wo" -o "$WORK/mix.wob" 2>"$WORK/e"; then
  mkdir -p "$WORK/d1"
  WO_DATA="$WORK/d1" "$WOVM" "$WORK/mix.wob" seed >/dev/null 2>&1
  got="$(WO_DATA="$WORK/d1" "$WOVM" "$WORK/mix.wob" 2>&1)"
  [[ "$got" == "kept=1 scratch=0" ]] \
    && ok "restart: durable row replays, volatile row is gone" \
    || bad "restart: durable replays, volatile gone" "got: $got"
else
  bad "restart fixture compiles" "$(head -1 "$WORK/e")"
fi

# ---- 2. the write-path saving, measured ------------------------------------
cat > "$WORK/only.wo" <<'EOF'
@table(name: "dur", index: [k])
class Dur { k: Text }
@table(name: "vol", durable: false, index: [k])
class Vol { k: Text }
fn main(args: multi Text) -> Int {
  let n = 0;
  while n < 50 {
    if args[0] == "dur" { insert Dur { k: "x" }; } else { insert Vol { k: "x" }; }
    n = n + 1;
  }
  return 0;
}
EOF
if "$WOC" --emit "$WORK/only.wo" -o "$WORK/only.wob" 2>"$WORK/e"; then
  for m in dur vol; do
    mkdir -p "$WORK/w_$m"
    WO_DATA="$WORK/w_$m" "$WOVM" "$WORK/only.wob" "$m" >/dev/null 2>&1
  done
  db="$(wal_bytes "$WORK/w_dur/shard-0.wal")"
  vb="$(wal_bytes "$WORK/w_vol/shard-0.wal")"
  [[ "$vb" -eq 0 ]] \
    && ok "50 volatile inserts write 0 WAL bytes (durable wrote $db)" \
    || bad "volatile inserts write nothing" "volatile wrote $vb bytes"
  [[ "$db" -gt 0 ]] \
    && ok "50 durable inserts do write to the WAL ($db bytes)" \
    || bad "durable inserts still log" "durable wrote $db bytes"
else
  bad "measurement fixture compiles" "$(head -1 "$WORK/e")"
fi

# ---- 3. mode mismatch refuses, exits 2, names the class --------------------
printf '@table(name: "orders", index: [k])\nclass Orders { k: Text }\nfn main() -> Int { insert Orders { k: "a" }; return 0; }\n' > "$WORK/wasdur.wo"
printf '@table(name: "orders", durable: false, index: [k])\nclass Orders { k: Text }\nfn main() -> Int { print("started"); return 0; }\n' > "$WORK/nowvol.wo"
if "$WOC" --emit "$WORK/wasdur.wo" -o "$WORK/a.wob" 2>/dev/null \
   && "$WOC" --emit "$WORK/nowvol.wo" -o "$WORK/b.wob" 2>/dev/null; then
  mkdir -p "$WORK/d3"
  WO_DATA="$WORK/d3" "$WOVM" "$WORK/a.wob" >/dev/null 2>&1
  out="$(WO_DATA="$WORK/d3" "$WOVM" "$WORK/b.wob" 2>&1)"; rc=$?
  [[ $rc -eq 2 ]] \
    && ok "mode mismatch exits 2 (refuses to start)" \
    || bad "mode mismatch exits 2" "exit=$rc"
  grep -q 'Orders' <<<"$out" \
    && ok "mode mismatch names the offending class" \
    || bad "mode mismatch names the class" "got: $out"
  grep -q 'corruption' <<<"$out" \
    && bad "mismatch is not reported as corruption" "got: $out" \
    || ok "mode mismatch is not misreported as corruption"
else
  bad "mismatch fixtures compile" "compile failed"
fi

# ---- 4. the compile-time refusals still fire ------------------------------
printf '@table(name: "x", durable: false, resident: keys)\nclass X { k: Text }\nfn main() -> Int { return 0; }\n' > "$WORK/combo.wo"
# NOTE: capture, then grep. `woc | grep` under `set -o pipefail` returns
# woc's exit 1 (it reports diagnostics) even when grep matched, which made
# both of these checks fail while the compiler was behaving correctly.
combo_out="$("$WOC" "$WORK/combo.wo" 2>&1)"
grep -q 'WO-E102' <<<"$combo_out" \
  && ok "durable:false + resident:keys is WO-E102" \
  || bad "combination refused" "got: $combo_out"

printf '@table(name: "s", durable: false)\nclass S { t: Text }\n@table(name: "o")\nclass O { s: ref S }\nfn main() -> Int { return 0; }\n' > "$WORK/dref.wo"
dref_out="$("$WOC" "$WORK/dref.wo" 2>&1)"
grep -q 'WO-E224' <<<"$dref_out" \
  && ok "durable ref into a volatile table is WO-E224" \
  || bad "dangling ref refused" "got: $dref_out"

# ---- 5. the doc example actually runs, and Product really is resident: keys
# The example is the readable half of this gate. An example no gate runs
# rots. `Product` is declared `resident: keys` in main.wo — checks 2 and 3
# below are this leg's whole point: the program runs, and `place_order`'s
# stock decrement on a keys-resident row survives a restart, replayed out of
# the log rather than out of a slab. That is the property a load-time refusal
# used to stand in for; now the example proves it directly instead.
LOG=/tmp/residency.log
: > "$LOG"
echo "residency-accept: example output -> $LOG (tail -F it)"
EX="docs/examples/residency"
{
  echo "===================== residency example ====================="
} >> "$LOG"
if "$WOC" --emit "$EX/main.wo" -o "$WORK/residency.wob" >>"$LOG" 2>&1; then
  ok "the doc example compiles"
  mkdir -p "$WORK/exdata"
  {
    echo "--------------------- run 1: seed ---------------------"
    WO_DATA="$WORK/exdata" "$WOVM" "$WORK/residency.wob" seed 2>&1
    echo "--------------------- run 2: restart + order ----------"
  } >> "$LOG"
  ex2="$(WO_DATA="$WORK/exdata" "$WOVM" "$WORK/residency.wob" order 2>&1)"
  printf '%s\n' "$ex2" >> "$LOG"
  grep -q 'products=2 carts=0' <<<"$ex2" \
    && ok "example: durable replayed, volatile did not" \
    || bad "example restart" "got: $ex2"
  # the stronger claim: a FIELD CHANGE survives, not just an insert
  ex3="$(WO_DATA="$WORK/exdata" "$WOVM" "$WORK/residency.wob" order 2>&1)"
  printf '%s\n' "$ex3" >> "$LOG"
  grep -q "an earlier order's decrement replayed" <<<"$ex3" \
    && ok "example: a stock update replays across a restart" \
    || bad "example update replay" "got: $ex3"
else
  bad "the doc example compiles" "see $LOG"
fi

echo
echo "residency-accept: $((pass + fail)) checks, $fail failures"
[[ $fail -eq 0 ]] || exit 1

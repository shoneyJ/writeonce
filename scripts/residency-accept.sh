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
    WO_DATA="$WORK/exdata" "$WOVM" "$WORK/residency.wob" seed 2>&1; rc=$?
    echo "--------------------- run 2: restart + order ----------"
  } >> "$LOG"
  # seed's rows commit before anything can crash it, so the restart legs
  # below pass on the log a dead seed leaves behind — its rc is its own check
  [[ $rc -eq 0 ]] \
    && ok "example: seed exits 0" \
    || bad "example seed" "rc=$rc (139 = SIGSEGV; see $LOG)"
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

# ---- 6. resident:keys refuses to run without WO_DATA -----------------------
# CRITICAL 1: the loader stopped refusing durable:true + resident:keys once
# UPDATE landed, and nothing replaced that refusal at runtime — a table
# declared this way ran with no WAL to fold its rows from, misreporting
# every read as "no such row" instead of naming the real problem.
printf '@table(name: "items", index: [k], durable: true, resident: keys)\nclass Items { k: Text }\nfn main() -> Int { insert Items { k: "a" }; return 0; }\n' > "$WORK/reskeys.wo"
if "$WOC" --emit "$WORK/reskeys.wo" -o "$WORK/reskeys.wob" 2>"$WORK/e"; then
  out="$("$WOVM" "$WORK/reskeys.wob" 2>&1)"; rc=$?
  [[ $rc -eq 2 ]] \
    && ok "resident:keys without WO_DATA exits 2" \
    || bad "resident:keys without WO_DATA exits 2" "exit=$rc"
  grep -q 'Items' <<<"$out" \
    && ok "resident:keys refusal names the offending class" \
    || bad "resident:keys refusal names the class" "got: $out"
  mkdir -p "$WORK/d6"
  WO_DATA="$WORK/d6" "$WOVM" "$WORK/reskeys.wob" >/dev/null 2>&1
  rc2=$?
  [[ $rc2 -eq 0 ]] \
    && ok "resident:keys WITH WO_DATA still runs" \
    || bad "resident:keys with WO_DATA runs" "exit=$rc2"
else
  bad "resident:keys refusal fixture compiles" "$(head -1 "$WORK/e")"
fi

# ---- 7. a durable table (the default) refuses to run without WO_DATA -------
# databasev2 2 task 6a: before this, a durable class with no WO_DATA ran
# RAM-only and every write was silently discarded — the exact outcome
# `durable: true` promises against. WO_EPHEMERAL=1 is the explicit opt-in to
# that RAM-only run; anything else refuses at startup, exit 2, naming the
# class and all three ways forward.
printf '@table(name: "notes", index: [k])\nclass Notes { k: Text }\nfn main() -> Int { insert Notes { k: "a" }; let n = 0; for x in from r in Notes select r { n = n + 1; } print("notes=${n}"); return 0; }\n' > "$WORK/dur.wo"
if "$WOC" --emit "$WORK/dur.wo" -o "$WORK/dur.wob" 2>"$WORK/e"; then
  # (i) default-durable, no WO_DATA, no WO_EPHEMERAL -> refuse
  out="$(env -u WO_DATA -u WO_EPHEMERAL "$WOVM" "$WORK/dur.wob" 2>&1)"; rc=$?
  [[ $rc -eq 2 ]] && grep -q 'Notes' <<<"$out" && grep -q 'WO_DATA=' <<<"$out" \
    && grep -q 'WO_EPHEMERAL=1' <<<"$out" && grep -q 'durable: false' <<<"$out" \
    && ok "durable table without WO_DATA exits 2, names the class and all three ways forward" \
    || bad "durable table without WO_DATA refuses" "exit=$rc got: $out"
  # (ii) WO_EPHEMERAL=1 -> runs from RAM: boot notice on stderr, a write round-trips
  out="$(env -u WO_DATA WO_EPHEMERAL=1 "$WOVM" "$WORK/dur.wob" 2>&1)"; rc=$?
  [[ $rc -eq 0 ]] && grep -q 'WO_EPHEMERAL=1' <<<"$out" && grep -q 'notes=1' <<<"$out" \
    && ok "WO_EPHEMERAL=1 runs the durable table from RAM (boot notice, write round-trips)" \
    || bad "WO_EPHEMERAL=1 runs from RAM" "exit=$rc got: $out"
  # (iii) WO_EPHEMERAL together with WO_DATA -> conflict, refuse
  mkdir -p "$WORK/d7"
  out="$(WO_DATA="$WORK/d7" WO_EPHEMERAL=1 "$WOVM" "$WORK/dur.wob" 2>&1)"; rc=$?
  [[ $rc -eq 2 ]] && grep -q 'incompatible with WO_DATA' <<<"$out" \
    && ok "WO_EPHEMERAL=1 with WO_DATA set exits 2 and names the conflict" \
    || bad "WO_EPHEMERAL + WO_DATA conflict" "exit=$rc got: $out"
  # (v) the only accepted value is 1
  out="$(env -u WO_DATA WO_EPHEMERAL=2 "$WOVM" "$WORK/dur.wob" 2>&1)"; rc=$?
  [[ $rc -eq 2 ]] && grep -q 'WO_EPHEMERAL=1' <<<"$out" \
    && ok "WO_EPHEMERAL=2 exits 2 and names the accepted value" \
    || bad "WO_EPHEMERAL=2 refused" "exit=$rc got: $out"
else
  bad "durable refusal fixture compiles" "$(head -1 "$WORK/e")"
fi
# (iv) resident:keys still refuses under WO_EPHEMERAL=1 — the keys loop wins
if [[ -f "$WORK/reskeys.wob" ]]; then
  out="$(env -u WO_DATA WO_EPHEMERAL=1 "$WOVM" "$WORK/reskeys.wob" 2>&1)"; rc=$?
  [[ $rc -eq 2 ]] && grep -q 'resident: keys' <<<"$out" \
    && ok "WO_EPHEMERAL=1 does not rescue resident:keys (exit 2, keys message)" \
    || bad "WO_EPHEMERAL=1 vs resident:keys" "exit=$rc got: $out"
fi
# (vi) a class that is NOT a @table is not a durable table: no WO_DATA, no
# WO_EPHEMERAL, rc 0 and nothing on stderr. `durable: true` is a @table
# property; the .wob carries the table bit (v8) so the refusal loops can tell.
if "$WOC" --emit tests/corpus/run/methods/fixture.wo -o "$WORK/plain.wob" 2>"$WORK/e"; then
  out="$(env -u WO_DATA -u WO_EPHEMERAL "$WOVM" "$WORK/plain.wob" 2>"$WORK/plain.err")"; rc=$?
  [[ $rc -eq 0 && "$out" == $'15\n42' ]] && ! grep -q '^wovm:' "$WORK/plain.err" \
    && ok "a plain class (no @table) runs without WO_DATA: rc 0, no wovm: line" \
    || bad "plain class without WO_DATA" "exit=$rc out: $out err: $(cat "$WORK/plain.err")"
else
  bad "plain-class fixture compiles" "$(head -1 "$WORK/e")"
fi

# ---- 8. WO_DATA=<file>: the store as one file ------------------------------
# databasev2 7: WO_DATA names either a directory (-> <dir>/shard-0.wal, as it
# always did) or THE log file. Underneath it is the same wo_wal_* path, so
# these legs are a regression guard on main.c's resolution and its two new
# refusals, plus proof that compaction's temp (<file>.compact) never leaves a
# second artifact beside the file. Refusal legs run under `timeout`: a wrong
# resolution that opened the fifo would block, not fail.
F8="$WORK/f8"
mkdir -p "$F8/a" "$F8/d" "$F8/c" "$F8/k"
if [[ -f "$WORK/mix.wob" ]]; then
  # (i) seed -> restart prints exactly what the directory form printed in 1
  WO_DATA="$F8/a/app.db" "$WOVM" "$WORK/mix.wob" seed >"$F8/seed.out" 2>&1; rc=$?
  got="$(WO_DATA="$F8/a/app.db" "$WOVM" "$WORK/mix.wob" 2>&1)"
  [[ $rc -eq 0 && "$got" == "kept=1 scratch=0" ]] \
    && ok "file form: seed rc 0, restart replays the durable row, volatile gone" \
    || bad "file form restart" "seed rc=$rc got: $got $(head -1 "$F8/seed.out")"
  arts="$(find "$F8/a" -mindepth 1 -printf '%P\n' | sort | tr '\n' ' ')"
  [[ "$arts" == "app.db " ]] \
    && ok "file form: app.db is the only artifact (no shard-0.wal, no .compact)" \
    || bad "file form single artifact" "found: '$arts'"
  # (ii) missing parent: exit 2, name the path AND the parent, create nothing
  out="$(WO_DATA="$F8/nope/app.db" timeout 10 "$WOVM" "$WORK/mix.wob" seed 2>&1)"; rc=$?
  [[ $rc -eq 2 ]] && grep -Eq 'its parent .*/nope is not an existing directory' <<<"$out" \
    && grep -q 'nope/app.db' <<<"$out" && [[ ! -e "$F8/nope" ]] \
    && ok "file form: missing parent exits 2, names path + parent, runs no mkdir -p" \
    || bad "file form missing parent" "exit=$rc nope-exists=$([[ -e "$F8/nope" ]] && echo yes || echo no) got: $out"
  # (iii) exists but is neither a regular file nor a directory
  mkfifo "$F8/pipe.db"
  out="$(WO_DATA="$F8/pipe.db" timeout 10 "$WOVM" "$WORK/mix.wob" seed 2>&1)"; rc=$?
  [[ $rc -eq 2 ]] && grep -q 'neither a regular file nor a directory' <<<"$out" \
    && ok "file form: a fifo at WO_DATA exits 2 and says why" \
    || bad "file form fifo" "exit=$rc got: $out"
  # (iv) a trailing slash keeps the directory form, present or missing
  WO_DATA="$F8/d/" "$WOVM" "$WORK/mix.wob" seed >/dev/null 2>&1; rc=$?
  [[ $rc -eq 0 && -f "$F8/d/shard-0.wal" ]] \
    && ok "trailing slash: WO_DATA=<dir>/ still writes <dir>/shard-0.wal" \
    || bad "trailing slash dir form" "exit=$rc in d: $(ls "$F8/d" 2>&1 | tr '\n' ' ')"
  out="$(WO_DATA="$F8/nodir/" timeout 10 "$WOVM" "$WORK/mix.wob" seed 2>&1)"; rc=$?
  [[ $rc -eq 2 ]] && grep -Eq 'cannot open .*/nodir//shard-0.wal' <<<"$out" \
    && ok "trailing slash on a missing dir: pre-7 'cannot open .../nodir//shard-0.wal' kept byte for byte" \
    || bad "trailing slash missing dir" "exit=$rc got: $out"
  # (v) WO_EPHEMERAL=1 conflicts with the file form exactly as with a directory
  out="$(WO_EPHEMERAL=1 WO_DATA="$F8/a/app.db" timeout 10 "$WOVM" "$WORK/mix.wob" 2>&1)"; rc=$?
  [[ $rc -eq 2 ]] && grep -q 'incompatible with WO_DATA' <<<"$out" \
    && ok "file form: WO_EPHEMERAL=1 with WO_DATA=<file> exits 2, names the conflict" \
    || bad "file form ephemeral conflict" "exit=$rc got: $out"
else
  bad "file form legs" "mix.wob missing (section 1 did not compile)"
fi
# `wal N` prints `acked i` AFTER each insert returns (the return is the ack);
# `verify M` wants rows 1..M present exactly once, rows past M allowed.
cat > "$WORK/onefile.wo" <<'EOF'
@table(name: "rows", index: [k])
class Row { k: Int }
fn main(args: multi Text) -> Int {
  if len(args) < 2 { return 2; }
  let n = parse_int(args[1]);
  if n == nil or n < 1 { return 2; }
  if args[0] == "wal" {
    let i = 1;
    while i <= n { insert Row { k: i }; print("acked ${i}"); i = i + 1; }
    return 0;
  }
  let i = 1;
  while i <= n {
    let hits = 0;
    for r in from r in Row where r.k == i select r { hits = hits + 1; }
    if hits != 1 { print("row ${i}: ${hits} hits"); return 3; }
    i = i + 1;
  }
  print("verified ${n}");
  return 0;
}
EOF
if "$WOC" --emit "$WORK/onefile.wo" -o "$WORK/onefile.wob" 2>"$WORK/e"; then
  # (vi) kill -9 mid-write against the file: every acked row replays. stdout
  # is line-buffered (stdbuf) so the ack count is exact, not a flush boundary;
  # N is far more than 0.5 s of inserts so the kill lands mid-run — a run that
  # exited on its own (rc != 137) proves nothing and is reported as such.
  WO_DATA="$F8/k/app.db" stdbuf -oL "$WOVM" "$WORK/onefile.wob" wal 1000000 >"$F8/k.out" 2>/dev/null &
  kp=$!
  sleep "0.$((RANDOM % 30 + 20))"
  kill -KILL "$kp" 2>/dev/null; wait "$kp" 2>/dev/null; krc=$?
  acked="$(grep -E '^acked [0-9]+$' "$F8/k.out" | tail -1 | cut -d' ' -f2)"
  if [[ $krc -eq 137 && -n "$acked" && "$acked" -gt 0 ]]; then
    out="$(WO_DATA="$F8/k/app.db" "$WOVM" "$WORK/onefile.wob" verify "$acked" 2>&1)"; rc=$?
    [[ $rc -eq 0 ]] \
      && ok "file form: kill -9 after $acked acks, all $acked rows replay from app.db" \
      || bad "file form kill -9 replay" "acked=$acked verify rc=$rc got: $(tail -1 <<<"$out")"
  else
    bad "file form kill -9" "run rc=$krc acked=${acked:-0} -- the kill did not land mid-write, the leg proves nothing"
  fi
  # (vii) a forced compaction in the file form: the temp is <file>.compact
  #       beside the log; after the rename app.db must still be the only
  #       artifact and every row must replay. WO_WAL_STATS proves the
  #       compaction RAN — a trigger that never fired would prove nothing.
  out="$(WO_DATA="$F8/c/app.db" WO_CHECKPOINT_BYTES=1 WO_WAL_STATS=1 "$WOVM" "$WORK/onefile.wob" wal 300 2>&1)"; rc=$?
  comps="$(grep -o 'compactions=[0-9]*' <<<"$out" | cut -d= -f2)"
  [[ $rc -eq 0 && "${comps:-0}" -ge 1 ]] \
    && ok "file form: WO_CHECKPOINT_BYTES=1 forced $comps compaction(s) over 300 inserts" \
    || bad "file form compaction binds" "exit=$rc compactions=${comps:-none} got: $(tail -1 <<<"$out")"
  arts="$(find "$F8/c" -mindepth 1 -printf '%P\n' | sort | tr '\n' ' ')"
  [[ "$arts" == "app.db " ]] \
    && ok "file form: app.db is still the only artifact after compaction (no .compact left)" \
    || bad "file form compaction artifacts" "found: '$arts'"
  out="$(WO_DATA="$F8/c/app.db" "$WOVM" "$WORK/onefile.wob" verify 300 2>&1)"; rc=$?
  [[ $rc -eq 0 ]] \
    && ok "file form: all 300 rows replay from the compacted app.db" \
    || bad "file form compacted replay" "exit=$rc got: $(tail -1 <<<"$out")"
else
  bad "file form battery fixture compiles" "$(head -1 "$WORK/e")"
fi

echo
echo "residency-accept: $((pass + fail)) checks, $fail failures"
[[ $fail -eq 0 ]] || exit 1

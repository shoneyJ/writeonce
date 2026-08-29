#!/usr/bin/env python3
"""db-bench campaign driver (iteration 22).

Runs the docs/examples/db-bench sample across the campaign matrix
(ram/durable x 1/default shards), collects the sample's metric lines
into one results JSON, runs the durability legs (restart proof + kill -9
battery), samples RSS/fd during the mix phase (LW_SOAK discipline), and
evaluates every metric against bench/baseline.json.

    scripts/db-bench.py [--quick] [--write-baseline]

Exit 0 = campaign green; 1 = gate breach or a durability leg failed.
Plan deviation, disclosed: one python driver instead of bash+python —
the live stdout sampling and JSON assembly are the whole job.
"""
import json, os, re, subprocess, sys, time, random, shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "docs/examples/db-bench/target/db-bench")
WOC = os.path.join(ROOT, "compiler/_build/default/bin/woc")
WOVM = os.path.join(ROOT, "runtime/wovm")
BASELINE = os.path.join(ROOT, "bench/baseline.json")
RESULTS_DIR = os.path.join(ROOT, "bench/results")

QUICK = "--quick" in sys.argv
WRITE_BASELINE = "--write-baseline" in sys.argv

N = 2000 if QUICK else 20000
# databasev2 4: the write-concurrent leg. `mix` writes on one op in ten with
# C=4, so group commit had almost nothing to batch there (measured mean batch
# 1.01, peak 3) — a property of that workload, not of the mechanism. C is high
# on purpose: batching is a function of how many writes are in flight, and
# measured mean batch rose 1.13 -> 1.76 -> 5.35 at C = 4 -> 16 -> 64.
WMIX_N = 4000 if QUICK else 20000
WMIX_C = 32 if QUICK else 64
# databasev2 3: the checkpoint leg. Ages a store by UPDATING the same rows, so
# history grows while the live set does not — otherwise the leg measures insert
# throughput instead of compaction.
CKPT_SEED = 2000 if QUICK else 5000
CKPT_OPS = 8000 if QUICK else 20000
# The stop-the-world budget. 50ms is a stall a serving process can absorb
# without a client noticing a timeout; measured at ~13ms for a 2MB live set,
# so this leaves real headroom while still failing before a stall becomes
# user-visible. Compaction is O(live rows), so this budget is what eventually
# forces the incremental design the spec deliberately did not buy in advance.
CKPT_PAUSE_BUDGET_US = 50000
MSG_N = 20000 if QUICK else 200000
WAL_N = 800 if QUICK else 4000
CRASH_REPS = 1 if QUICK else 3
LINE = re.compile(r"^(\w+) (\d+) (\d+) (\d+) (\d+)$")
MSGLINE = re.compile(r"^msgrate (\d+) (\d+)$")

passed, failed = [], []
def ok(name): passed.append(name); print(f"ok   {name}")
def bad(name, why): failed.append(name); print(f"FAIL {name} -- {why}")

def build():
    os.makedirs(os.path.dirname(BIN), exist_ok=True)
    r = subprocess.run([WOC, "build", os.path.join(ROOT, "docs/examples/db-bench"),
                        "-o", BIN, "--runtime", WOVM], capture_output=True, text=True)
    if r.returncode != 0:
        bad("build", r.stderr.strip()[:200]); sys.exit(1)
    ok("builds")

def run(args, env_extra, timeout, sample_after=None):
    """Run the sample; return (rc, lines, rss_growth_kb, fd_growth).
    sample_after: stdout prefix that starts the RSS/fd baseline (the mix
    phase begins after the 'write' report line lands)."""
    env = dict(os.environ); env.update(env_extra)
    p = subprocess.Popen([BIN] + args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, env=env)
    lines, base_rss, base_fd, peak_rss, peak_fd = [], None, None, None, None
    def rss_fd():
        try:
            with open(f"/proc/{p.pid}/status") as f:
                rss = next((int(l.split()[1]) for l in f if l.startswith("VmRSS:")), None)
            fd = len(os.listdir(f"/proc/{p.pid}/fd"))
            return rss, fd
        except OSError:
            return None, None
    deadline = time.time() + timeout
    import threading
    def reader():
        assert p.stdout is not None
        for line in p.stdout:
            lines.append(line.rstrip("\n"))
    t = threading.Thread(target=reader); t.start()
    while p.poll() is None and time.time() < deadline:
        if sample_after is not None and any(l.startswith(sample_after) for l in lines):
            r, f = rss_fd()
            if r is not None:
                if base_rss is None: base_rss, base_fd = r, f
                peak_rss = r if peak_rss is None else max(peak_rss, r)
                if f is not None:
                    peak_fd = f if peak_fd is None else max(peak_fd, f)
        time.sleep(0.25)
    if p.poll() is None:
        p.kill(); t.join(); return 124, lines, None, None
    t.join()
    growth = (peak_rss - base_rss) if base_rss is not None and peak_rss is not None else None
    fdg = (peak_fd - base_fd) if base_fd is not None and peak_fd is not None else None
    return p.returncode, lines, growth, fdg

def parse_metrics(lines, into, prefix):
    for l in lines:
        m = LINE.match(l)
        if m:
            op, _cnt, rate, p50, p99 = m.group(1), *(int(x) for x in m.groups()[1:])
            into[f"{prefix}.{op}.ops_sec"] = rate
            into[f"{prefix}.{op}.p50us"] = p50
            into[f"{prefix}.{op}.p99us"] = p99
        m = MSGLINE.match(l)
        if m:
            into[f"{prefix}.msgrate.msgs_sec"] = int(m.group(2))

def wmix_leg(metrics, tag, env, data):
    """Every op a durable write, WMIX_C at once — the leg that actually
    exercises group commit.

    It reuses the store the `all` run just seeded (a fresh process replays it,
    so `kmod` is there) and asks the runtime for its group-commit counters via
    WO_WAL_STATS. The counters matter as much as the throughput: if batches are
    always one the mechanism is inert and any throughput change came from
    somewhere else, so a payoff would be attributed to the wrong cause."""
    e = dict(env); e["WO_WAL_STATS"] = "1"
    rc, lines, _, _ = run(["wmix", str(WMIX_N), str(WMIX_C)], e, 1800)
    if rc != 0:
        bad(f"{tag}.wmix", f"rc={rc} tail={lines[-2:]}")
        return
    ops = p50 = p99 = None
    batches = records = peak_batch = peak_staged = None
    for l in lines:
        f = l.split()
        if f and f[0] == "wmix" and len(f) == 5:
            ops, p50, p99 = int(f[2]), int(f[3]), int(f[4])
        elif f and f[0] == "walstats":
            kv = dict(x.split("=", 1) for x in f[1:] if "=" in x)
            batches = int(kv.get("batches", 0)); records = int(kv.get("records", 0))
            peak_batch = int(kv.get("peak_batch", 0)); peak_staged = int(kv.get("peak_staged", 0))
    if ops is None or batches is None:
        bad(f"{tag}.wmix", "no report or no walstats line")
        return
    metrics[f"{tag}.wmix.ops_sec"] = ops
    metrics[f"{tag}.wmix.p50us"] = p50
    metrics[f"{tag}.wmix.p99us"] = p99
    metrics[f"{tag}.wmix.peak_batch"] = peak_batch
    metrics[f"{tag}.wmix.peak_staged"] = peak_staged
    mean = round(records / batches, 2) if batches else 0
    metrics[f"{tag}.wmix.mean_batch"] = mean
    ok(f"{tag}.wmix: {ops} ops/sec, p50 {p50}us p99 {p99}us; "
       f"{records} records over {batches} barriers (mean {mean}, peak {peak_batch}), "
       f"peak staged {peak_staged}B")
    # The gate that matters. Only the MULTI-shard leg can batch: a worker's
    # statements marshal to shard 0 and queue, while shard-0 statements run
    # inline and commit one at a time by design (see db.c).
    if tag.endswith(".sN"):
        if mean > 1.0:
            ok(f"{tag}.wmix batches form (mean {mean} > 1)")
        else:
            bad(f"{tag}.wmix-inert",
                f"mean batch {mean} — group commit is not engaging, so a "
                f"throughput change would not be attributable to it")


def campaign():
    metrics = {}
    ncores = os.cpu_count() or 1
    for flavor in ("ram", "durable"):
        for shards in (1, ncores):
            tag = f"{flavor}.s{'1' if shards == 1 else 'N'}"
            env = {"WO_SHARDS": str(shards)}
            data = None
            if flavor == "durable":
                data = os.path.join(ROOT, "bench", f"tmp.{os.getpid()}.{tag}")
                os.makedirs(data, exist_ok=True)
                env["WO_DATA"] = data
            rc, lines, rssg, fdg = run(["all", str(N)], env, 1800, sample_after="write ")
            if rc != 0:
                bad(f"{tag}.all", f"rc={rc} tail={lines[-2:]}")
            else:
                parse_metrics(lines, metrics, tag)
                ok(f"{tag}.all")
                if rssg is not None:
                    metrics[f"{tag}.mix.rss_growth_kb"] = rssg
                    metrics[f"{tag}.mix.fd_growth"] = fdg
                    # LW_SOAK discipline: mix inserts ~N/100 rows (slab
                    # growth is legitimate); the budget catches MB-class
                    # leaks, fd growth is zero-tolerance
                    if fdg and fdg > 0:
                        bad(f"{tag}.mix.fds", f"grew {fdg}")
                    else:
                        ok(f"{tag}.mix.fds flat")
            if flavor == "durable" and data:
                wmix_leg(metrics, tag, env, data)
            if data: shutil.rmtree(data, ignore_errors=True)
        # msgrate once per shard count, RAM only (no store dependency)
    for shards in (1, ncores):
        tag = f"msg.s{'1' if shards == 1 else 'N'}"
        # iteration 24 gave mailboxes a cap (default 1024, fail-fast trap);
        # msgrate's contract is an UNBOUNDED one-way flood, so the driver
        # raises the cap to the flood size — the measured number keeps
        # iteration 22's semantics exactly.
        rc, lines, _, _ = run(["msgrate", str(MSG_N)],
                              {"WO_SHARDS": str(shards), "WO_MAILBOX": str(MSG_N)}, 300)
        if rc != 0:
            bad(tag, f"rc={rc}")
        else:
            parse_metrics(lines, metrics, tag.replace("msg.", "ram."))
            ok(tag)
    return metrics

def durability(metrics):
    ncores = os.cpu_count() or 1
    for shards in (1, ncores):
        s = "s1" if shards == 1 else "sN"
        env = {"WO_SHARDS": str(shards)}
        # restart proof
        data = os.path.join(ROOT, "bench", f"tmp.{os.getpid()}.restart.{s}")
        os.makedirs(data, exist_ok=True)
        env["WO_DATA"] = data
        rc1, _, _, _ = run(["seed", "3000"], env, 300)
        rc2, lines, _, _ = run(["verify"], env, 300)
        if rc1 == 0 and rc2 == 0:
            ok(f"restart.{s}: seeded store replays byte-true")
        else:
            bad(f"restart.{s}", f"seed rc={rc1} verify rc={rc2} {lines[-1:]}")
        shutil.rmtree(data, ignore_errors=True)
        # crash battery: kill -9 mid-wal, verify every acked row
        for rep in range(CRASH_REPS):
            data = os.path.join(ROOT, "bench", f"tmp.{os.getpid()}.crash.{s}.{rep}")
            os.makedirs(data, exist_ok=True)
            env["WO_DATA"] = data
            e = dict(os.environ); e.update(env)
            p = subprocess.Popen([BIN, "wal", str(WAL_N)], stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL, text=True, env=e)
            time.sleep(random.uniform(0.05, 0.5))
            p.kill()  # SIGKILL: no unwind, no flush — the honest crash
            out, _ = p.communicate()
            acked = 0
            for l in out.splitlines():
                if l.startswith("acked "):
                    acked = int(l.split()[1])
            rc, lines, _, _ = run(["verify-acked", str(max(acked, 1))], env, 300)
            if acked > 0 and rc == 0:
                ok(f"crash.{s}.{rep}: {acked} acked rows all present after kill -9")
            elif acked == 0:
                ok(f"crash.{s}.{rep}: killed before first ack (nothing owed)")
            else:
                bad(f"crash.{s}.{rep}", f"acked={acked} verify rc={rc} {lines[-1:]}")
            shutil.rmtree(data, ignore_errors=True)

def gate(metrics):
    if not os.path.exists(BASELINE):
        if WRITE_BASELINE:
            write_baseline(metrics); return
        bad("gate", "no bench/baseline.json (run --write-baseline once)"); return
    base = json.load(open(BASELINE))
    for key, spec in sorted(base.items()):
        if key.startswith("_"): continue
        got = metrics.get(key)
        if got is None:
            bad(f"gate.{key}", "metric missing from this run"); continue
        val, tol, floor = spec["value"], spec.get("tolerance_pct", 15), spec.get("floor")
        higher_is_better = spec.get("dir", "higher") == "higher"
        if QUICK:
            # quick mode: floors only — counts are too small for stable
            # deltas. mix* skipped entirely: at N=2000 the completion
            # POLL (20ms sleeps) dominates wall time, so its ops/sec is
            # an artifact of the poll quantum, not the store.
            if ".mixread." in key or ".mixwrite." in key:
                ok(f"gate.{key} (skipped: quick-mode mix is poll-bound)")
                continue
            breach = floor is not None and ((got < floor) if higher_is_better else (got > floor))
            (ok if not breach else lambda n: bad(n, f"{got} vs floor {floor}"))(f"gate.{key} (floor)")
            continue
        if higher_is_better:
            rel_bad = got < val * (100 - tol) / 100
            floor_bad = floor is not None and got < floor
        else:
            # sub-20µs latencies are histogram quantization: 2µs vs 1µs
            # reads as "+100%" while meaning one bucket — floor-only there
            rel_bad = val >= 20 and got > val * (100 + tol) / 100
            floor_bad = floor is not None and got > floor
        if rel_bad or floor_bad:
            bad(f"gate.{key}", f"{got} vs baseline {val} (tol {tol}%, floor {floor})")
        else:
            ok(f"gate.{key} {got} (baseline {val})")
    if WRITE_BASELINE:
        write_baseline(metrics)

def tolerance_for(key):
    """The tuning POLICY lives here so --write-baseline refreshes keep it
    (the first refresh silently reset hand-edits to 15% — never again).
    mix*: scheduling-dependent small counts. read/query + all .sN.*:
    machine jitter, and at post-index-µs scale a 1µs histogram step on a
    7µs p50 is already 14%."""
    # databasev2 4: batch SHAPE follows arrival timing, so gating it tightly
    # would gate the scheduler — what must hold is that the mean exceeds one
    # under contention, which wmix_leg asserts directly against the live run.
    # wmix's throughput and latency are NOT waived: they are the payoff, and a
    # blanket waiver here would have left the whole leg ungated.
    if key.endswith((".wmix.mean_batch", ".wmix.peak_batch", ".wmix.peak_staged")):
        return 100
    # databasev2 4: DURABLE multi-shard p99 is an fsync TAIL, and group commit
    # made it both noisier and legitimately higher. Measured across three full
    # runs of the same build, durable.sN.mixread.p99 was 1043 / 2318 / 4147 us
    # and wmix.p99 8758 / 20000 — a 2-4x spread with the box near idle, because
    # a barrier now blocks the owner shard LONGER (more records per fsync) even
    # though it blocks LESS OFTEN. That is the trade group commit makes on a
    # single-threaded owner, and part B (async submission) is what would undo
    # it. Gating a 2-4x-variable tail at 50% gates the disk, not the engine, so
    # the FLOOR is the real guard here — and it is not slack: mixread's floor
    # (4172us) came within 25us of tripping on the worst run.
    if key.startswith("durable.sN.") and key.endswith(".p99us"):
        return 100
    # databasev2 3: the RECLAIM ratio is structural and gated tightly — it is
    # the feature's whole claim. Boot time and the pause are wall-clock on a
    # shared box and are not: waiving them all would have left the leg ungated,
    # which is the mistake part A's task 4 made and had to undo.
    if key in ("ckpt.boot_off_ms", "ckpt.boot_on_ms", "ckpt.pause_us_max",
               "ckpt.compactions", "ckpt.bytes_off", "ckpt.bytes_on"):
        return 100
    if ".mixread." in key or ".mixwrite." in key: return 50
    if ".sN." in key: return 50
    if ".read." in key or ".query." in key: return 50
    return 15

def write_baseline(metrics):
    base = {"_config": {"N": N, "msg_n": MSG_N, "wal_n": WAL_N, "crash_reps": CRASH_REPS,
                        "note": "refresh only with a commit that says why; "
                                "tolerances come from tolerance_for() in the driver"}}
    for k, v in sorted(metrics.items()):
        if k.endswith(("rss_growth_kb", "fd_growth")): continue
        # reclaim_x: MORE reclaimed is better. Recorded as lower-is-better by
        # the default detector, which would have passed "no reclaim at all" and
        # failed an improvement — the feature's central claim, gated backwards.
        higher = k.endswith(("ops_sec", "msgs_sec", "mean_batch", "peak_batch",
                             "reclaim_x"))
        floor_div = 8 if k.endswith("msgs_sec") else 4
        # latency floors never sit below 100µs: at post-index µs scale a
        # 4×1µs "catastrophe line" is noise; the tripwire means "µs became
        # ms" (an O(table) relapse lands at 600µs+ and is still caught)
        base[k] = {"value": v, "tolerance_pct": tolerance_for(k),
                   "floor": (v // floor_div if higher else max(v * 4, 100)),
                   "dir": "higher" if higher else "lower"}
    os.makedirs(os.path.dirname(BASELINE), exist_ok=True)
    json.dump(base, open(BASELINE, "w"), indent=1, sort_keys=True)
    ok(f"baseline written ({len(base) - 1} metrics)")

def wal_used_bytes(data):
    """Bytes actually written, as the non-zero prefix — never the file size:
    shard WALs are preallocated, so getsize reports the preallocation."""
    total = 0
    for name in sorted(os.listdir(data)):
        with open(os.path.join(data, name), "rb") as f:
            total += len(f.read().rstrip(b"\x00"))
    return total


def checkpoint_leg(metrics):
    """Space reclaimed, boot time, and the stop-the-world PAUSE.

    The same workload runs twice, differing only in whether checkpointing can
    fire: an enormous floor disables it, a small one lets it. Comparing two runs
    of one build is what isolates compaction from everything else the workload
    does.

    Boot is measured with the sample's `boot` mode, which does nothing at all —
    with WO_DATA set the runtime replays the whole log before main runs, so a
    mode with no work of its own is the only honest way to price replay."""
    ncores = os.cpu_count() or 1
    out = {}
    for name, knobs in (("off", {"WO_CHECKPOINT_BYTES": "1000000000"}),
                        ("on", {"WO_CHECKPOINT_BYTES": "65536", "WO_CHECKPOINT_RATIO": "2"})):
        data = os.path.join(ROOT, "bench", f"tmp.{os.getpid()}.ckpt.{name}")
        shutil.rmtree(data, ignore_errors=True); os.makedirs(data, exist_ok=True)
        env = {"WO_DATA": data, "WO_SHARDS": str(ncores), "WO_WAL_STATS": "1"}
        env.update(knobs)
        rc, _, _, _ = run(["seed", str(CKPT_SEED)], env, 1800)
        if rc != 0:
            bad(f"ckpt.{name}.seed", f"rc={rc}"); shutil.rmtree(data, ignore_errors=True); return
        rc, lines, _, _ = run(["wmix", str(CKPT_OPS), "16"], env, 1800)
        if rc != 0:
            bad(f"ckpt.{name}.age", f"rc={rc}"); shutil.rmtree(data, ignore_errors=True); return
        stats = {}
        for l in lines:
            f = l.split()
            if f and f[0] == "walstats":
                stats = dict(x.split("=", 1) for x in f[1:] if "=" in x)
        used = wal_used_bytes(data)
        # NOT through run(): it samples RSS on a 250ms poll, so every timing it
        # produces floors at the poll quantum — boot measured that way reported
        # 251ms both with and without checkpointing, which is the harness's
        # clock, not the engine's. Median of 3 because this is wall-clock.
        benv = dict(os.environ)
        benv.update({"WO_DATA": data, "WO_SHARDS": str(ncores)})
        samples = []
        brc = 0
        for _ in range(3):
            t0 = time.monotonic()
            pr = subprocess.run([BIN, "boot"], stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, env=benv, timeout=900)
            samples.append((time.monotonic() - t0) * 1000.0)
            brc = pr.returncode or brc
        boot_ms = sorted(samples)[1]
        if brc != 0:
            bad(f"ckpt.{name}.boot", f"rc={brc}"); shutil.rmtree(data, ignore_errors=True); return
        out[name] = (used, boot_ms, stats)
        shutil.rmtree(data, ignore_errors=True)

    (off_b, off_boot, _), (on_b, on_boot, st) = out["off"], out["on"]
    comps = int(st.get("compactions", 0))
    if comps == 0:
        bad("ckpt.inert", "no compaction ran — the leg proves nothing about checkpointing")
        return
    metrics["ckpt.compactions"] = comps
    metrics["ckpt.bytes_off"] = off_b
    metrics["ckpt.bytes_on"] = on_b
    metrics["ckpt.reclaim_x"] = round(off_b / max(on_b, 1), 2)
    metrics["ckpt.boot_off_ms"] = int(round(off_boot))
    metrics["ckpt.boot_on_ms"] = int(round(on_boot))
    metrics["ckpt.pause_us_max"] = int(st.get("compact_us_max", 0))
    ok(f"ckpt: {off_b} -> {on_b} bytes ({metrics['ckpt.reclaim_x']}x reclaimed) over "
       f"{comps} compactions; boot {off_boot:.0f} -> {on_boot:.0f} ms; "
       f"stop-the-world pause max {metrics['ckpt.pause_us_max']}us")
    # the space claim is the point of the feature, so it is asserted, not just recorded
    if off_b <= on_b:
        bad("ckpt.no-reclaim", f"checkpointing did not shrink the log ({off_b} -> {on_b})")
    else:
        ok(f"ckpt: the log is smaller with checkpointing on")
    # THE BUDGET. Stated, not assumed — the spec refused to assume it.
    if metrics["ckpt.pause_us_max"] > CKPT_PAUSE_BUDGET_US:
        bad("ckpt.pause-budget",
            f"stop-the-world pause {metrics['ckpt.pause_us_max']}us exceeds the stated "
            f"{CKPT_PAUSE_BUDGET_US}us budget — alternatives (incremental copy, "
            f"fork-and-dump) are bought against THIS number")
    else:
        ok(f"ckpt: pause within budget ({metrics['ckpt.pause_us_max']} <= {CKPT_PAUSE_BUDGET_US}us)")


def main():
    # --check <results.json>: gate-only evaluation of a recorded run — the
    # gate-bites smoke doctors a copy and this mode must FAIL on it
    if "--check" in sys.argv:
        f = sys.argv[sys.argv.index("--check") + 1]
        gate(json.load(open(f)))
        print()
        print(f"db-bench --check: {len(passed) + len(failed)} checks, {len(failed)} failures")
        sys.exit(1 if failed else 0)
    build()
    metrics = campaign()
    durability(metrics)
    checkpoint_leg(metrics)
    os.makedirs(RESULTS_DIR, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    out = os.path.join(RESULTS_DIR, f"run-{stamp}{'-quick' if QUICK else ''}.json")
    json.dump(metrics, open(out, "w"), indent=1, sort_keys=True)
    gate(metrics)
    print()
    print(f"db-bench: {len(passed) + len(failed)} checks, {len(failed)} failures "
          f"(results: {os.path.relpath(out, ROOT)})")
    sys.exit(1 if failed else 0)

main()

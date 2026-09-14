#!/usr/bin/env python3
"""db-bench campaign driver (iteration 22).

Runs the docs/examples/db-bench sample across the campaign matrix
(ram/durable x 1/default shards), collects the sample's metric lines
into one results JSON, runs the durability legs (restart proof + kill -9
battery), samples RSS/fd during the mix phase (LW_SOAK discipline), and
evaluates every metric against bench/baseline.json.

    scripts/db-bench.py [--quick] [--write-baseline] [--wo-data-file]

Exit 0 = campaign green; 1 = gate breach or a durability leg failed.
Plan deviation, disclosed: one python driver instead of bash+python —
the live stdout sampling and JSON assembly are the whole job.
"""
import json, os, re, subprocess, sys, tempfile, time, random, shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "docs/examples/db-bench/target/db-bench")
# databasev2 2 task 7. Its own program, not a mode in db-bench: declaring a
# `resident: keys` table is a WHOLE-PROGRAM constraint — the runtime refuses to
# start without WO_DATA, for every mode in the module, and WO_EPHEMERAL=1 does
# not rescue it. Putting those classes in db-bench's shared types made the ram,
# msgrate, growth and randread legs, which deliberately run WITHOUT WO_DATA
# (ceiling sets one — it measures what survives the kill), refuse to start.
# databasev2 2 task 6a: those RAM legs now opt in with WO_EPHEMERAL=1; a
# WO_DATA exported in the caller's shell no longer silently turns them durable,
# it refuses loudly (WO_EPHEMERAL is incompatible with WO_DATA).
RESID_BIN = os.path.join(ROOT, "docs/examples/residency-bench/target/residency-bench")
WOC = os.path.join(ROOT, "compiler/_build/default/bin/woc")
WOVM = os.path.join(ROOT, "runtime/wovm")
BASELINE = os.path.join(ROOT, "bench/baseline.json")
RESULTS_DIR = os.path.join(ROOT, "bench/results")

QUICK = "--quick" in sys.argv
WRITE_BASELINE = "--write-baseline" in sys.argv
# databasev2 7: WO_DATA may name THE log file instead of a directory. The flag
# points the durability legs (restart proof, kill -9 battery) at <tmp>/app.db.
# Same wo_wal_* path underneath — a guard on main.c's resolution under the
# honest crash, not a new measurement: no metric, baseline untouched.
FILE_FORM = "--wo-data-file" in sys.argv
FORM = ".file" if FILE_FORM else ""
def store(d):
    """a durability leg's WO_DATA: the directory, or with --wo-data-file the
    one file <d>/app.db (its parent must exist; wovm never runs mkdir -p)"""
    return os.path.join(d, "app.db") if FILE_FORM else d

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
    r = subprocess.run([WOC, "build", os.path.join(ROOT, "docs/examples/residency-bench"),
                        "-o", RESID_BIN, "--runtime", WOVM], capture_output=True, text=True)
    if r.returncode != 0:
        bad("build residency-bench", r.stderr.strip()[:200]); sys.exit(1)
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
            env = {"WO_SHARDS": str(shards), "WO_EPHEMERAL": "1"}
            data = None
            if flavor == "durable":
                data = os.path.join(ROOT, "bench", f"tmp.{os.getpid()}.{tag}")
                os.makedirs(data, exist_ok=True)
                env["WO_DATA"] = data
                del env["WO_EPHEMERAL"]
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
                              {"WO_SHARDS": str(shards), "WO_MAILBOX": str(MSG_N),
                               "WO_EPHEMERAL": "1"}, 300)
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
        env["WO_DATA"] = store(data)
        rc1, _, _, _ = run(["seed", "3000"], env, 300)
        rc2, lines, _, _ = run(["verify"], env, 300)
        # file form: app.db must be the ONLY artifact (no shard-0.wal, no
        # .compact temp); the directory form has nothing to assert here
        arts = sorted(os.listdir(data))
        if rc1 == 0 and rc2 == 0 and (not FILE_FORM or arts == ["app.db"]):
            ok(f"restart.{s}{FORM}: seeded store replays byte-true")
        else:
            bad(f"restart.{s}{FORM}", f"seed rc={rc1} verify rc={rc2} {lines[-1:]} artifacts={arts}")
        shutil.rmtree(data, ignore_errors=True)
        # crash battery: kill -9 mid-wal, verify every acked row
        for rep in range(CRASH_REPS):
            data = os.path.join(ROOT, "bench", f"tmp.{os.getpid()}.crash.{s}.{rep}")
            os.makedirs(data, exist_ok=True)
            env["WO_DATA"] = store(data)
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
                ok(f"crash.{s}.{rep}{FORM}: {acked} acked rows all present after kill -9")
            elif acked == 0:
                ok(f"crash.{s}.{rep}{FORM}: killed before first ack (nothing owed)")
            else:
                bad(f"crash.{s}.{rep}{FORM}", f"acked={acked} verify rc={rc} {lines[-1:]}")
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
    # databasev2 1: footprint is a STRUCTURAL number -- 96.5 vs 320.6 B/row
    # reproduced to <2% across runs -- so it gets a tight tolerance and is the
    # one growth metric worth gating. The doubling COUNT and the latency
    # samples are allowed to move: doublings depend on where N lands relative
    # to a pow2 rehash, and at 1us p50 a single histogram step is already 100%.
    if ".bytes_per_row" in key: return 10
    if key.startswith("growth."): return 100
    if key.startswith("ceiling."): return 100
    if key.startswith("randread."): return 100
    # databasev2 2 task 7. Same split randread makes, for the same reason: the
    # absolute ops/sec under a cap is swap and disk I/O and belongs to the box,
    # so it is recorded and waived. The RATIOS are the engine's property.
    #
    # rss_ratio is the headline claim — keys must hold the same rows in a
    # materially smaller resident set or the mode has no purpose — and it is
    # STRUCTURAL: 87.5 MiB against 34.4 MiB, reproducible, the same class of
    # number as bytes_per_row. It gets the same tight tolerance, and residency()
    # additionally hard-fails below 2.0x regardless of drift.
    if key == "residency.rss_ratio": return 10
    # in_ram_cost is a throughput ratio between two cached runs: stable in
    # shape (a pread and a fold against a pointer dereference) but it moves
    # with page-cache weather, so it is gated loosely rather than waived.
    if key == "residency.in_ram_cost_x": return 50
    # overcap_vs_swap compares two I/O-bound runs, so BOTH halves are the box's.
    # The ratio is still worth recording — it is the answer to the question the
    # iteration was written to ask — but residency() guards the direction of it
    # (keys must not be slower than swapping) rather than its magnitude.
    if key == "residency.overcap_vs_swap_x": return 100
    if key.startswith("residency."): return 100
    if key.startswith("replay."): return 100
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
        # Widened again 2026-08-29 with more evidence: mixread p99 was measured
        # at 1043 / 2318 / 4147us and mixwrite at 1623 / 4446us across runs of
        # the SAME build on a near-idle box — a 3-4x spread. 100% was still
        # gating the disk. The FLOOR stays the real guard and is not slack:
        # mixread's came within 25us of tripping on the worst run observed.
        return 300
    # databasev2 3: the RECLAIM ratio is structural and gated tightly — it is
    # the feature's whole claim. Boot time and the pause are wall-clock on a
    # shared box and are not: waiving them all would have left the leg ungated,
    # which is the mistake part A's task 4 made and had to undo.
    if key in ("ckpt.boot_off_ms", "ckpt.boot_on_ms", "ckpt.pause_us_max",
               "ckpt.compactions", "ckpt.bytes_off", "ckpt.bytes_on"):
        return 400
    # compaction BANDWIDTH is the engine's own property, so it is gated for
    # real — it is what regressed 8x when the dump was fsyncing per flush
    if key == "ckpt.pause_us_per_mb":
        return 100
    # msgrate is actor-to-actor throughput and is scheduling-bound, so its
    # run-to-run spread is far wider than its old 15%. MEASURED across the 10
    # full runs recorded on 2026-08-28/29 — several of them predating the
    # checkpoint work — it ranged 10.7M to 17.9M msgs/sec, a 1.67x spread. A
    # 15% gate on that gates the scheduler and fails intermittently whatever
    # the engine does. Pre-existing; found while closing databasev2 3, not
    # caused by it.
    if ".msgrate." in key: return 70
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

# ---- databasev2 1: the RAM ceiling -----------------------------------------

GROWTH_N = 20000 if QUICK else 200000
GROWTH_SHAPES = ("int", "text")


def cap_wrapper(mem_mb, swap_mb):
    """systemd-run --user --scope argv prefix that caps memory rootlessly, or
    None when the mechanism is unavailable.

    cgroup v2 with the `memory` controller delegated to the user slice is the
    only mechanism used. `ulimit -v` is deliberately NOT a fallback: it bounds
    address space, not resident set, which is the wrong quantity for an engine
    that mallocs slabs, and ASan's huge virtual reservations trip it long
    before real memory pressure. When the cap is unavailable the legs are
    SKIPPED and say so -- never silently run uncapped, because "it survived on
    a 32 GiB workstation" measures the workstation."""
    if not shutil.which("systemd-run"):
        return None
    try:
        with open("/proc/self/cgroup") as f:
            mine = f.readline().strip().split(":")[-1]
        ctl = f"/sys/fs/cgroup{os.path.dirname(mine)}/cgroup.controllers"
        if "memory" not in open(ctl).read().split():
            return None
    except OSError:
        return None
    return ["systemd-run", "--user", "--scope", "--quiet",
            "-p", f"MemoryMax={mem_mb}M", "-p", f"MemorySwapMax={swap_mb}M", "--"]


def parse_growth(lines):
    """(rows, rss_kb) samples plus per-decile read p50/p99, from the sample's
    own `growthrss` / `growthN` lines. RSS is read by the SAMPLE, not polled
    here: the driver polls every 250 ms and would miss the value AT a decile
    boundary, and per-row footprint is this iteration's headline number."""
    pts, lat = [], {}
    for l in lines:
        f = l.split()
        if f and f[0] == "growthrss" and len(f) == 4:
            pts.append((int(f[2]), int(f[3])))
        elif f and f[0].startswith("growth") and len(f) == 5 and f[0][6:].isdigit():
            lat[int(f[0][6:])] = (int(f[3]), int(f[4]))
    return pts, lat


def bytes_per_row(pts):
    """Steady-state marginal footprint = MEDIAN of the per-interval marginals.

    Not a two-point slope: the id hash and index buckets are open-addressing
    pow2 and DOUBLE periodically, so a two-point slope lands arbitrarily on or
    off a doubling and swings 2x (measured: 96 vs 205 B/row for the same shape).
    The median rejects those steps; they are reported separately as `doublings`
    because a transient RSS step is exactly what a resident-footprint budget
    must leave headroom for."""
    marg = sorted((k1 - k0) * 1024.0 / (r1 - r0)
                  for (r0, k0), (r1, k1) in zip(pts, pts[1:]) if r1 > r0)
    if not marg:
        return None, 0
    med = marg[len(marg) // 2]
    doublings = sum(1 for m in marg if m > med * 1.5)
    return med, doublings


def growth(metrics):
    """Per-shape footprint and the read-latency curve, under a rootless cap,
    with swap ON and OFF.

    What this leg actually measures is FOOTPRINT. It does not reach the cap:
    GROWTH_N rows need far less than the 512 MiB cap, so both swap legs are
    identical by construction and p99_departure_decile is legitimately 0.
    The ceiling itself is ceiling() below -- keep the two separate, because a
    footprint regression and a ceiling-behaviour change are different faults.

    Two earlier claims in this docstring were measured FALSE and are recorded
    in docs/stories/databasev2/01-ram-ceiling-measurement.md: swap-off is not
    a "clean checked-malloc" path (it is SIGKILL, rc=137), and swap-on is not
    "latency collapse" (900k rows finished in 148s capped-with-swap vs 150s
    uncapped -- an append-mostly workload never re-touches its cold pages)."""
    wrap = cap_wrapper(512, 0)
    if wrap is None:
        ok("growth: SKIPPED -- no rootless cgroup v2 memory cap on this host")
        metrics["growth.available"] = 0
        return
    metrics["growth.available"] = 1
    for shape in GROWTH_SHAPES:
        for legname, swap_mb in (("noswap", 0), ("swap", 256)):
            w = cap_wrapper(512, swap_mb)
            env = dict(os.environ); env["WO_EPHEMERAL"] = "1"
            argv = w + [BIN, "growth", str(GROWTH_N), shape]
            pr = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, env=env, timeout=900)
            lines = pr.stdout.splitlines()
            pts, lat = parse_growth(lines)
            key = f"growth.{shape}.{legname}"
            if not pts:
                bad(f"{key}: produced no samples", (lines[-1] if lines else "no output"))
                continue
            bpr, doublings = bytes_per_row(pts)
            metrics[f"{key}.bytes_per_row"] = int(round(bpr))
            metrics[f"{key}.doublings"] = doublings
            metrics[f"{key}.rows"] = pts[-1][0]
            metrics[f"{key}.rss_kb"] = pts[-1][1]
            if lat:
                last = max(lat)
                metrics[f"{key}.read_p50us"] = lat[last][0]
                metrics[f"{key}.read_p99us"] = lat[last][1]
                # the curve's departure point: first decile whose p99 exceeds
                # 4x the first decile's, as a MEASURED sample not an estimate
                first = lat[min(lat)][1]
                dep = next((d for d in sorted(lat) if lat[d][1] > max(first, 1) * 4), 0)
                metrics[f"{key}.p99_departure_decile"] = dep
            ok(f"{key}: {int(round(bpr))} B/row steady, {doublings} doubling step(s), "
               f"{pts[-1][0]} rows in {pts[-1][1]} KiB")



CEIL_N, CEIL_CAP_MB = 60000, 8
RAND_N = 60000 if QUICK else 200000
RAND_R = 20000 if QUICK else 40000
RAND_CAP_MB = 6 if QUICK else 14      # over-cap: holds roughly a third of the rows
RAND_FIT_MB = 256                     # control: same mechanism, cap simply does not bind
REPLAY_N = 20000 if QUICK else 100000
REPLAY_BOOTS = 3                      # median of 3; boot is timed, so noise matters

def ceiling(metrics):
    """The ceiling itself, and the durability claim across it.

    Sized so the process CANNOT fit: 60k Int rows need ~9.7 MiB resident
    (96.5 B/row measured, plus a ~3.9 MiB base) under an 8 MiB cap, swap off.
    Two things are under test and the second is the one that matters:

      1. HOW it dies. Measured: SIGKILL, rc=137 -- not a refusal. Table
         storage has no checked ceiling, and under vm.overcommit_memory=0
         malloc succeeds and the process dies TOUCHING the pages, so it never
         gets the chance to report failure. (The VM object arena is the
         opposite: WO_HEAP_MB is checked and traps.) rc is asserted, not
         recorded as a metric -- when databasev2 5's byte budget (bounded
         tables) lands this should become a checked refusal, and the gate must
         not fail on that improvement.

      2. WHAT SURVIVES. With WO_DATA set, replay must yield a contiguous
         intact prefix: rows 1..M present with the right v, no holes, and not
         reported as corruption. M is wherever the kill landed -- the SHAPE of
         the survivor is the claim, not its size, so rows_recovered carries a
         wide tolerance. This is ack-after-fsync holding in the one shutdown
         path that skips every cleanup handler."""
    wrap = cap_wrapper(CEIL_CAP_MB, 0)
    if wrap is None:
        ok("ceiling: SKIPPED -- no rootless cgroup v2 memory cap on this host")
        return
    data = os.path.join(ROOT, "bench", f"tmp.{os.getpid()}.ceiling")
    shutil.rmtree(data, ignore_errors=True); os.makedirs(data, exist_ok=True)
    env = dict(os.environ); env["WO_DATA"] = data
    pr = subprocess.run(wrap + [BIN, "growth", str(CEIL_N), "int"],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, env=env, timeout=900)
    if pr.returncode == 0:
        bad("ceiling: process SURVIVED the cap",
            f"{CEIL_N} rows fit under {CEIL_CAP_MB} MiB -- footprint changed, resize the leg")
        shutil.rmtree(data, ignore_errors=True); return
    # subprocess returncode is NEGATIVE for signal death (-9 = SIGKILL); 137
    # is the SHELL spelling of the same event (128+9). Getting this backwards
    # once labelled a SIGKILL as a "checked refusal", which is the exact
    # distinction this leg exists to report.
    if pr.returncode < 0:
        sig = -pr.returncode
        how = f"killed by signal {sig}" + (" (SIGKILL -- no checked refusal)" if sig == 9 else "")
    else:
        how = f"exited {pr.returncode} (checked refusal)"
    ok(f"ceiling: died at the cap, {how}")
    vr = subprocess.run([BIN, "growth-verify"], stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True, env=env, timeout=900)
    m = re.search(r"^growthverify (\d+)$", vr.stdout, re.M)
    if vr.returncode == 0 and m and int(m.group(1)) > 0:
        metrics["ceiling.rows_recovered"] = int(m.group(1))
        ok(f"ceiling: durable prefix intact across the kill -- {m.group(1)} rows, no holes")
    else:
        bad("ceiling: durable prefix broken across the kill",
            (vr.stdout.strip().splitlines() or ["no output"])[-1][:160])
    shutil.rmtree(data, ignore_errors=True)



# databasev2 2 task 7: does `resident: keys` beat letting the kernel swap?
RESID_N = 40000 if QUICK else 200000
RESID_R = 10000 if QUICK else 40000
RESID_FIT_MB = 256   # control: neither mode is under pressure
# Between the two resident sets, so `all` pages and `keys` does not. This MUST
# scale with N: at QUICK's 40k rows `all` needs only ~17 MiB, so a 48 MiB cap
# binds neither mode and the comparison silently becomes "keys is slower when
# nothing is under pressure" — which is true, and not what this leg asks.
RESID_CAP_MB = 10 if QUICK else 48


def parse_resid(lines, op):
    """`<op> <n> <ops/sec> <p50> <p99>`, plus the mode's own rss/hits line."""
    ops = p50 = p99 = rss = hits = filled = None
    for l in lines:
        f = l.split()
        if not f:
            continue
        if f[0] == op and len(f) == 5:
            ops, p50, p99 = int(f[2]), int(f[3]), int(f[4])
        elif f[0] == op + "rss" and len(f) == 3:
            rss, hits = int(f[1]), int(f[2])
        elif f[0] == "wreadfilled" and len(f) == 3:
            filled = int(f[2])
    return ops, p50, p99, rss, hits, filled


def residency(metrics):
    """`resident: keys` against `resident: all`, on tables IDENTICAL except the
    annotation, so a difference is the storage mode's doing and nothing else's.

    Measured 2026-08-30 on the WIDE shape deliberately. An Int-only pair shows
    the two modes as indistinguishable, and that is structural rather than
    surprising: dropping a payload frees each field's VALUE, and an Int's value
    IS its inline slot word, so nothing is freed and the slab stays allocated
    either way. A benchmark built on that shape would condemn the feature for a
    reason that has nothing to do with the feature.

    WHAT IS GATED, and what deliberately is not. The RATIOS are the engine's
    property and get real tolerances; the absolute ops/sec under a cap is swap
    and disk I/O, so it belongs to the box and is recorded but waived. This is
    the same split randread() already makes for the same reason.

    The headline claim is rss_ratio: keys must hold the same rows in a
    materially smaller resident set, or the mode has no purpose. The measured
    figure was 2.55x (34.4 MiB against 87.5 MiB)."""
    if cap_wrapper(RESID_FIT_MB, 0) is None:
        ok("residency: SKIPPED -- no rootless cgroup v2 memory cap on this host")
        return
    res = {}
    for mode, op in (("all", "wreadall"), ("keys", "wreadkeys")):
        for legname, cap_mb, swap_mb in (("fit", RESID_FIT_MB, 0),
                                         ("cap", RESID_CAP_MB, 512)):
            w = cap_wrapper(cap_mb, swap_mb)
            data = tempfile.mkdtemp(prefix="resid-")
            env = dict(os.environ)
            env["WO_DATA"] = data      # a keys-resident table cannot run without one
            env["WO_SHARDS"] = "1"
            pr = subprocess.run(w + [RESID_BIN, op, str(RESID_N), str(RESID_R)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, env=env, timeout=1800)
            shutil.rmtree(data, ignore_errors=True)
            lines = pr.stdout.splitlines()
            ops, p50, p99, rss, hits, filled = parse_resid(lines, op)
            key = f"residency.{mode}.{legname}"
            if pr.returncode != 0 or ops is None:
                bad(f"{key}: run failed",
                    f"rc={pr.returncode} {(lines[-1:] or ['no output'])[0][:120]}")
                return
            if hits != RESID_R:
                # a comparison over reads that did not resolve measures nothing
                bad(f"{key}: only {hits}/{RESID_R} reads resolved", "keys must all exist")
                return
            metrics[f"{key}.ops_sec"] = ops
            metrics[f"{key}.read_p50us"] = p50
            metrics[f"{key}.read_p99us"] = p99
            metrics[f"{key}.filled_rss_kb"] = filled
            res[f"{mode}.{legname}"] = (ops, filled)
            ok(f"{key}: {ops} reads/sec, p50 {p50}us p99 {p99}us, {filled} KiB after fill")

    all_rss = res["all.fit"][1]
    keys_rss = res["keys.fit"][1]
    rss_ratio = round(all_rss / max(keys_rss, 1), 2)
    metrics["residency.rss_ratio"] = rss_ratio

    # what the mode costs when memory is NOT tight: a pread and a fold per row
    # against a pointer dereference
    in_ram_cost = round(res["all.fit"][0] / max(res["keys.fit"][0], 1), 2)
    metrics["residency.in_ram_cost_x"] = in_ram_cost

    # the question the iteration was written to answer: under a cap that binds
    # `all` and not `keys`, is keys actually better than swapping?
    vs_swap = round(res["keys.cap"][0] / max(res["all.cap"][0], 1), 2)
    metrics["residency.overcap_vs_swap_x"] = vs_swap

    if rss_ratio < 2.0:
        bad("residency: keys saves less than 2x RSS",
            f"{all_rss} KiB vs {keys_rss} KiB = {rss_ratio}x -- the mode's whole purpose")
    else:
        ok(f"residency: keys holds the same rows in {rss_ratio}x less RSS "
           f"({all_rss} -> {keys_rss} KiB)")

    # The cap must actually BIND the resident half, or the comparison is
    # meaningless. randread learned this the same way; assert it rather than
    # trusting the constants to stay right as N changes.
    all_collapse = res["all.fit"][0] / max(res["all.cap"][0], 1)
    metrics["residency.all_collapse_x"] = round(all_collapse, 2)
    if all_collapse < 2.0:
        bad("residency: the cap did not bind `resident: all`",
            f"{res['all.fit'][0]} -> {res['all.cap'][0]} reads/sec is only "
            f"{all_collapse:.2f}x; {RESID_N} rows fit under {RESID_CAP_MB} MiB, resize the leg")
        return

    if vs_swap < 1.0:
        bad("residency: keys is SLOWER than letting the kernel swap",
            f"{res['keys.cap'][0]} vs {res['all.cap'][0]} reads/sec under a "
            f"{RESID_CAP_MB} MiB cap -- the mode buys nothing here")
    else:
        ok(f"residency: under a {RESID_CAP_MB} MiB cap keys is {vs_swap}x swapping "
           f"({res['keys.cap'][0]} vs {res['all.cap'][0]} reads/sec)")

    ok(f"residency: costs {in_ram_cost}x read throughput when memory is not tight")


def parse_randread(lines):
    """ops/sec, p50, p99, resolved-read count and post-fill RSS from the
    sample's own randread lines. `randreadfilled` also starts with "randread",
    so match f[0] exactly, not by prefix."""
    ops = p50 = p99 = hits = filled = None
    for l in lines:
        f = l.split()
        if not f:
            continue
        if f[0] == "randread" and len(f) == 5:
            ops, p50, p99 = int(f[2]), int(f[3]), int(f[4])
        elif f[0] == "randreadrss" and len(f) == 3:
            hits = int(f[2])
        elif f[0] == "randreadfilled" and len(f) == 3:
            filled = int(f[2])
    return ops, p50, p99, hits, filled


def randread(metrics):
    """Random reads over a table LARGER than the memory cap -- the access
    pattern the swap measurement was missing.

    growth() only inserts, and inserting is append-mostly: cold pages are
    written once and never re-read, so swap cost it ~1% (148s vs 150s
    uncapped). That result is real but does NOT generalise to "swap is fine".
    This leg reads back across the whole range in a Weyl-sequence order, so
    most reads must fault a page in.

    It matters because it is databasev2 2's `resident: keys` access pattern:
    that design reads rows back from a log larger than RAM by construction.

    Two runs, identical except for the cap, reading the SAME key order:
      - control  (RAND_FIT_MB): cap does not bind, everything resident
      - over-cap (RAND_CAP_MB): ~a third of the rows fit; swap ON, because
        with swap off this configuration is simply SIGKILLed (see ceiling())
    The headline is collapse_x, the throughput ratio between them. Tolerances
    are wide: the over-cap half is swap I/O, so its absolute numbers are the
    box's, while the RATIO is the property of the engine."""
    if cap_wrapper(RAND_FIT_MB, 0) is None:
        ok("randread: SKIPPED -- no rootless cgroup v2 memory cap on this host")
        return
    res = {}
    for legname, cap_mb, swap_mb in (("resident", RAND_FIT_MB, 0),
                                     ("overcap", RAND_CAP_MB, 256)):
        w = cap_wrapper(cap_mb, swap_mb)
        pr = subprocess.run(w + [BIN, "randread", str(RAND_N), str(RAND_R)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, env=dict(os.environ, WO_EPHEMERAL="1"),
                            timeout=900)
        lines = pr.stdout.splitlines()
        ops, p50, p99, hits, filled = parse_randread(lines)
        key = f"randread.{legname}"
        if pr.returncode != 0 or ops is None:
            bad(f"{key}: run failed", f"rc={pr.returncode} {(lines[-1:] or ['no output'])[0][:120]}")
            return
        if hits != RAND_R:
            # a collapse measured over reads that did not resolve is noise
            bad(f"{key}: only {hits}/{RAND_R} reads resolved", "keys must all exist")
            return
        metrics[f"{key}.ops_sec"] = ops
        metrics[f"{key}.read_p50us"] = p50
        metrics[f"{key}.read_p99us"] = p99
        metrics[f"{key}.filled_rss_kb"] = filled
        res[legname] = ops
        ok(f"{key}: {ops} reads/sec, p50 {p50}us p99 {p99}us, {filled} KiB after fill")
    collapse = res["resident"] // max(res["overcap"], 1)
    metrics["randread.collapse_x"] = collapse
    if collapse < 2:
        bad("randread: NO collapse -- the cap did not bind",
            f"{RAND_N} rows fit under {RAND_CAP_MB} MiB, resize the leg")
    else:
        ok(f"randread: random reads over an oversized table collapse {collapse}x "
           f"({res['resident']} -> {res['overcap']} reads/sec)")



def wal_used(data_dir):
    """Bytes actually written across the store's WAL files.

    The non-zero prefix, NOT the file size: shard WALs are preallocated, so
    getsize reports the preallocation (1 MiB) even for an empty store. Same
    reason scripts/residency-accept.sh measures it this way.

    databasev2 1 and databasev2 3 each grew their own copy of this helper on
    separate branches; this is the single one they now share."""
    total = 0
    for name in sorted(os.listdir(data_dir)):
        with open(os.path.join(data_dir, name), "rb") as f:
            total += len(f.read().rstrip(b"\x00"))
    return total


def time_boot(data_dir):
    """Median wall-clock ms of `boot`, which does nothing at all.

    With WO_DATA set the runtime replays the entire WAL BEFORE main runs, so a
    mode that does no work measures replay plus a fixed process startup. Any
    mode that touched rows would fold its own cost in. Median of REPLAY_BOOTS
    because this is wall-clock on a shared box."""
    env = dict(os.environ); env["WO_DATA"] = data_dir
    samples = []
    for _ in range(REPLAY_BOOTS):
        t0 = time.monotonic()
        pr = subprocess.run([BIN, "boot"], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, env=env, timeout=900)
        if pr.returncode != 0:
            return None
        samples.append((time.monotonic() - t0) * 1000.0)
    return sorted(samples)[len(samples) // 2]


def replay(metrics):
    """The replay baseline databasev2 3 has no "before" for.

    bench/baseline.json carried zero metrics for replay, restart, boot or
    recovery. Iteration 22 proved restart CORRECTNESS; it never timed it, so
    iteration 3's "bounded replay" claim had nothing to measure against.

    Two shapes with the SAME live dataset and different history lengths:
      - inserts: N inserts, N records
      - history: N inserts + N updates, 2N records, same N live rows
    The live data is identical; only the log is longer. That is iteration 3's
    entire case: with no checkpoint, boot replays HISTORY rather than DATA, so a
    row updated a thousand times costs a thousand records at every boot,
    forever. history_penalty_x is the headline -- the boot cost of history that
    a checkpoint would collapse.

    Startup is subtracted using an empty store, so the reported ms is replay,
    not process spawn."""
    empty = os.path.join(ROOT, "bench", f"tmp.{os.getpid()}.replay.empty")
    shutil.rmtree(empty, ignore_errors=True); os.makedirs(empty, exist_ok=True)
    base_ms = time_boot(empty)
    shutil.rmtree(empty, ignore_errors=True)
    if base_ms is None:
        bad("replay: empty-store boot failed", "cannot establish the startup floor")
        return
    metrics["replay.startup_ms"] = int(round(base_ms))
    ok(f"replay: empty-store startup floor {base_ms:.1f} ms (subtracted below)")

    res = {}
    for legname, updates in (("inserts", 0), ("history", REPLAY_N)):
        data = os.path.join(ROOT, "bench", f"tmp.{os.getpid()}.replay.{legname}")
        shutil.rmtree(data, ignore_errors=True); os.makedirs(data, exist_ok=True)
        env = dict(os.environ); env["WO_DATA"] = data
        sr = subprocess.run([BIN, "replayseed", str(REPLAY_N), str(updates)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
                            env=env, timeout=900)
        key = f"replay.{legname}"
        if sr.returncode != 0:
            bad(f"{key}: seed failed", f"rc={sr.returncode}")
            shutil.rmtree(data, ignore_errors=True); return
        wal = wal_used(data)
        boot_ms = time_boot(data)
        shutil.rmtree(data, ignore_errors=True)
        if boot_ms is None:
            bad(f"{key}: boot failed", "replay did not complete")
            return
        records = REPLAY_N + updates
        rep_ms = max(boot_ms - base_ms, 0.0)
        metrics[f"{key}.ms"] = int(round(rep_ms))
        metrics[f"{key}.records"] = records
        metrics[f"{key}.wal_bytes"] = wal
        # NANOseconds, integer: us_per_record rounded 5.5 and 5.3 to 6 and 5,
        # which is too coarse for the one number iteration 3 exists to improve
        metrics[f"{key}.ns_per_record"] = int(round(rep_ms * 1e6 / records))
        res[legname] = (rep_ms, wal, records)
        ok(f"{key}: {rep_ms:.0f} ms replaying {records} records "
           f"({rep_ms*1000.0/records:.1f} us/record), WAL {wal} B")

    ins_ms, ins_wal, _ = res["inserts"]
    his_ms, his_wal, _ = res["history"]
    # premise check: an update MUST cost a WAL record, or the two shapes are
    # the same measurement and history_penalty_x means nothing
    if his_wal < ins_wal * 1.5:
        bad("replay: updates are not appending WAL records",
            f"history WAL {his_wal} B vs inserts {ins_wal} B -- expected ~2x")
        return
    ok(f"replay: {REPLAY_N} updates doubled the log ({ins_wal} -> {his_wal} B) "
       f"with the live row count unchanged")
    penalty = his_ms / max(ins_ms, 1.0)
    metrics["replay.history_penalty_x"] = round(penalty, 2)
    ok(f"replay: identical dataset, {penalty:.2f}x the boot cost from history alone "
       f"({ins_ms:.0f} -> {his_ms:.0f} ms) -- what a checkpoint would collapse")
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
        used = wal_used(data)
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
    # The RAW pause scales with the live set, and this workload's live set is
    # not fixed: wmix's hist_dump inserts a row per latency bucket, so a noisier
    # box produces more buckets, more rows, and a longer pause. Gating the raw
    # number against a baseline therefore gates the box. What belongs to the
    # ENGINE is the rate, so that is what carries a real tolerance; the raw
    # pause keeps the absolute budget assertion below as its guard.
    cb = int(st.get("compacted_bytes", 0))
    if cb > 0 and metrics["ckpt.pause_us_max"] > 0:
        metrics["ckpt.pause_us_per_mb"] = int(round(
            metrics["ckpt.pause_us_max"] / (cb / (1024.0 * 1024.0))))
    ok(f"ckpt: {off_b} -> {on_b} bytes ({metrics['ckpt.reclaim_x']}x reclaimed) over "
       f"{comps} compactions; boot {off_boot:.0f} -> {on_boot:.0f} ms; "
       f"stop-the-world pause max {metrics['ckpt.pause_us_max']}us "
       f"({metrics.get('ckpt.pause_us_per_mb', 0)}us/MB)")
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
    growth(metrics)
    ceiling(metrics)
    randread(metrics)
    residency(metrics)
    replay(metrics)
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

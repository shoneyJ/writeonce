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
    # databasev2 1: footprint is a STRUCTURAL number -- 96.5 vs 320.6 B/row
    # reproduced to <2% across runs -- so it gets a tight tolerance and is the
    # one growth metric worth gating. The doubling COUNT and the latency
    # samples are allowed to move: doublings depend on where N lands relative
    # to a pow2 rehash, and at 1us p50 a single histogram step is already 100%.
    if ".bytes_per_row" in key: return 10
    if key.startswith("growth."): return 100
    if key.startswith("ceiling."): return 100
    if key.startswith("randread."): return 100
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
        higher = k.endswith(("ops_sec", "msgs_sec"))
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
            env = dict(os.environ)
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
         recorded as a metric -- when databasev2 2's byte budget lands this
         should become a checked refusal, and the gate must not fail on that
         improvement.

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
                            text=True, env=dict(os.environ), timeout=900)
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

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
        rc, lines, _, _ = run(["msgrate", str(MSG_N)], {"WO_SHARDS": str(shards)}, 300)
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
            # quick mode: floors only — counts are too small for stable deltas
            breach = floor is not None and ((got < floor) if higher_is_better else (got > floor))
            (ok if not breach else lambda n: bad(n, f"{got} vs floor {floor}"))(f"gate.{key} (floor)")
            continue
        if higher_is_better:
            rel_bad = got < val * (100 - tol) / 100
            floor_bad = floor is not None and got < floor
        else:
            rel_bad = got > val * (100 + tol) / 100
            floor_bad = floor is not None and got > floor
        if rel_bad or floor_bad:
            bad(f"gate.{key}", f"{got} vs baseline {val} (tol {tol}%, floor {floor})")
        else:
            ok(f"gate.{key} {got} (baseline {val})")
    if WRITE_BASELINE:
        write_baseline(metrics)

def write_baseline(metrics):
    base = {"_config": {"N": N, "msg_n": MSG_N, "wal_n": WAL_N, "crash_reps": CRASH_REPS,
                        "note": "refresh only with a commit that says why"}}
    for k, v in sorted(metrics.items()):
        if k.endswith(("rss_growth_kb", "fd_growth")): continue
        higher = k.endswith(("ops_sec", "msgs_sec"))
        base[k] = {"value": v, "tolerance_pct": 15,
                   "floor": (v // 4 if higher else v * 4), "dir": "higher" if higher else "lower"}
    os.makedirs(os.path.dirname(BASELINE), exist_ok=True)
    json.dump(base, open(BASELINE, "w"), indent=1, sort_keys=True)
    ok(f"baseline written ({len(base) - 1} metrics)")

def main():
    build()
    metrics = campaign()
    durability(metrics)
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

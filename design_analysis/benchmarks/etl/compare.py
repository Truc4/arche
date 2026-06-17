#!/usr/bin/env python3
"""Run ETL benchmarks across engines and compare checksums + timing, at a chosen dataset size.

Layout:  etl/<size>/<backend>/task_{n}_{name}.{arche,c,py}   (size ∈ {1k,10k,100m})
  - 10k  is the throughput config: each source self-loops N=216,000 times and reports per-iter time.
  - 1k / 100m are single-pass; the source reports load+compute time (or the runner uses wall time).

Usage:
  python compare.py --size 100m --task 4 --engines arche,c,pandas
  python compare.py --size 10k  --task all
  python compare.py --size 1k   --task 1 --engines arche

Each engine prints two lines:  taskN_checksum: <value>   and (optionally)  taskN_time: <seconds>.
The runner captures subprocess wall time plus the internal time line, and reports min/median.
"""

import argparse
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
BENCH_DIR = Path(__file__).resolve().parent
ARCHE_BIN = REPO_ROOT / "build" / "arche"

SIZES = ("1k", "10k", "100m")
CSV_BY_SIZE = {"1k": "data_1k.csv", "10k": "data_10k.csv", "100m": "data_100m.csv"}

_COLD_CACHE = False


def drop_page_cache():
    """Drop the OS page cache. Linux/WSL only. Requires passwordless sudo for sysctl."""
    try:
        proc = subprocess.run(
            ["sudo", "-n", "/sbin/sysctl", "vm.drop_caches=3"],
            capture_output=True, text=True, timeout=30,
        )
        if proc.returncode != 0:
            print(f"    WARN: drop_caches failed ({proc.stderr.strip()[:120]}).", flush=True)
    except FileNotFoundError:
        print("    WARN: sudo or sysctl not found; --cold-cache ignored.", flush=True)


TASKS = {
    1: {"name": "derived_columns", "description": "revenue = price * quantity", "checksum_type": float, "tolerance": 0.01},
    2: {"name": "filter_invalid", "description": "count(quantity > 0)", "checksum_type": int, "tolerance": 0},
    3: {"name": "bucket_timestamps", "description": "price_bucket = price / 10.0", "checksum_type": float, "tolerance": 0.01},
    4: {"name": "aggregate_region", "description": "sum(price * quantity)", "checksum_type": float, "tolerance": 0.01},
    5: {"name": "pipeline", "description": "filter quantity>0 AND price>10, sum(price*quantity)", "checksum_type": float, "tolerance": 0.01},
}


# Layout is <size>/<mode>/<backend>/ — mode ∈ {single-thread, multicore, gpu} is the execution category,
# backend (arche, c, pandas, …) lives under it. The harness DISCOVERS a backend's folder by globbing
# under whatever mode dir holds it — the categorization lives in the file tree, never hardcoded here.
def backend_dir(size, backend):
    hits = sorted(BENCH_DIR.joinpath(size).glob(f"*/{backend}"))
    return hits[0] if hits else (BENCH_DIR / size / backend)


def arche_src(size, n, name):
    return backend_dir(size, "arche") / f"task_{n}_{name}.arche"


def c_dir(size):
    return backend_dir(size, "c")


def c_bin(size, n, name):
    return c_dir(size) / "bin" / f"task_{n}_{name}"


def py_script(size, engine, n, name):
    return backend_dir(size, engine) / f"task_{n}_{name}.py"


def parse_output(stdout, task_n, checksum_type):
    """Pull taskN_checksum and taskN_time from a script's stdout."""
    cs = re.search(rf"task{task_n}_checksum:\s*([\-\d.eE+]+)", stdout)
    tm = re.search(rf"task{task_n}_time:\s*([\d.eE+\-]+)", stdout)
    checksum = checksum_type(cs.group(1)) if cs else None
    internal_time = float(tm.group(1)) if tm else None
    return checksum, internal_time


def run_arche(size, task_n, task_name, csv_path, iterations):
    src = arche_src(size, task_n, task_name)
    if not src.exists():
        return [(None, None, None, f"source not found: {src}")]
    out_bin = REPO_ROOT / "build" / f"task{task_n}_{task_name}_{size}_bench"
    out_bin.parent.mkdir(parents=True, exist_ok=True)
    comp = subprocess.run([str(ARCHE_BIN), "build", "-o", str(out_bin), str(src)],
                          capture_output=True, text=True, timeout=600)
    if comp.returncode != 0:
        return [(None, None, None, comp.stderr)]
    results = []
    for _ in range(iterations):
        if _COLD_CACHE:
            drop_page_cache()
        t0 = time.perf_counter()
        proc = subprocess.run([str(out_bin)], capture_output=True, text=True, timeout=600)
        wall = time.perf_counter() - t0
        if proc.returncode != 0:
            results.append((None, None, wall, proc.stderr))
            continue
        checksum, internal = parse_output(proc.stdout, task_n, TASKS[task_n]["checksum_type"])
        results.append((checksum, internal, wall, None))
    return results


_C_BUILT = set()


def ensure_c_built(size):
    if size in _C_BUILT:
        return True
    if not c_dir(size).exists():
        return False
    proc = subprocess.run(["make", "-s"], cwd=str(c_dir(size)),
                          capture_output=True, text=True, timeout=300)
    if proc.returncode != 0:
        print(f"    C baseline build failed: {proc.stderr.strip()[:200]}", flush=True)
        return False
    _C_BUILT.add(size)
    return True


def run_c(size, task_n, task_name, csv_path, iterations):
    if not ensure_c_built(size):
        return [(None, None, None, f"c build failed / absent for size {size}")]
    binary = c_bin(size, task_n, task_name)
    if not binary.exists():
        return [(None, None, None, f"binary not found: {binary}")]
    results = []
    for _ in range(iterations):
        if _COLD_CACHE:
            drop_page_cache()
        t0 = time.perf_counter()
        proc = subprocess.run([str(binary), str(csv_path)], capture_output=True, text=True, timeout=600)
        wall = time.perf_counter() - t0
        if proc.returncode != 0:
            results.append((None, None, wall, proc.stderr))
            continue
        checksum, internal = parse_output(proc.stdout, task_n, TASKS[task_n]["checksum_type"])
        results.append((checksum, internal, wall, None))
    return results


def run_python_engine(size, engine, task_n, task_name, csv_path, iterations):
    script = py_script(size, engine, task_n, task_name)
    if not script.exists():
        return [(None, None, None, f"script not found: {script}")]
    results = []
    for _ in range(iterations):
        if _COLD_CACHE:
            drop_page_cache()
        t0 = time.perf_counter()
        proc = subprocess.run([sys.executable, str(script), str(csv_path)],
                              capture_output=True, text=True, timeout=600)
        wall = time.perf_counter() - t0
        if proc.returncode != 0:
            results.append((None, None, wall, proc.stderr))
            continue
        checksum, internal = parse_output(proc.stdout, task_n, TASKS[task_n]["checksum_type"])
        results.append((checksum, internal, wall, None))
    return results


def summarize(results):
    successful = [r for r in results if r[3] is None and r[0] is not None]
    if not successful:
        errs = [r[3] for r in results if r[3] is not None]
        return {"ok": False, "error": errs[0] if errs else "no successful runs"}
    internals = [r[1] for r in successful if r[1] is not None]
    walls = [r[2] for r in successful if r[2] is not None]
    return {
        "ok": True,
        "checksum": successful[0][0],
        "internal_min": min(internals) if internals else None,
        "internal_median": statistics.median(internals) if internals else None,
        "internal_max": max(internals) if internals else None,
        "wall_min": min(walls) if walls else None,
        "wall_median": statistics.median(walls) if walls else None,
        "wall_max": max(walls) if walls else None,
        "n_ok": len(successful),
        "n_total": len(results),
    }


def checksums_match(a, b, tolerance):
    if a is None or b is None:
        return False
    if tolerance == 0:
        return a == b
    if b == 0:
        return abs(a - b) < tolerance
    return abs(a - b) / abs(b) < tolerance


def run_task(size, task_n, engines, csv_path, iterations):
    task = TASKS[task_n]
    print(f"\n=== [{size}] Task {task_n} ({task['name']}): {task['description']} ===")
    print(f"CSV: {csv_path}")
    print(f"Engines: {','.join(engines)}    Iterations: {iterations}")
    summaries = {}
    for engine in engines:
        print(f"\n  Running {engine}...", flush=True)
        if engine == "arche":
            results = run_arche(size, task_n, task["name"], csv_path, iterations)
        elif engine == "c":
            results = run_c(size, task_n, task["name"], csv_path, iterations)
        else:
            results = run_python_engine(size, engine, task_n, task["name"], csv_path, iterations)
        s = summarize(results)
        summaries[engine] = s
        if not s["ok"]:
            print(f"    FAILED: {s['error'].strip()[:200]}")
        else:
            it = f"{s['internal_min']:.4f}s" if s["internal_min"] is not None else "—"
            print(f"    checksum={s['checksum']}  internal_min={it}  wall_min={s['wall_min']:.3f}s  "
                  f"({s['n_ok']}/{s['n_total']} runs)")
    print()
    print_table(task_n, summaries)
    return summaries


def print_table(task_n, summaries):
    tolerance = TASKS[task_n]["tolerance"]
    ref = "pandas" if summaries.get("pandas", {}).get("ok") else None
    if ref is None:
        for cand in ("arche", "duckdb", "polars", "datafusion", "c"):
            if summaries.get(cand, {}).get("ok"):
                ref = cand
                break
    ref_checksum = summaries[ref]["checksum"] if ref else None
    ref_internal = summaries[ref]["internal_min"] if ref else None
    ref_wall = summaries[ref]["wall_min"] if ref else None

    header = (f"  {'engine':<12} {'wall_min':>10} {'wall_med':>10}  "
              f"{'int_min':>10} {'int_med':>10}  {'speedup':>8} {'match':>8}  checksum")
    print(header)
    print("  " + "-" * (len(header) - 2))
    for engine, s in summaries.items():
        if not s["ok"]:
            print(f"  {engine:<12} {'FAIL':>10}")
            continue
        et = s["internal_min"] if s["internal_min"] is not None else s["wall_min"]
        rt = ref_internal if (ref_internal is not None and s["internal_min"] is not None) else ref_wall
        speedup = f"{rt / et:>7.2f}x" if rt and et else "—"
        match = "ref" if engine == ref else ("ok" if checksums_match(s["checksum"], ref_checksum, tolerance) else "MISMATCH")

        def fmt(v):
            return f"{v:>9.4f}s" if v is not None else f"{'—':>10}"

        print(f"  {engine:<12} {fmt(s['wall_min'])} {fmt(s['wall_median'])}  "
              f"{fmt(s['internal_min'])} {fmt(s['internal_median'])}  {speedup:>8} {match:>8}  {s['checksum']}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--size", choices=SIZES, default="100m", help="Dataset size (default: 100m)")
    p.add_argument("--task", default="all", help="Task number (1-5) or 'all' (default: all)")
    p.add_argument("--engines", default="arche,c,pandas,polars,duckdb,datafusion",
                   help="Comma-separated engines (default: all)")
    p.add_argument("--iterations", type=int, default=10, help="Iterations per engine (default: 10)")
    p.add_argument("--csv", default=None, help="Input CSV path (default: data/<data_for_size>.csv)")
    p.add_argument("--cold-cache", action="store_true", help="Drop OS page cache before each iteration")
    args = p.parse_args()

    global _COLD_CACHE
    _COLD_CACHE = args.cold_cache

    engines = [e.strip() for e in args.engines.split(",") if e.strip()]
    valid = {"arche", "c", "pandas", "polars", "duckdb", "datafusion"}
    for e in engines:
        if e not in valid:
            print(f"Unknown engine: {e}", file=sys.stderr)
            sys.exit(2)

    task_nums = sorted(TASKS) if args.task == "all" else None
    if task_nums is None:
        try:
            n = int(args.task)
        except ValueError:
            print(f"Invalid --task: {args.task}", file=sys.stderr)
            sys.exit(2)
        if n not in TASKS:
            print(f"Unknown task: {n}", file=sys.stderr)
            sys.exit(2)
        task_nums = [n]

    csv_path = Path(args.csv).resolve() if args.csv else (BENCH_DIR / "data" / CSV_BY_SIZE[args.size]).resolve()
    if not csv_path.exists():
        print(f"CSV not found: {csv_path}  (generate it with data/generate_data.py)", file=sys.stderr)
        sys.exit(1)

    all_ok = True
    for task_n in task_nums:
        summaries = run_task(args.size, task_n, engines, csv_path, args.iterations)
        ref = None
        for cand in ("pandas", "arche", "duckdb", "polars", "datafusion", "c"):
            if summaries.get(cand, {}).get("ok"):
                ref = summaries[cand]["checksum"]
                break
        if ref is not None:
            for s in summaries.values():
                if s["ok"] and not checksums_match(s["checksum"], ref, TASKS[task_n]["tolerance"]):
                    all_ok = False
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()

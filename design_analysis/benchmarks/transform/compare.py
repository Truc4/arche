#!/usr/bin/env python3
"""Transform-only benchmark runner.

Each engine's task script does: load CSV (off the clock) → time(N=216,000 iter of the
task body) → write computed output → print `taskN_time` and `taskN_checksum`. This runner
invokes each (engine, task) once and tabulates the results.

Usage:
  python compare.py --task all --engines arche,c,pandas,polars,duckdb,datafusion
  python compare.py --task 1 --engines arche,c
"""

import argparse
import re
import statistics  # noqa: F401  (kept for forward compat)
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
BENCH_DIR = Path(__file__).resolve().parent
DEFAULT_CSV = REPO_ROOT / "design_analysis/benchmarks/etl/data/data_10k.csv"
VENV_PY = REPO_ROOT / "design_analysis/benchmarks/etl/.venv/bin/python3"

TASKS = {
    1: {"name": "derived_columns",    "checksum_type": float, "tolerance": 0.01},
    2: {"name": "filter_invalid",     "checksum_type": int,   "tolerance": 0},
    3: {"name": "bucket_timestamps",  "checksum_type": float, "tolerance": 0.01},
    4: {"name": "aggregate_region",   "checksum_type": float, "tolerance": 0.01},
    5: {"name": "pipeline",           "checksum_type": float, "tolerance": 0.01},
}

ENGINES = {
    "pandas":     {"kind": "py",    "path": "pandas/task_{n}_{name}.py"},
    "polars":     {"kind": "py",    "path": "polars/task_{n}_{name}.py"},
    "duckdb":     {"kind": "py",    "path": "duckdb/task_{n}_{name}.py"},
    "datafusion": {"kind": "py",    "path": "datafusion/task_{n}_{name}.py"},
    "c":          {"kind": "bin",   "path": "c/bin/task_{n}_{name}"},
    "arche":      {"kind": "arche", "path": "arche/bin/task_{n}"},
}


def parse_output(stdout, task_n, checksum_type):
    cs_re = re.compile(rf"task{task_n}_checksum:\s*([\-\d.eE+]+)")
    t_re  = re.compile(rf"task{task_n}_time:\s*([\-\d.eE+]+)")
    cs = cs_re.search(stdout)
    tt = t_re.search(stdout)
    return (
        checksum_type(cs.group(1)) if cs else None,
        float(tt.group(1)) if tt else None,
    )


def invoke_engine(engine, task_n, task_name, csv_path, timeout):
    cfg = ENGINES[engine]
    path = BENCH_DIR / cfg["path"].format(n=task_n, name=task_name)
    if cfg["kind"] == "py":
        cmd = [str(VENV_PY), str(path), str(csv_path)]
    else:  # bin or arche
        cmd = [str(path), str(csv_path)]
    if not Path(cmd[1] if cfg["kind"] == "py" else cmd[0]).exists():
        return None, None, None, f"binary/script missing: {path}"
    t0 = time.perf_counter()
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    wall = time.perf_counter() - t0
    if proc.returncode != 0:
        return None, None, wall, proc.stderr.strip()[:300] or proc.stdout.strip()[:300]
    checksum, per_iter = parse_output(proc.stdout, task_n, TASKS[task_n]["checksum_type"])
    return checksum, per_iter, wall, None


def checksums_match(a, b, tol):
    if a is None or b is None:
        return False
    if tol == 0:
        return a == b
    denom = abs(b) if b != 0 else 1.0
    return abs(a - b) / denom < tol


def run_task(task_n, engines, csv_path, timeout):
    task = TASKS[task_n]
    print(f"\n=== Task {task_n} ({task['name']}) ===")
    rows = []
    for engine in engines:
        print(f"  running {engine}…", flush=True)
        checksum, per_iter, wall, err = invoke_engine(engine, task_n, task["name"], csv_path, timeout)
        rows.append({"engine": engine, "checksum": checksum, "per_iter": per_iter, "wall": wall, "err": err})
        if err:
            print(f"    FAILED: {err}")
        else:
            print(f"    per_iter={per_iter*1e6:.3f}µs   checksum={checksum}   total_wall={wall:.1f}s")
    return rows


N_ITERS = 216_000


def print_table(task_n, rows):
    tol = TASKS[task_n]["tolerance"]
    # Reference engine for speedup + checksum match: prefer arche; fall back to
    # the first available engine if arche isn't in this run.
    ref = next((r for r in rows if r["engine"] == "arche" and r["err"] is None), None)
    if ref is None:
        ref = next((r for r in rows if r["err"] is None), None)
    ref_name = ref["engine"] if ref else None
    ref_t = ref["per_iter"] if ref else None
    ref_cs = ref["checksum"] if ref else None

    print(f"\nTask {task_n} ({TASKS[task_n]['name']}):")
    sp_label = f"vs_{ref_name}" if ref_name else "speedup"
    header = f"  {'engine':<12} {'per_iter':>12} {'total_216k':>12}  {sp_label:>12}  {'match':>9}  checksum"
    print(header)
    print("  " + "-" * (len(header) - 2))
    for r in rows:
        if r["err"]:
            print(f"  {r['engine']:<12} {'FAIL':>12}")
            continue
        pi = r["per_iter"]
        total = pi * N_ITERS if pi is not None else None
        pi_str = f"{pi*1e6:>10.3f}µs"
        tot_str = f"{total:>10.3f}s" if total is not None else f"{'—':>12}"
        if ref_t and pi:
            speedup = ref_t / pi
            sp_str = f"{speedup:>10.2f}x"
        else:
            sp_str = "—"
        match = "ref" if r["engine"] == ref_name else ("ok" if checksums_match(r["checksum"], ref_cs, tol) else "MISMATCH")
        print(f"  {r['engine']:<12} {pi_str} {tot_str}  {sp_str:>12}  {match:>9}  {r['checksum']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--task", default="all", help="Task number (1-5) or 'all' (default: all)")
    parser.add_argument("--engines", default="arche,c,pandas,polars,duckdb,datafusion",
                        help="Comma-separated engines (default: all)")
    parser.add_argument("--csv", default=str(DEFAULT_CSV), help=f"Path to input CSV (default: {DEFAULT_CSV})")
    parser.add_argument("--timeout", type=int, default=1800, help="Per-engine timeout in seconds (default: 1800)")
    args = parser.parse_args()

    engines = [e.strip() for e in args.engines.split(",") if e.strip()]
    for e in engines:
        if e not in ENGINES:
            print(f"Unknown engine: {e}", file=sys.stderr); sys.exit(2)

    if args.task == "all":
        task_nums = sorted(TASKS.keys())
    else:
        task_nums = [int(args.task)]

    csv_path = Path(args.csv).resolve()
    if not csv_path.exists():
        print(f"CSV not found: {csv_path}", file=sys.stderr); sys.exit(1)

    all_results = {}
    for task_n in task_nums:
        rows = run_task(task_n, engines, csv_path, args.timeout)
        print_table(task_n, rows)
        all_results[task_n] = rows


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Run ETL scale benchmarks across multiple engines and compare checksums + timing.

Usage:
  python compare_scale.py --task 1 --engines arche,pandas,polars,duckdb,datafusion
  python compare_scale.py --task all --iterations 3
  python compare_scale.py --task 2 --engines arche,duckdb --iterations 5

Each engine prints two lines:
  taskN_checksum: <value>
  taskN_time:     <seconds>  (load + compute, excludes process startup)

The runner captures both: subprocess wall time (includes Python/runtime startup) and
the internal time line (load + compute only). Reports min/median across iterations.
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
DEFAULT_CSV = BENCH_DIR / "data" / "data_100m.csv"
ARCHE_BIN = REPO_ROOT / "build" / "arche"

# Set by --cold-cache; drops the OS page cache before each iteration.
# Requires NOPASSWD sudo for /sbin/sysctl:
#   echo "$USER ALL=(ALL) NOPASSWD: /sbin/sysctl vm.drop_caches=3" | sudo tee /etc/sudoers.d/drop_caches
_COLD_CACHE = False


def drop_page_cache():
    """Drop the OS page cache. Linux/WSL only. Requires passwordless sudo for sysctl."""
    try:
        proc = subprocess.run(
            ["sudo", "-n", "/sbin/sysctl", "vm.drop_caches=3"],
            capture_output=True,
            text=True,
            timeout=30,
        )
        if proc.returncode != 0:
            print(
                f"    WARN: drop_caches failed ({proc.stderr.strip()[:120]}); "
                "set up NOPASSWD sudo for /sbin/sysctl to use --cold-cache.",
                flush=True,
            )
    except FileNotFoundError:
        print("    WARN: sudo or sysctl not found; --cold-cache ignored.", flush=True)

TASKS = {
    1: {
        "name": "derived_columns",
        "description": "sum(price * quantity)",
        "checksum_type": float,
        "tolerance": 0.01,
    },
    2: {
        "name": "filter_invalid",
        "description": "count(quantity > 0)",
        "checksum_type": int,
        "tolerance": 0,
    },
    3: {
        "name": "bucket_timestamps",
        "description": "sum(price / 10.0)",
        "checksum_type": float,
        "tolerance": 0.01,
    },
    4: {
        "name": "aggregate_region",
        "description": "sum(price * quantity)",
        "checksum_type": float,
        "tolerance": 0.01,
    },
    5: {
        "name": "pipeline",
        "description": "filter quantity>0 AND price>10, sum(price*quantity)",
        "checksum_type": float,
        "tolerance": 0.01,
    },
}

ENGINE_SCRIPTS = {
    "pandas": "pandas/task_{n}_{name}.py",
    "polars": "polars/task_{n}_{name}.py",
    "duckdb": "duckdb/task_{n}_{name}.py",
    "datafusion": "datafusion/task_{n}_{name}.py",
}

ARCHE_SOURCES = "arche_scale/task_{n}_{name}.arche"
C_BASELINE_DIR = "c_baseline"
C_BASELINE_BIN = "c_baseline/bin/task_{n}_{name}"


def parse_output(stdout, task_n, checksum_type):
    """Pull taskN_checksum and taskN_time from a script's stdout."""
    checksum_re = re.compile(rf"task{task_n}_checksum:\s*([\-\d.eE+]+)")
    time_re = re.compile(rf"task{task_n}_time:\s*([\d.eE+\-]+)")
    cs_match = checksum_re.search(stdout)
    t_match = time_re.search(stdout)
    checksum = checksum_type(cs_match.group(1)) if cs_match else None
    internal_time = float(t_match.group(1)) if t_match else None
    return checksum, internal_time


def run_arche(task_n, task_name, csv_path, iterations):
    """Compile (once) then run the Arche binary `iterations` times."""
    src = BENCH_DIR / ARCHE_SOURCES.format(n=task_n, name=task_name)
    out_bin = REPO_ROOT / "build" / f"task{task_n}_{task_name}_bench"
    out_bin.parent.mkdir(parents=True, exist_ok=True)

    compile_proc = subprocess.run(
        [str(ARCHE_BIN), "-o", str(out_bin), str(src)],
        capture_output=True,
        text=True,
        timeout=600,
    )
    if compile_proc.returncode != 0:
        return [(None, None, None, compile_proc.stderr)]

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


_C_BUILT = False


def ensure_c_built():
    """Build the C baseline binaries once per runner invocation."""
    global _C_BUILT
    if _C_BUILT:
        return True
    proc = subprocess.run(
        ["make", "-s"],
        cwd=str(BENCH_DIR / C_BASELINE_DIR),
        capture_output=True,
        text=True,
        timeout=300,
    )
    if proc.returncode != 0:
        print(f"    C baseline build failed: {proc.stderr.strip()[:200]}", flush=True)
        return False
    _C_BUILT = True
    return True


def run_c(task_n, task_name, csv_path, iterations):
    """Run the hand-written C baseline `iterations` times."""
    if not ensure_c_built():
        return [(None, None, None, "c build failed")]
    binary = BENCH_DIR / C_BASELINE_BIN.format(n=task_n, name=task_name)
    if not binary.exists():
        return [(None, None, None, f"binary not found: {binary}")]

    results = []
    for _ in range(iterations):
        if _COLD_CACHE:
            drop_page_cache()
        t0 = time.perf_counter()
        proc = subprocess.run(
            [str(binary), str(csv_path)], capture_output=True, text=True, timeout=600
        )
        wall = time.perf_counter() - t0
        if proc.returncode != 0:
            results.append((None, None, wall, proc.stderr))
            continue
        checksum, internal = parse_output(proc.stdout, task_n, TASKS[task_n]["checksum_type"])
        results.append((checksum, internal, wall, None))
    return results


def run_python_engine(engine, task_n, task_name, csv_path, iterations):
    """Run a Python engine script `iterations` times."""
    script = BENCH_DIR / ENGINE_SCRIPTS[engine].format(n=task_n, name=task_name)
    if not script.exists():
        return [(None, None, None, f"script not found: {script}")]

    results = []
    for _ in range(iterations):
        if _COLD_CACHE:
            drop_page_cache()
        t0 = time.perf_counter()
        proc = subprocess.run(
            [sys.executable, str(script), str(csv_path)],
            capture_output=True,
            text=True,
            timeout=600,
        )
        wall = time.perf_counter() - t0
        if proc.returncode != 0:
            results.append((None, None, wall, proc.stderr))
            continue
        checksum, internal = parse_output(proc.stdout, task_n, TASKS[task_n]["checksum_type"])
        results.append((checksum, internal, wall, None))
    return results


def summarize(results):
    """Reduce a list of (checksum, internal, wall, err) tuples to summary stats."""
    successful = [r for r in results if r[3] is None and r[0] is not None]
    if not successful:
        errs = [r[3] for r in results if r[3] is not None]
        return {"ok": False, "error": errs[0] if errs else "no successful runs"}

    checksum = successful[0][0]
    internals = [r[1] for r in successful if r[1] is not None]
    walls = [r[2] for r in successful if r[2] is not None]

    return {
        "ok": True,
        "checksum": checksum,
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
    """Compare checksums with the per-task tolerance."""
    if a is None or b is None:
        return False
    if tolerance == 0:
        return a == b
    if b == 0:
        return abs(a - b) < tolerance
    return abs(a - b) / abs(b) < tolerance


def run_task(task_n, engines, csv_path, iterations):
    task = TASKS[task_n]
    print(f"\n=== Task {task_n} ({task['name']}): {task['description']} ===")
    print(f"CSV: {csv_path}")
    print(f"Engines: {','.join(engines)}    Iterations: {iterations}")

    summaries = {}
    for engine in engines:
        print(f"\n  Running {engine}...", flush=True)
        if engine == "arche":
            results = run_arche(task_n, task["name"], csv_path, iterations)
        elif engine == "c":
            results = run_c(task_n, task["name"], csv_path, iterations)
        else:
            results = run_python_engine(engine, task_n, task["name"], csv_path, iterations)
        summary = summarize(results)
        summaries[engine] = summary
        if not summary["ok"]:
            print(f"    FAILED: {summary['error'].strip()[:200]}")
        else:
            internal_str = (
                f"{summary['internal_min']:.3f}s" if summary["internal_min"] is not None else "—"
            )
            print(
                f"    checksum={summary['checksum']}  "
                f"internal_min={internal_str}  "
                f"wall_min={summary['wall_min']:.3f}s  "
                f"({summary['n_ok']}/{summary['n_total']} runs)"
            )

    print()
    print_table(task_n, summaries)
    return summaries


def print_table(task_n, summaries):
    """Print a results table for one task."""
    tolerance = TASKS[task_n]["tolerance"]
    reference_engine = "pandas" if "pandas" in summaries and summaries["pandas"]["ok"] else None
    if reference_engine is None:
        for cand in ("arche", "duckdb", "polars", "datafusion"):
            if cand in summaries and summaries[cand]["ok"]:
                reference_engine = cand
                break

    reference_checksum = summaries[reference_engine]["checksum"] if reference_engine else None
    # Reference time used for speedup: prefer internal_min when both engines have it,
    # but fall back to wall_min for engines that don't report internal (e.g. arche, c).
    reference_internal = summaries[reference_engine]["internal_min"] if reference_engine else None
    reference_wall = summaries[reference_engine]["wall_min"] if reference_engine else None

    header = (
        f"  {'engine':<12} "
        f"{'wall_min':>10} {'wall_med':>10} {'wall_max':>10}  "
        f"{'int_min':>10} {'int_med':>10} {'int_max':>10}  "
        f"{'speedup':>8} {'match':>7}  checksum"
    )
    print(header)
    print("  " + "-" * (len(header) - 2))
    for engine, s in summaries.items():
        if not s["ok"]:
            print(f"  {engine:<12} {'FAIL':>10}")
            continue

        engine_time = s["internal_min"] if s["internal_min"] is not None else s["wall_min"]
        ref_time = (
            reference_internal
            if (reference_internal is not None and s["internal_min"] is not None)
            else reference_wall
        )
        speedup = f"{ref_time / engine_time:>7.2f}x" if ref_time and engine_time else "—"

        match = (
            "ok" if checksums_match(s["checksum"], reference_checksum, tolerance) else "MISMATCH"
        )
        if engine == reference_engine:
            match = "ref"

        def fmt(v):
            return f"{v:>9.3f}s" if v is not None else f"{'—':>10}"

        print(
            f"  {engine:<12} "
            f"{fmt(s['wall_min'])} {fmt(s['wall_median'])} {fmt(s['wall_max'])}  "
            f"{fmt(s['internal_min'])} {fmt(s['internal_median'])} {fmt(s['internal_max'])}  "
            f"{speedup:>8} {match:>7}  {s['checksum']}"
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--task",
        default="all",
        help="Task number (1-4) or 'all' (default: all)",
    )
    parser.add_argument(
        "--engines",
        default="arche,c,pandas,polars,duckdb,datafusion",
        help="Comma-separated engines (default: arche,c,pandas,polars,duckdb,datafusion)",
    )
    parser.add_argument(
        "--iterations",
        type=int,
        default=10,
        help="Iterations per engine (default: 10)",
    )
    parser.add_argument(
        "--csv",
        default=str(DEFAULT_CSV),
        help=f"Path to input CSV (default: {DEFAULT_CSV})",
    )
    parser.add_argument(
        "--cold-cache",
        action="store_true",
        help="Drop OS page cache before each iteration (Linux/WSL only; requires NOPASSWD "
        "sudo for /sbin/sysctl vm.drop_caches=3)",
    )
    args = parser.parse_args()

    global _COLD_CACHE
    _COLD_CACHE = args.cold_cache

    engines = [e.strip() for e in args.engines.split(",") if e.strip()]
    valid_engines = set(ENGINE_SCRIPTS) | {"arche", "c"}
    for e in engines:
        if e not in valid_engines:
            print(f"Unknown engine: {e}", file=sys.stderr)
            sys.exit(2)

    if args.task == "all":
        task_nums = sorted(TASKS.keys())
    else:
        try:
            task_nums = [int(args.task)]
        except ValueError:
            print(f"Invalid --task value: {args.task}", file=sys.stderr)
            sys.exit(2)
        if task_nums[0] not in TASKS:
            print(f"Unknown task: {task_nums[0]}", file=sys.stderr)
            sys.exit(2)

    csv_path = Path(args.csv).resolve()
    if not csv_path.exists():
        print(f"CSV not found: {csv_path}", file=sys.stderr)
        sys.exit(1)

    all_ok = True
    for task_n in task_nums:
        summaries = run_task(task_n, engines, csv_path, args.iterations)
        reference = None
        for cand in ("pandas", "arche", "duckdb", "polars", "datafusion"):
            if cand in summaries and summaries[cand]["ok"]:
                reference = summaries[cand]["checksum"]
                break
        if reference is not None:
            for engine, s in summaries.items():
                if s["ok"] and not checksums_match(
                    s["checksum"], reference, TASKS[task_n]["tolerance"]
                ):
                    all_ok = False

    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()

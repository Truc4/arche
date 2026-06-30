#!/usr/bin/env python3
"""Build-time placement measurement (the real mapper: measure, don't estimate).

For a terminating arche program, this:
  1. lists the GPU-eligible maps (a force-CPU build with ARCHE_PLACE_DEBUG),
  2. times the program with everything on the CPU (baseline),
  3. for each eligible map, times the program with ONLY that map forced to GPU,
  4. writes `<cache>/placement.decisions` (`name gpu|cpu` per map) — the frozen, MEASURED winner,
     which `arche build` then reads at the placement gate (beating the static cost estimate).

This is AutoMap's "run + profile candidates, keep the measured winner," folded into the build because
arche's static pools + folded `#run` make the workload known ahead of runtime. (v1: total-wall-time per
map, so it needs a terminating `#run`; a per-dispatch-timer version would handle `forever` loops.)

Usage:  python3 measure.py <program.arche> [--arche build/arche] [--cache <dir>] [--runs N]
"""
import argparse, os, subprocess, sys, time, tempfile, re

ap = argparse.ArgumentParser()
ap.add_argument("program")
ap.add_argument("--arche", default="build/arche")
ap.add_argument("--cache", default=os.environ.get("ARCHE_CACHE_DIR", ".arche-cache"))
ap.add_argument("--runs", type=int, default=3)
a = ap.parse_args()
os.makedirs(a.cache, exist_ok=True)
tmp = tempfile.mkdtemp(prefix="arche_measure_")
exe = os.path.join(tmp, "p")


def build(env_extra):
    env = dict(os.environ, ARCHE_CACHE_DIR=a.cache, **env_extra)
    r = subprocess.run([a.arche, "build", "--gpu", "-o", exe, a.program],
                       env=env, capture_output=True, text=True)
    return r


def run_ms():
    best = None
    for _ in range(a.runs):
        t0 = time.perf_counter()
        subprocess.run([exe], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        dt = (time.perf_counter() - t0) * 1000.0
        best = dt if best is None else min(best, dt)  # min = least-noisy estimate
    return best


# 1. eligible maps (force CPU so the build always succeeds even with no usable GPU).
r = build({"ARCHE_FORCE_PLACE": "cpu", "ARCHE_PLACE_DEBUG": "1"})
if r.returncode != 0:
    print(r.stderr); sys.exit("measure: baseline build failed")
eligible = []
for m in re.finditer(r"PLACE (\S+):", r.stderr):
    if m.group(1) not in eligible:
        eligible.append(m.group(1))
if not eligible:
    print("measure: no GPU-eligible maps in", a.program); sys.exit(0)
print("eligible maps:", ", ".join(eligible))

# 2. baseline: everything CPU.
build({"ARCHE_FORCE_PLACE": "cpu"})
cpu_ms = run_ms()
print(f"all-CPU baseline: {cpu_ms:.0f} ms")

# 3. per map: only this map on GPU; compare to baseline.
decisions = {}
for mapname in eligible:
    r = build({"ARCHE_FORCE_PLACE_ONLY": mapname})
    if r.returncode != 0:
        print(f"  {mapname}: GPU build failed -> CPU"); decisions[mapname] = "cpu"; continue
    gpu_ms = run_ms()
    win = "gpu" if gpu_ms < cpu_ms else "cpu"
    decisions[mapname] = win
    print(f"  {mapname}: only-GPU {gpu_ms:.0f} ms vs CPU {cpu_ms:.0f} ms -> {win.upper()}")

# 4. freeze the measured winners.
path = os.path.join(a.cache, "placement.decisions")
with open(path, "w") as f:
    for k, v in decisions.items():
        f.write(f"{k} {v}\n")
print("wrote", path)

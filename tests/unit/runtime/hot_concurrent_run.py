#!/usr/bin/env python3
"""
RED bug-capture: two `arche run` sessions of the SAME project collide and crash each other.

`arche run` is hot mode. Each imported device is built to its own `.so` under ARCHE_HOT_DIR, and the
running host polls that path and re-dlopens on change (cli/cmd_run.c, compile/compile.c). The default hot
dir is per-PROJECT — `<project>/build/.arche-hot`, where <project> is the directory holding arche.toml —
so every concurrent run of that project publishes its devices over the same paths and each host re-dlopens
whatever the other just wrote. The result is a SIGSEGV in the host mid-call.

Publishing is already atomic: the `.so` is linked to a pid-tagged temp and renamed into place
(compile/compile.c), and a comment there records an earlier fix for sibling runs racing a fixed `.tmp`
name. What is still shared is the DESTINATION, so atomicity does not help — a complete, correct `.so`
belonging to another host is exactly what gets loaded.

Two things hide it:
  * the SIGSEGV handler (runtime/stack_check.c) prints "stack overflow" for ANY segfault and exits 0 (a
    documented choice — see tests/unit/language/errors/stack_overflow.arche), so a crashed run reports
    SUCCESS to its caller and the only symptom is missing stdout;
  * a fast developer machine usually wins the race. It surfaced first in CI, on a 2-core runner, as
    `extras/camera_smoke.arche` failing while the same commit passed locally 40/40.

`tests/unit/compiler/per_unit/singleton_read.arche` sidesteps this by passing its own ARCHE_HOT_DIR /
ARCHE_CACHE_DIR. That is a property of that test, not a fix: two developers running the same project — or
one developer running it twice — still collide, and so do any two `%arche run` tests in one directory
(today `tests/extras/camera_smoke.arche` and `tests/extras/demo.arche`).

What SHOULD happen: concurrent `arche run` of one project is safe with NO environment set up by the
caller. A one-shot run has nothing to gain from a shared hot dir; a watch session that wants to reuse
build products across invocations can keep doing so without letting a second session load its artifacts.

This test sets NO ARCHE_HOT_DIR and NO ARCHE_CACHE_DIR on purpose — that is the configuration users get.
Expected once fixed: every concurrent run prints its expected line, every time.

It goes red in TWO stages, because it also lands on a second, unrelated bug:

  1. TODAY it fails at IMPORT. `tests/unit/runtime/inspect.py` sits beside this file and shadows the
     stdlib `inspect` module for anything run from this directory, so `concurrent.futures` — which pulls
     in `inspect` via `traceback`/`dataclasses` — dies before `main()` runs:
     "module 'inspect' has no attribute 'signature'". Any Python test added here hits the same wall.
     That file needs a name that is not a stdlib module's.
  2. THEN it fails on the bug it is actually for: a handful of the 64 concurrent runs crash or produce
     no output (measured 6/64 with the import problem stepped around).

Both are written as we want them to be, not as they are: the imports are the ordinary ones, and the
environment is the default one. Neither is worked around here.

RUN: python3 %s
"""

import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, '..', '..', '..'))
ARCHE = os.path.join(REPO_ROOT, 'build', 'arche')
PROJECT = os.path.join(REPO_ROOT, 'tests', 'extras')

# Two drivers in ONE project, both hot: the shape that collides. Each is a whole-program-correct test in
# its own right (they pass when run alone), so any failure here is the interference, not the program.
CASES = [
    ('camera_smoke.arche', 's=260.0 180.0 eyex=150.0'),
    ('demo.arche', 'os=linux sep=/ null=/dev/null'),
]

ROUNDS = 8
CONCURRENCY = 4  # runs of each case per round


def run_one(case):
    """One `arche run`. No ARCHE_HOT_DIR / ARCHE_CACHE_DIR: the default, per-project dirs are the
    thing under test."""
    src, expect = case
    env = dict(os.environ)
    env.pop('ARCHE_HOT_DIR', None)
    env.pop('ARCHE_CACHE_DIR', None)
    try:
        r = subprocess.run([ARCHE, 'run', src], cwd=PROJECT, env=env,
                           capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        return (src, 'TIMEOUT')
    out = r.stdout + r.stderr
    if expect in out:
        return None
    # A crashed host exits 0 with "stack overflow" on stderr — report what actually happened.
    detail = 'crashed (SIGSEGV → "stack overflow", exit 0)' if 'stack overflow' in out \
        else 'no expected output; got: %r' % out.strip()[-200:]
    return (src, detail)


def main():
    if not os.path.exists(ARCHE):
        print('FAIL: compiler not built: %s' % ARCHE)
        return 1

    failures = []
    for rnd in range(ROUNDS):
        work = [c for c in CASES for _ in range(CONCURRENCY)]
        with ThreadPoolExecutor(max_workers=len(work)) as pool:
            for res in pool.map(run_one, work):
                if res:
                    failures.append((rnd, res[0], res[1]))

    total = ROUNDS * CONCURRENCY * len(CASES)
    if failures:
        print('FAIL: %d/%d concurrent `arche run` invocations of one project failed'
              % (len(failures), total))
        for rnd, src, detail in failures[:10]:
            print('  round %d: %s: %s' % (rnd, src, detail))
        print('Concurrent runs share ARCHE_HOT_DIR (<project>/build/.arche-hot), so each host')
        print('re-dlopens device .so files published by the other. See this file\'s docstring.')
        return 1

    print('ok: %d concurrent runs, no interference' % total)
    return 0


if __name__ == '__main__':
    sys.exit(main())

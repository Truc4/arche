#!/usr/bin/env python3
"""
Guards `arche calibrate`'s GPU probe (cli/cmd_calibrate.c). The probe writes a tiny `@gpu` program that
hand-indexes a `[6]T` timer pool (`T.ns[i]`) — which trips W0029 `pool_index_outside_query`, an
error-BY-DEFAULT lint. If the probe doesn't opt out (`--pool-index=allow`), it fails SEMANTIC analysis and
`measure_gpu` returns 0, so EVERY machine reports a phantom `gpu_present=0` (GPU auto-placement silently
dead) — and because the failure is swallowed, `make install` still "succeeds". CI never runs `make install`,
so this regression is otherwise invisible; this test makes it visible.

The check is GPU-INDEPENDENT: W0029 fires at semantic analysis, BEFORE any GPU/glslc is needed. On a runner
with no GPU (or no glslc), calibrate legitimately writes a CPU-only profile (`gpu_present 0`) with NO
semantic/lint errors — that passes. Only a probe that fails to COMPILE (W0029 re-introduced, an in/out-shadow
warning promoted to an error, etc.) prints the failure markers — that fails.

RUN: python3 %s
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

test_dir = os.path.dirname(os.path.abspath(__file__))
repo_root = os.path.abspath(os.path.join(test_dir, '..', '..'))
arche_bin = os.path.join(repo_root, 'build', 'arche')


def main():
    if not os.path.exists(arche_bin):
        print("SKIP: arche binary not built", file=sys.stderr)
        return 0

    cache = tempfile.mkdtemp(prefix='arche_calib_')
    try:
        env = dict(os.environ)
        env['ARCHE_CACHE_DIR'] = cache  # write the profile here, never the machine-global lib dir
        run = subprocess.run([arche_bin, 'calibrate'], capture_output=True, text=True, env=env, timeout=180)
        out = run.stdout + run.stderr

        # The probe must not fail to COMPILE. These markers only appear if the generated `@gpu` probe program
        # is rejected by the front end (W0029 hand-indexing, a promoted warning, a syntax/type regression).
        for marker in ("Semantic analysis failed", "pool_index_outside_query"):
            if marker in out:
                print("FAIL: calibrate GPU probe failed to compile (%r)\n%s" % (marker, out), file=sys.stderr)
                return 1

        # calibrate is non-fatal by design (a box with no device writes a CPU-only profile), so it should
        # always finish and write a profile — proving the probe path actually ran to completion.
        profile = os.path.join(cache, 'machine.profile')
        if run.returncode != 0 or not os.path.exists(profile):
            print("FAIL: calibrate did not complete (rc=%d, profile=%s)\n%s"
                  % (run.returncode, os.path.exists(profile), out), file=sys.stderr)
            return 1
        with open(profile) as f:
            body = f.read()
        if not re.search(r'^gpu_present\s+[01]\s*$', body, re.M):
            print("FAIL: machine.profile missing a gpu_present line\n%s" % body, file=sys.stderr)
            return 1

        print("PASS: calibrate GPU probe compiles + writes a profile (gpu_present detected, not a compile failure)")
        return 0
    finally:
        shutil.rmtree(cache, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())

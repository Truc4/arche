#!/usr/bin/env python3
"""
OFF-GATE live hot-reload smoke. Run by `make test-e2e`, NOT by `make test` (this dir is excluded from
lit). It's the one thing the deterministic C unit test + lit IR tests can't prove: that a REAL `arche run`
process, with its file watcher, rebuilds an edited device and the running host picks it up live.

Robustness (the bar that justifies keeping a process test at all):
  - the arche source is REAL .arche FIXTURE FILES in this dir (formatter-checked; a syntax change fails
    loudly at build, never rots silently like embedded string literals did);
  - completion is detected by POLLING the host's stdout for a token with a hard timeout — no fixed sleeps,
    NO retry-to-mask-flakiness (a failure is a real failure);
  - headless: the device returns a string tag the host prints via fmt.print (raw os.write, unbuffered) —
    no window, no pixels.

Flow: copy the fixtures to a temp dir, run, wait for GEN-ALPHA, copy dev_v2.arche over dev/dev.arche,
wait for GEN-BETA (the live reload), assert the host never restarted.
"""

import os
import shutil
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
ARCHE = os.path.join(REPO, 'build', 'arche')
TIMEOUT = 60  # generous hard ceiling; the test stops as soon as it sees the token


def wait_for(path, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(path) as fh:
                if needle in fh.read():
                    return True
        except FileNotFoundError:
            pass
        time.sleep(0.05)
    return False


def main():
    if not os.path.exists(ARCHE):
        print("SKIP: arche not built", file=sys.stderr)
        return 0

    work = REPO + '/build/.e2e_hot_reload'
    shutil.rmtree(work, ignore_errors=True)
    shutil.copytree(HERE, work)
    out_path = work + '/out.txt'
    out = open(out_path, 'w')
    proc = subprocess.Popen([ARCHE, 'run', 'main.arche'], cwd=work,
                            stdout=out, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        if not wait_for(out_path, 'GEN-ALPHA', TIMEOUT):
            print("FAIL: host never emitted the initial tag", file=sys.stderr)
            return 1
        # Live edit: swap dev.arche for the v2 fixture while the host runs.
        shutil.copyfile(work + '/dev_v2.arche', work + '/dev/dev.arche')
        if not wait_for(out_path, 'GEN-BETA', TIMEOUT):
            print("FAIL: edit did not reload into the running host", file=sys.stderr)
            return 1
        if proc.poll() is not None:
            print("FAIL: host exited instead of reloading in place", file=sys.stderr)
            return 1
        print("PASS: live `arche run` rebuilt + reloaded an edited device (ALPHA -> BETA)")
        return 0
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        except ProcessLookupError:
            pass
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())

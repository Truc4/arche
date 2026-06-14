#!/usr/bin/env python3
"""
Hot-reload of a device SYSTEM (not just a proc) — the ECS path. A system is emitted in its declaring
device's unit and the driver's `run` reaches it through the reload trampoline, so editing a system body
hot-swaps live, exactly like a proc. This is the case that used to silently NOT reload (systems were
compiled into the host).

Shape: the driver owns a 1-row `Cell` pool; the `ticker` device's system writes a value into it each
frame; the driver reads the column and prints a tag (via fmt.print = raw os.write, unbuffered). We start
`arche run`, wait for the original tag, edit the SYSTEM body to write a different value, and assert the
running host emits the new tag — the device .so rebuilt, the host reloaded it, and the DRIVER'S POOL
survived the reload (the system writes the host's storage through the run-site pointer).

RUN: python3 %s
"""

import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

test_dir = os.path.dirname(os.path.abspath(__file__))
repo_root = os.path.join(test_dir, '..', '..')
arche_bin = os.path.join(repo_root, 'build', 'arche')

DS = "v :: int;\n[1]Cell;\n"
SYS_V1 = "Cell :: arche { v }\n\ntick :: sys (v) {\n  v = 105;\n}\n"
SYS_V2 = "Cell :: arche { v }\n\ntick :: sys (v) {\n  v = 305;\n}\n"
MAIN = (
    "#import { ticker fmt os }\n"
    "[1]Cell;\n"
    "main :: proc() {\n"
    "  insert(Cell, 0)(_:, _:);\n"
    "  i := 0;\n"
    "  for (i = 0; i < 3000; i = i + 1) {\n"
    "    run ticker.tick;\n"
    "    if (Cell.v[0] == 305) { fmt.print(\"GEN-BETA\\n\"); } else { fmt.print(\"GEN-ALPHA\\n\"); }\n"
    "    os.sleep_ms(20);\n"
    "  }\n"
    "}\n"
)


def wait_for(path, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(path, 'r') as fh:
                if needle in fh.read():
                    return True
        except FileNotFoundError:
            pass
        time.sleep(0.1)
    return False


def attempt():
    """Run the scenario once; return (ok, detail). Timing-sensitive, so main() retries."""
    work = tempfile.mkdtemp(prefix='arche_hotsys_')
    proc = None
    try:
        os.makedirs(os.path.join(work, 'ticker'))
        with open(os.path.join(work, 'ticker', 'ticker.ds.arche'), 'w') as f:
            f.write(DS)
        sys_path = os.path.join(work, 'ticker', 'ticker.arche')
        with open(sys_path, 'w') as f:
            f.write(SYS_V1)
        with open(os.path.join(work, 'main.arche'), 'w') as f:
            f.write(MAIN)

        out_path = os.path.join(work, 'out.txt')
        err_path = os.path.join(work, 'err.txt')
        out = open(out_path, 'w')
        err = open(err_path, 'w')
        proc = subprocess.Popen([arche_bin, 'run', 'main.arche'],
                                cwd=work, stdout=out, stderr=err, start_new_session=True)

        def fail(msg):
            try:
                with open(err_path) as fh:
                    tail = ''.join(fh.readlines()[-25:])
                if tail.strip():
                    msg += "\n--- host/watcher stderr (tail) ---\n" + tail
            except OSError:
                pass
            return (False, msg)

        if not wait_for(out_path, 'GEN-ALPHA', timeout=60):
            return fail("host never emitted the initial tag (GEN-ALPHA) — system v1 not running")

        with open(sys_path, 'w') as f:
            f.write(SYS_V2)

        if not wait_for(out_path, 'GEN-BETA', timeout=60):
            return fail("editing the system body did not reload (no GEN-BETA)")

        if proc.poll() is not None:
            return (False, "host exited instead of reloading in place")

        return (True, "live edit of a device SYSTEM reloaded into the running host (pool survived)")
    finally:
        if proc is not None and proc.poll() is None:
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                except ProcessLookupError:
                    pass
        shutil.rmtree(work, ignore_errors=True)


def main():
    if not os.path.exists(arche_bin):
        print("SKIP: arche binary not built", file=sys.stderr)
        return 0
    last = ""
    for i in range(2):  # one retry: live-timing test, not a logic test
        ok, detail = attempt()
        if ok:
            print("PASS: " + detail)
            return 0
        last = detail
        print("attempt %d failed: %s" % (i + 1, detail.splitlines()[0]), file=sys.stderr)
    print("FAIL: " + last, file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())

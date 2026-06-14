#!/usr/bin/env python3
"""
Integration test for the DEV HOT-RELOAD loop (`arche run`) — the one piece the lit unit tests can't
cover, because it needs a long-running host plus a source edit WHILE it runs. No display required: the
"draw" is a string the device returns and the host prints via `fmt.print` (raw `os.write`, unbuffered),
so the edit's effect is observable on stdout as it happens.

Shape: a driver loops calling `dev.tag()` and printing it. We start `arche run`, wait until the first
tag ("GEN-ALPHA") streams out, rewrite the device to return "GEN-BETA", and assert the running host
starts emitting the NEW tag — without restarting. That proves: device built as a reloadable `.so`, the
watcher rebuilt it on edit, and the live host picked it up. (`arche build` would compile the indirection
out; that purity is asserted by tests/unit/compiler/per_unit/release_no_hot.arche.)

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

DEV_DS = "// reload-test device (behavior only; the driver owns all state)\n"
DEV_ALPHA = 'tag :: func() -> []char {\n  return "GEN-ALPHA\\n";\n}\n'
DEV_BETA = 'tag :: func() -> []char {\n  return "GEN-BETA\\n";\n}\n'
# A long loop so the host stays alive across the rebuild; the test kills it the moment it sees the new
# tag, so this ceiling (~60s) is only hit if reload never happens (→ the test fails on timeout instead).
MAIN = (
    "#import { dev fmt os }\n"
    "main :: proc() {\n"
    "  i := 0;\n"
    "  for (i = 0; i < 3000; i = i + 1) {\n"
    "    t := dev.tag();\n"
    "    fmt.print(t);\n"
    "    os.sleep_ms(20);\n"
    "  }\n"
    "}\n"
)


def wait_for(path, needle, timeout):
    """Poll `path` until it contains `needle`; return True on success, False on timeout."""
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
    work = tempfile.mkdtemp(prefix='arche_hotreload_')
    proc = None
    try:
        os.makedirs(os.path.join(work, 'dev'))
        with open(os.path.join(work, 'dev', 'dev.ds.arche'), 'w') as f:
            f.write(DEV_DS)
        dev_path = os.path.join(work, 'dev', 'dev.arche')
        with open(dev_path, 'w') as f:
            f.write(DEV_ALPHA)
        with open(os.path.join(work, 'main.arche'), 'w') as f:
            f.write(MAIN)

        out_path = os.path.join(work, 'out.txt')
        err_path = os.path.join(work, 'err.txt')
        out = open(out_path, 'w')
        err = open(err_path, 'w')
        env = dict(os.environ)
        env['ARCHE_HOT_DEBUG'] = '1'  # makes the watcher log its mtime polls (dumped on failure below)
        # start_new_session so the whole `arche run` group (the watcher parent + the host child it forks)
        # can be torn down together at the end — killing only the parent would orphan the live host.
        proc = subprocess.Popen([arche_bin, 'run', 'main.arche'],
                                cwd=work, stdout=out, stderr=err,
                                start_new_session=True, env=env)

        def fail(msg):
            try:
                with open(err_path) as fh:
                    tail = ''.join(fh.readlines()[-25:])
                if tail.strip():
                    msg += "\n--- host/watcher stderr (tail) ---\n" + tail
            except OSError:
                pass
            return (False, msg)

        # 1) Host comes up and streams the original tag (includes the initial compile + device .so links).
        if not wait_for(out_path, 'GEN-ALPHA', timeout=60):
            return fail("host never emitted the initial tag (GEN-ALPHA)")

        # 2) Edit the device WHILE the host runs. The watcher should rebuild dev's .so; the host reloads it.
        with open(dev_path, 'w') as f:
            f.write(DEV_BETA)

        # 3) The live host must start emitting the NEW tag — the actual reload assertion.
        if not wait_for(out_path, 'GEN-BETA', timeout=60):
            return fail("edit did not take effect in the running host (no GEN-BETA)")

        # Sanity: the host was the SAME process throughout (never restarted).
        if proc.poll() is not None:
            return (False, "host exited instead of reloading in place")

        return (True, "live device edit reloaded into the running host (ALPHA -> BETA)")
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

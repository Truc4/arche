#!/usr/bin/env python3
"""
Hot-reload across a DEVICE→DEVICE boundary, pixel-verified, headless — the realistic arche-rpg shape.
A driver opens a gfx window (an `opaque` whose C framebuffer state lives in the HOST shim) and loops
calling a `paint` device that draws through gfx. We edit `paint` (the circle color) while it runs and
assert the dumped framebuffer's center pixel changes — proving three things at once:

  1. the edited device (`paint`) rebuilt to its `.so` and reloaded into the live host;
  2. `gfx` was NOT reloaded (unchanged IR → content-hash gate skips its `.so`), so the window `opaque`
     + the C-side framebuffer it points at SURVIVE the reload — the host keeps drawing, never re-opens;
  3. the device→device call (paint→gfx) keeps resolving after paint is swapped.

Headless gfx backend (ARCHE_SELECT=gfx=headless) dumps the framebuffer to a PPM each present (atomic
rename, so reads aren't torn). No display required.

RUN: python3 %s
"""

import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

repo_root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
arche_bin = os.path.join(repo_root, 'build', 'arche')

CX, CY = 32, 32
RED, GREEN = (0xFF, 0x00, 0x00), (0x00, 0xFF, 0x00)
PAINT = ("#import { gfx }\n"
         "draw :: proc(win: window) {\n"
         "  gfx.clear(win, 16);\n"
         "  gfx.circle(win, 32, 32, 10, %d);\n"
         "}\n")
MAIN = ("#import { gfx paint os }\n"
        "main :: proc() {\n"
        "  gfx.open(64, 64, \"t\")(win:);\n"
        "  is_open := 1;\n"
        "  for (; is_open != 0;) {\n"
        "    paint.draw(win);\n"
        "    gfx.present(win);\n"
        "    gfx.poll(win)(is_open);\n"
        "    os.sleep_ms(20);\n"
        "  }\n"
        "}\n")
MANIFEST = ("[lib]\npaths = [\"%s\", \"%s\"]\n[select]\ngfx = \"headless\"\n"
            % (os.path.join(repo_root, 'extras'), os.path.join(repo_root, 'stdlib')))


def center(path):
    """(r,g,b) center pixel of the PPM, or None if not readable whole yet."""
    try:
        d = open(path, 'rb').read()
    except OSError:
        return None
    if d[:2] != b'P6':
        return None
    idx, toks = 2, []
    while len(toks) < 3:
        while idx < len(d) and d[idx] in b' \t\n\r':
            idx += 1
        s = idx
        while idx < len(d) and d[idx] not in b' \t\n\r':
            idx += 1
        toks.append(d[s:idx])
    idx += 1
    w, h = int(toks[0]), int(toks[1])
    o = idx + (CY * w + CX) * 3
    if o + 3 > len(d):
        return None
    return (d[o], d[o + 1], d[o + 2])


def wait(path, want, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if center(path) == want:
            return True
        time.sleep(0.1)
    return False


def attempt():
    """Run the scenario once; return (ok, detail). Timing-sensitive (live build + reload under possibly
    heavy parallel CI load), so main() retries — a real regression fails every attempt, a blip doesn't."""
    work = tempfile.mkdtemp(prefix='arche_hotgfx_')
    os.makedirs(work + '/paint')
    open(work + '/arche.toml', 'w').write(MANIFEST)
    open(work + '/paint/paint.ds.arche', 'w').write("// paint device (draws through gfx; owns no state)\n")
    paint_path = work + '/paint/paint.arche'
    open(paint_path, 'w').write(PAINT % 0xFF0000)
    open(work + '/main.arche', 'w').write(MAIN)

    ppm = work + '/fb.ppm'
    env = dict(os.environ)
    env['ARCHE_SELECT'] = 'gfx=headless'
    env['GFX_HEADLESS_DUMP'] = ppm
    env['GFX_HEADLESS_FRAMES'] = '100000'  # loop "forever"; killed once we see green
    p = subprocess.Popen([arche_bin, 'run', 'main.arche'], cwd=work,
                         stdout=open(work + '/o', 'w'), stderr=open(work + '/e', 'w'),
                         start_new_session=True, env=env)
    try:
        if not wait(ppm, RED, 60):
            return False, "initial frame never showed the red circle (gfx didn't come up); stderr:\n" + open(
                work + '/e').read()[-400:]
        open(paint_path, 'w').write(PAINT % 0x00FF00)  # edit the draw device live
        if not wait(ppm, GREEN, 60):
            return False, "editing paint did not reload (center never turned green)"
        if p.poll() is not None:
            return False, "host exited — window opaque / gfx state did not survive the reload"
        return True, "device->device hot reload, gfx window survived, pixels updated (red -> green)"
    finally:
        try:
            os.killpg(os.getpgid(p.pid), signal.SIGTERM)
        except ProcessLookupError:
            pass
        shutil.rmtree(work, ignore_errors=True)


def main():
    if not os.path.exists(arche_bin):
        print("SKIP: arche binary not built", file=sys.stderr)
        return 0
    last = ""
    for i in range(2):  # one retry: this is a live-timing test, not a logic test
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

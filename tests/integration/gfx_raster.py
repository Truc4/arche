#!/usr/bin/env python3
"""
Pixel-level test for the gfx raster ops (gfx.clear / rect / circle) — HEADLESS, no display. Uses the
`headless` gfx backend (ARCHE_SELECT=gfx=headless), an in-memory framebuffer whose `present` dumps a PPM
(GFX_HEADLESS_DUMP). We draw a known scene at known coordinates and assert the actual pixels, so the
raster math (disc fill, rect bounds, 0xRRGGBB packing) is covered in CI instead of only by eyeballing a
window. The program imports gfx from the repo's `extras` via an arche.toml `[lib] paths` (also exercising
the lib-search path + the variant selector end to end).

RUN: python3 %s
"""

import os
import shutil
import subprocess
import sys
import tempfile

test_dir = os.path.dirname(os.path.abspath(__file__))
repo_root = os.path.abspath(os.path.join(test_dir, '..', '..'))
arche_bin = os.path.join(repo_root, 'build', 'arche')

W, H = 64, 64
CLEAR = 0x000010
RECT = 0x00FF00
CIRC = 0xFF0000

PROG = (
    # New model: gfx is query-driven. The driver owns the pools (a [1] Window + shape pools) and SCHEDULES
    # the gfx draw systems by name; each draw op nests a fan over the window and a fan over its shapes (no
    # procs, no joins). `boot` opens the window (running the open Eff) and seeds one disc + one rect.
    "#import { gfx }\n"
    "Window :: arche { handle :: window  bg :: int }\n"
    "Disc :: arche { pos(x, y) :: int  color :: int  r :: int }\n"
    "Rect :: arche { rx :: int  ry :: int  rw :: int  rh :: int  rcolor :: int }\n"
    "[1]Window ?abort;\n"
    "[1]Disc ?abort;\n"
    "[1]Rect ?abort;\n"
    "boot :: system {\n"
    "  gfx.open(%d, %d, \"t\")(win:);\n"
    "  insert(Window { handle: win, bg: %d });\n"
    "  insert(Disc { pos: (32, 32), color: %d, r: 10 });\n"          # disc center (32,32) r=10
    "  insert(Rect { rx: 2, ry: 2, rw: 8, rh: 8, rcolor: %d });\n"   # covers x,y in [2,10)
    "}\n"
    "#run seq({ boot, gfx.clear, gfx.rect, gfx.circle, gfx.present })\n"
) % (W, H, CLEAR, CIRC, RECT)

MANIFEST = (
    "[lib]\n"
    "paths = [\"%s\", \"%s\"]\n"
    "[select]\n"
    "gfx = \"headless\"\n"
) % (os.path.join(repo_root, 'extras'), os.path.join(repo_root, 'stdlib'))


def read_ppm(path):
    """Minimal binary-PPM (P6) reader → (w, h, bytes). Header is 'P6\\n<w> <h>\\n255\\n'."""
    with open(path, 'rb') as f:
        data = f.read()
    assert data[:2] == b'P6', "not a P6 PPM"
    # Walk past the three whitespace-separated header tokens after the magic, then one whitespace byte.
    idx, tokens = 2, []
    while len(tokens) < 3:
        while idx < len(data) and data[idx] in b' \t\n\r':
            idx += 1
        start = idx
        while idx < len(data) and data[idx] not in b' \t\n\r':
            idx += 1
        tokens.append(data[start:idx])
    idx += 1  # the single whitespace after maxval
    w, h, _maxval = int(tokens[0]), int(tokens[1]), int(tokens[2])
    return w, h, data[idx:]


def main():
    if not os.path.exists(arche_bin):
        print("SKIP: arche binary not built", file=sys.stderr)
        return 0

    work = tempfile.mkdtemp(prefix='arche_gfxraster_')
    try:
        with open(os.path.join(work, 'arche.toml'), 'w') as f:
            f.write(MANIFEST)
        with open(os.path.join(work, 'draw.arche'), 'w') as f:
            f.write(PROG)

        exe = os.path.join(work, 'draw')
        env = dict(os.environ)
        env['ARCHE_SELECT'] = 'gfx=headless'
        build = subprocess.run([arche_bin, 'build', '-o', exe, 'draw.arche'],
                               cwd=work, capture_output=True, text=True, env=env)
        if build.returncode != 0:
            print("FAIL: headless build failed\n" + build.stdout + build.stderr, file=sys.stderr)
            return 1

        ppm = os.path.join(work, 'out.ppm')
        env['GFX_HEADLESS_DUMP'] = ppm
        env['GFX_HEADLESS_FRAMES'] = '1'
        run = subprocess.run([exe], cwd=work, capture_output=True, text=True, env=env)
        if run.returncode != 0:
            print("FAIL: headless run crashed\n" + run.stderr, file=sys.stderr)
            return 1
        if not os.path.exists(ppm):
            print("FAIL: no framebuffer dump produced", file=sys.stderr)
            return 1

        w, h, px = read_ppm(ppm)
        if (w, h) != (W, H):
            print("FAIL: dump is %dx%d, expected %dx%d" % (w, h, W, H), file=sys.stderr)
            return 1

        def pixel(x, y):
            o = (y * w + x) * 3
            return (px[o] << 16) | (px[o + 1] << 8) | px[o + 2]

        checks = [
            ("circle center (32,32)", pixel(32, 32), CIRC),
            ("rect interior (4,4)", pixel(4, 4), RECT),
            ("clear background corner (0,63)", pixel(0, 63), CLEAR),
            ("clear background far edge (63,0)", pixel(63, 0), CLEAR),
        ]
        ok = True
        for name, got, want in checks:
            if got != want:
                print("FAIL: %s = 0x%06X, expected 0x%06X" % (name, got, want), file=sys.stderr)
                ok = False
        if not ok:
            return 1

        print("PASS: gfx raster pixels correct (clear/rect/circle, headless)")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())

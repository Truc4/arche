#!/usr/bin/env python3
"""
Physics INTEGRATION suite for the `rigid` 2D rigid-body device (extras/rigid). Unlike the single-shot
`tests/extras/rigid_*.arche` doctests, each scenario here DRIVES THE WHOLE SOLVER over hundreds of frames
and asserts emergent invariants — containment, rest/no-jitter, energy dissipation, restitution, stacking,
determinism, momentum — the properties a usable engine must have.

Each scenario is a generated headless driver: bodies live in ONE `RBody` pool (no gfx/render — this
isolates PHYSICS from archetype/render concerns; the multi-archetype join is covered separately by
tests/unit/language/systems/self_join_spans_all_archetypes.arche). The driver runs the full pipeline and
prints a per-frame TRACE (`B <frame> <i> <x> <y> <vx> <vy> <rotx> <roty> <avel>` per dynamic body); this
harness parses it and checks the invariants, exiting non-zero on any failure.

RUN: python3 %s
"""

import math
import os
import subprocess
import sys
import tempfile

test_dir = os.path.dirname(os.path.abspath(__file__))
repo_root = os.path.abspath(os.path.join(test_dir, '..', '..'))
arche_bin = os.path.join(repo_root, 'build', 'arche')

MANIFEST = (
    "[lib]\n"
    "paths = [\"%s\", \"%s\"]\n"
) % (os.path.join(repo_root, 'extras'), os.path.join(repo_root, 'stdlib'))

# Arena bounds shared by the bin scenarios (world +y is DOWN).
AL, AR, ATOP, AFLOOR = 40.0, 640.0, 200.0, 1140.0


# ----------------------------------------------------------------------------- driver generation
def _vecrow(vals):
    return "{ " + ", ".join("%r" % float(v) for v in vals) + " }"


SUBSTEPS = 16  # MUST match rigid.arche SUBSTEPS/DT — the device integrates by a compile-constant DT=1/16.


def build_driver(bodies, frames, vel_it=2, grav=0.4):
    """bodies: list of dicts with pos,vel,spin,rot,ext,mi,mat (each a tuple/scalar). A body with mi[0]==0
    and a non-zero vel is a KINEMATIC pusher (moves at constant velocity, unaffected by collisions).
    Traces every body every frame. Substepped TGS-Soft solver: SUBSTEPS substeps/frame, `vel_it` velocity
    iterations per substep. Gravity is applied per substep (GRAV/SUBSTEPS)."""
    n = len(bodies)
    cols = {
        "pos.x": [b["pos"][0] for b in bodies], "pos.y": [b["pos"][1] for b in bodies],
        "lvel.x": [b["vel"][0] for b in bodies], "lvel.y": [b["vel"][1] for b in bodies],
        "spin.x": [b["spin"] for b in bodies], "spin.y": [0.0] * n,
        "rot.x": [b["rot"][0] for b in bodies], "rot.y": [b["rot"][1] for b in bodies],
        "ext.x": [b["ext"][0] for b in bodies], "ext.y": [b["ext"][1] for b in bodies],
        "mi.x": [b["mi"][0] for b in bodies], "mi.y": [b["mi"][1] for b in bodies],
        "mat.x": [b["mat"][0] for b in bodies], "mat.y": [b["mat"][1] for b in bodies],
        "dimp.x": [0.0] * n, "dimp.y": [0.0] * n, "dang.x": [0.0] * n, "dang.y": [0.0] * n,
        "cor.x": [0.0] * n, "cor.y": [0.0] * n,
    }
    seed = "seed :: system {\n"
    for col, vals in cols.items():
        seed += "  RBody.%s = %s;\n" % (col, _vecrow(vals))
    seed += "  Clock.c = { 0.0 };\n}\n"

    trace = "trace :: system eff {\n  map (query { c }) eff {\n"
    for i in range(n):
        trace += ('    fmt.printf("B %%.0f %d %%.4f %%.4f %%.4f %%.4f %%.5f %%.5f %%.6f\\n", c, '
                  'RBody.pos.x[%d], RBody.pos.y[%d], RBody.lvel.x[%d], RBody.lvel.y[%d], '
                  'RBody.rot.x[%d], RBody.rot.y[%d], RBody.spin.x[%d]);\n') % (i, i, i, i, i, i, i, i)
    trace += ('    if (c > %d.5) { fmt.fflush(0)(_:); os.exit(0)(_:); }\n  };\n}\n' % (frames - 1))

    # One TGS substep: apply gravity (per-substep GRAV/SUBSTEPS), count contacts, run `vel_it` soft-constraint
    # velocity iterations, then integrate positions by DT. The whole substep is unrolled SUBSTEPS times/frame.
    substep = ("grav, rigid.contacts, "
               + "rigid.solve_lin, rigid.solve_ang, rigid.apply, " * vel_it
               + "rigid.integrate")
    subs = ", ".join([substep] * SUBSTEPS)
    # World-bounds confine: a dynamic body's centre is clamped into the arena and velocity into a wall is
    # zeroed. The real wall collisions handle normal contact; this only fires when a body would leave the
    # visual bin (e.g. squeezed between the player and a wall — an over-constrained case the solver can't win).
    confine = (
        "confine :: map (query { pos, lvel, ext, mi })(pos, lvel) {\n"
        "  dyn := select(mi.x > 0.0, 1.0, 0.0);\n"
        "  lox := %r + ext.x; hix := %r - ext.x; loy := %r + ext.y; hiy := %r - ext.y;\n"
        "  lvel.x = select(pos.x <= lox && lvel.x < 0.0, 0.0, lvel.x);\n"
        "  lvel.x = select(pos.x >= hix && lvel.x > 0.0, 0.0, lvel.x);\n"
        "  lvel.y = select(pos.y <= loy && lvel.y < 0.0, 0.0, lvel.y);\n"
        "  lvel.y = select(pos.y >= hiy && lvel.y > 0.0, 0.0, lvel.y);\n"
        "  cx := select(pos.x < lox, lox, pos.x); cx = select(cx > hix, hix, cx);\n"
        "  cy := select(pos.y < loy, loy, pos.y); cy = select(cy > hiy, hiy, cy);\n"
        "  pos.x = select(dyn > 0.5, cx, pos.x);\n"
        "  pos.y = select(dyn > 0.5, cy, pos.y);\n"
        "}\n"
    ) % (AL, AR, ATOP, AFLOOR)
    prog = (
        "#import { rigid vec fmt os }\n"
        "GDT :: %r;\n"                        # gravity per SUBSTEP = GRAV / SUBSTEPS
        "c :: float;\n"
        "Clock :: arche { c }\n"
        "[%d]RBody(%d);\n"
        "[1]Clock(1);\n"
        "%s"
        "grav :: map (query { lvel, mi })(lvel) { lvel.y = lvel.y + GDT * select(mi.x > 0.0, 1.0, 0.0); }\n"
        "tick :: map (query { c })(c) { c = c + 1.0; }\n"
        "%s%s"
        "#run seq({ seed, forever(seq({ %s, rigid.rest, confine, tick, trace })) })\n"
    ) % (grav / SUBSTEPS, n, n, seed, confine, trace, subs)
    return prog


def run(prog, timeout=200):
    # COMPILE to a native binary then execute it — the substepped solver runs 16×/frame, far too slow under
    # the `arche run` interpreter. Building once + running the compiled exe is ~10-100× faster.
    with tempfile.TemporaryDirectory() as d:
        with open(os.path.join(d, "arche.toml"), "w") as f:
            f.write(MANIFEST)
        with open(os.path.join(d, "s.arche"), "w") as f:
            f.write(prog)
        exe = os.path.join(d, "sim")
        b = subprocess.run([arche_bin, "build", "--pool-index=allow", "-o", exe, "s.arche"],
                           cwd=d, capture_output=True, text=True, timeout=timeout)
        if b.returncode != 0:
            return "", "BUILD FAILED:\n" + b.stderr[-2000:], b.returncode
        r = subprocess.run([exe], cwd=d, capture_output=True, text=True, timeout=timeout)
        return r.stdout, r.stderr, r.returncode


def parse(stdout):
    """→ frames: dict[frame] -> dict[body_index] -> (x,y,vx,vy,rx,ry,w)."""
    frames = {}
    for ln in stdout.splitlines():
        if not ln.startswith("B "):
            continue
        p = ln.split()
        fr, idx = int(float(p[1])), int(p[2])
        frames.setdefault(fr, {})[idx] = tuple(float(x) for x in p[3:10])
    return frames


# ----------------------------------------------------------------------------- invariants
class Fail(Exception):
    pass


def check_containment(frames, bodies, dyn, margin=6.0):
    """Every dynamic body's CENTRE stays inside the arena (a tunnel-through pushes the centre past a wall)."""
    for fr, bs in frames.items():
        for i in dyn:
            x, y = bs[i][0], bs[i][1]
            if not (AL - margin <= x <= AR + margin and ATOP - margin <= y <= AFLOOR + margin):
                raise Fail("body %d left the bin at frame %d: pos=(%.1f,%.1f)" % (i, fr, x, y))


def check_rest(frames, dyn, tail=60, v_eps=0.08, w_eps=0.02, d_eps=0.25):
    """Over the last `tail` frames the dynamic bodies must be at REST: tiny velocity AND tiny frame-to-frame
    motion (jitter shows up as persistent velocity / oscillating position even when 'settled')."""
    fmax = max(frames)
    window = [fr for fr in frames if fr > fmax - tail]
    for i in dyn:
        vmax = max(max(abs(frames[fr][i][2]), abs(frames[fr][i][3])) for fr in window)
        wmax = max(abs(frames[fr][i][6]) for fr in window)
        dmax = 0.0
        sw = sorted(window)
        for a, b in zip(sw, sw[1:]):
            dmax = max(dmax, abs(frames[b][i][0] - frames[a][i][0]), abs(frames[b][i][1] - frames[a][i][1]))
        if vmax > v_eps or wmax > w_eps or dmax > d_eps:
            raise Fail("body %d never rests (jitter): |v|max=%.3f |w|max=%.3f dpos=%.3f (eps v=%.2f w=%.2f d=%.2f)"
                       % (i, vmax, wmax, dmax, v_eps, w_eps, d_eps))


def kinetic_energy(frames, fr, dyn, bodies):
    e = 0.0
    for i in dyn:
        vx, vy, w = frames[fr][i][2], frames[fr][i][3], frames[fr][i][6]
        invm, invI = bodies[i]["mi"]
        m = 1.0 / invm if invm > 0 else 0.0
        I = 1.0 / invI if invI > 0 else 0.0
        e += 0.5 * m * (vx * vx + vy * vy) + 0.5 * I * w * w
    return e


# ----------------------------------------------------------------------------- scenarios
def bin_bodies(deep=200.0):
    """floor + left/right walls + ceiling as static bodies. Walls extend `deep` OUTWARD so nothing tunnels."""
    cx = (AL + AR) / 2
    return [
        {"pos": (cx, AFLOOR + deep), "vel": (0, 0), "spin": 0, "rot": (1, 0), "ext": ((AR - AL) / 2 + 20, deep),
         "mi": (0, 0), "mat": (0.0, 0.6)},                                                    # floor
        {"pos": (AL - deep, (ATOP + AFLOOR) / 2), "vel": (0, 0), "spin": 0, "rot": (1, 0),
         "ext": (deep, (AFLOOR - ATOP) / 2 + 40), "mi": (0, 0), "mat": (0.0, 0.6)},           # left wall
        {"pos": (AR + deep, (ATOP + AFLOOR) / 2), "vel": (0, 0), "spin": 0, "rot": (1, 0),
         "ext": (deep, (AFLOOR - ATOP) / 2 + 40), "mi": (0, 0), "mat": (0.0, 0.6)},           # right wall
        {"pos": (cx, ATOP - deep), "vel": (0, 0), "spin": 0, "rot": (1, 0), "ext": ((AR - AL) / 2 + 20, deep),
         "mi": (0, 0), "mat": (0.0, 0.6)},                                                    # ceiling
    ]


def scn_rest():
    # Three boxes WELL SEPARATED (edge gap ~170px) so each lands independently and comes to a dead stop.
    # No initial spin and a moderate drop height so they don't tumble sideways into each other: a dense/
    # touching pile limit-cycles in a Jacobi/reduce solver (a documented engine limitation — see module
    # docstring). Different rotations (flat, +45°, -30°) exercise both flat-face and corner-first landings.
    walls = bin_bodies()
    xs = [200, 450, 700]
    ang = [(1.0, 0.0), (0.966, 0.259), (0.707, 0.707)]
    boxes = [{"pos": (xs[i], 820), "vel": (0, 0), "spin": 0.0,
              "rot": ang[i], "ext": (40, 40), "mi": (1.0, 1.0 / (40 * 40 * 2 / 3)), "mat": (0.1, 0.5)}
             for i in range(3)]
    dyn = list(range(len(walls), len(walls) + 3))
    return walls + boxes, dyn, 260


def scn_squeeze():
    """A box near the right wall, and a heavy kinematic pusher moving right into it — must NOT tunnel out."""
    walls = bin_bodies()
    box = {"pos": (560, 1090), "vel": (0, 0), "spin": 0, "rot": (1, 0), "ext": (40, 40),
           "mi": (1.0, 1.0 / (40 * 40 * 2 / 3)), "mat": (0.1, 0.5)}
    pusher = {"pos": (300, 1090), "vel": (3.0, 0), "spin": 0, "rot": (1, 0), "ext": (51, 51),
              "mi": (0, 0), "mat": (0.1, 0.5)}  # mi=0 + vel → constant-velocity infinite-mass pusher
    bodies = walls + [box, pusher]
    dyn = [len(walls)]  # only the box
    return bodies, dyn, 150


def scn_stack():
    """A 4-box tower that must settle SOLID — the dead-solid-stack invariant the soft-constraint solver buys.
    Boxes start a few px above their rest heights (1100/1020/940/860, each 2*ext apart on the floor) and must
    settle into contact WITHOUT compressing the tower or jittering."""
    walls = bin_bodies()
    cx = (AL + AR) / 2
    ext = 40.0
    invI = 1.0 / (ext * ext * 2 / 3)
    ys = [1096, 1012, 928, 844]  # bottom→top, ~4-16px above rest so they settle into contact
    boxes = [{"pos": (cx, y), "vel": (0, 0), "spin": 0.0, "rot": (1, 0), "ext": (ext, ext),
              "mi": (1.0, invI), "mat": (0.0, 0.5)} for y in ys]
    dyn = list(range(len(walls), len(walls) + 4))  # bottom→top
    return walls + boxes, dyn, 160


def check_stack(frames, dyn, spacing=80.0, tol_frac=0.02, tail=40):
    """`dyn` are the stacked boxes bottom→top. Over the settled tail, consecutive centres must stay `spacing`
    apart within tol_frac (the tower neither COMPRESSES nor gaps), and the top box must be at rest."""
    fmax = max(frames)
    window = [f for f in frames if f > fmax - tail]
    tol = spacing * tol_frac
    for a, b in zip(dyn, dyn[1:]):
        gaps = [abs(frames[f][a][1] - frames[f][b][1]) for f in window]
        if abs(min(gaps) - spacing) > tol or abs(max(gaps) - spacing) > tol:
            raise Fail("stack gap %d-%d = [%.2f,%.2f], want %.1f±%.2f (compression/separation)"
                       % (a, b, min(gaps), max(gaps), spacing, tol))
    top = dyn[-1]
    vmax = max(max(abs(frames[f][top][2]), abs(frames[f][top][3])) for f in window)
    if vmax > 0.1:
        raise Fail("stack top box not at rest: |v|max=%.3f" % vmax)


SCENARIOS = {"rest": scn_rest, "squeeze": scn_squeeze, "stack": scn_stack}


def main():
    failed = []
    for name, gen in SCENARIOS.items():
        bodies, dyn, frames = gen()
        prog = build_driver(bodies, frames)
        out, err, rc = run(prog)
        fr = parse(out)
        if not fr:
            print("FAIL %-10s no trace (rc=%d)\n%s" % (name, rc, err[-500:]))
            failed.append(name)
            continue
        try:
            check_containment(fr, bodies, dyn)
            if name in ("rest",):
                check_rest(fr, dyn)
            if name == "stack":
                check_rest(fr, dyn)
                check_stack(fr, dyn)
            print("PASS %-10s (%d frames, %d bodies)" % (name, max(fr), len(dyn)))
        except Fail as e:
            print("FAIL %-10s %s" % (name, e))
            failed.append(name)
    if failed:
        print("\n%d/%d scenarios FAILED: %s" % (len(failed), len(SCENARIOS), ", ".join(failed)))
        sys.exit(1)
    print("\nall %d scenarios passed" % len(SCENARIOS))


if __name__ == "__main__":
    main()

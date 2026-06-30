#!/usr/bin/env python3
"""Generate the derived-placement benchmark sources (Slice 4).

Emits two committed .arche benches next to this script:
  intensity_sweep.arche — CPU vs forced-GPU across arithmetic intensities (the decision-correctness table).
  resident_loop.arche   — a @resident map run K times: the regime where the GPU actually wins.

Usage:  python3 gen.py     (writes both .arche files; rerun to regenerate)
Run a bench:  arche build --gpu -o /tmp/b <bench>.arche && ARCHE_GPU_DEBUG=1 /tmp/b
"""
import os
HERE = os.path.dirname(os.path.abspath(__file__))


def horner(var, levels):
    # one balanced nested expression = 2*levels flops, ONE write (deep nesting breaks glslc past ~32 levels,
    # and many sequential writes to one column also break it — so a single deep expression is the safe form).
    return "(" * levels + var + ")*1.0001+0.0001" * levels


def intensity_sweep(N=262144, K=20, levels=(0, 8, 16, 32)):
    o = ["// Decision-correctness sweep: for each arithmetic intensity, a CPU arm (plain map) and a forced-GPU",
         "// arm (@gpu) over identical static pools, each timed over K dispatches. Prints",
         "// 'ROW <flops/elem> <cpu_ms> <gpu_ms>'. On a box where the GPU's throughput beats its per-dispatch",
         "// overhead some rows flip to GPU; on an overhead-bound box the CPU wins every row (the honest result).",
         "#import { fmt os }", "c :: float;", "P :: arche { c }", f"[{N}]P({N});",
         "ms :: i64;", "T :: arche { ms }", f"[{4*len(levels)}]T({4*len(levels)});"]
    for i, lv in enumerate(levels):
        body = "c = c + 1.0;" if lv == 0 else f"c = {horner('c', lv)};"
        o.append(f"cpu{i} :: map (query {{ c }}) (c) {{ {body} }}")
        o.append("@gpu")
        o.append(f"gpu{i} :: map (query {{ c }}) (c) {{ {body} }}")
    o.append("seed :: system { P.c = { 1.0 }; }")
    narms = 2 * len(levels)
    for a in range(narms):
        o.append(f"s{a} :: system eff {{ os.now_ms()(now:); T.ms[{2*a}] = now; }}")
        o.append(f"e{a} :: system eff {{ os.now_ms()(now:); T.ms[{2*a+1}] = now; }}")
    parts, args = [], []
    for i in range(len(levels)):
        parts.append(f"ROW {2*levels[i]} %d %d")
        args += [f"T.ms[{2*(2*i)+1}] - T.ms[{2*(2*i)}]", f"T.ms[{2*(2*i+1)+1}] - T.ms[{2*(2*i+1)}]"]
    o.append('report :: system eff {\n  fmt.printf("' + "\\n".join(parts) + '\\n", ' + ", ".join(args) + ");\n}")
    sched = ["seed"]
    for i in range(len(levels)):
        for a, nm in [(2*i, f"cpu{i}"), (2*i+1, f"gpu{i}")]:
            sched += [f"s{a}"] + [nm]*K + [f"e{a}"]
    sched.append("report")
    o.append("#run seq({ " + ", ".join(sched) + " })")
    return "\n".join(o) + "\n"


def resident_loop(N=16000000, K=40, lv=32):
    # The GPU-wins regime: a @resident pool (uploaded once, reused in VRAM) + the same map run K times, so the
    # one-time transfer amortizes and the GPU's per-flop throughput advantage accumulates. CPU arm = the same
    # body over a non-resident pool. Prints 'N=.. K=.. fpe=.. GPU_ms=.. CPU_ms=..'.
    o = ["// The regime where the derived placer SHOULD pick GPU (and Slice 4's per-dispatch model does not yet):",
         "// @resident data + many dispatches amortize the one-time transfer; the GPU's faster compute then wins.",
         "// Measured on a GTX 1650 (this box): GPU ~661 ms vs CPU ~1921 ms (~2.9x). Capturing this in the cost",
         "// model is Slice 5 (residency + amortized-transfer term).",
         "#import { fmt os }",
         "g :: float;", "GR :: arche { g }", "@resident", f"[{N}]GR({N});",
         "h :: float;", "HC :: arche { h }", f"[{N}]HC({N});",
         "ms :: i64;", "T :: arche { ms }", "[4]T(4);",
         "@gpu", f"gpu_heavy :: map (query {{ g }}) (g) {{ g = {horner('g', lv)}; }}",
         f"cpu_heavy :: map (query {{ h }}) (h) {{ h = {horner('h', lv)}; }}",
         "seedg :: system { GR.g = { 1.0 }; }", "seedc :: system { HC.h = { 1.0 }; }",
         "sg :: system eff { os.now_ms()(now:); T.ms[0]=now; }",
         "eg :: system eff { os.now_ms()(now:); T.ms[1]=now; }",
         "sc :: system eff { os.now_ms()(now:); T.ms[2]=now; }",
         "ec :: system eff { os.now_ms()(now:); T.ms[3]=now; }",
         f'rep :: system eff {{ fmt.printf("N={N} K={K} fpe={2*lv}  GPU_ms=%d  CPU_ms=%d\\n", '
         "T.ms[1]-T.ms[0], T.ms[3]-T.ms[2]); }"]
    g = ", ".join(["gpu_heavy"] * K)
    c = ", ".join(["cpu_heavy"] * K)
    o.append(f"#run seq({{ seedg, seedc, sg, {g}, gpu.sync(GR), eg, sc, {c}, ec, rep }})")
    return "\n".join(o) + "\n"


for name, src in [("intensity_sweep.arche", intensity_sweep()), ("resident_loop.arche", resident_loop())]:
    with open(os.path.join(HERE, name), "w") as f:
        f.write(src)
    print("wrote", name)

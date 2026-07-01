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
    # Each arm gets its OWN pool + distinct column name, so no arm shares data with any other. That matters
    # now that residency + coherence are derived: a shared pool would make the GPU arm keep it resident and
    # force a `gpu.sync` before the next arm read it — a download landing inside that arm's timing window,
    # corrupting the measurement. Separate pools keep every arm independent (a GPU arm's pool is resident +
    # never host-read → measured as the compiler really runs it; a CPU arm's pool never leaves the host).
    o = ["// Decision-correctness sweep: for each arithmetic intensity, a CPU arm (plain map) and a GPU arm",
         "// (@gpu), each over its OWN static pool, timed over K dispatches. Prints 'ROW <flops/elem> <cpu_ms>",
         "// <gpu_ms>'. Each arm has a private pool so derived residency/coherence never crosses arms; the GPU",
         "// arm is measured exactly as the compiler runs it (resident pool, transfer amortized over K).",
         "#import { fmt os }"]
    for i, lv in enumerate(levels):
        o += [f"ca{i} :: float;", f"CA{i} :: arche {{ ca{i} }}", f"[{N}]CA{i}({N});",
              f"cb{i} :: float;", f"CB{i} :: arche {{ cb{i} }}", f"[{N}]CB{i}({N});"]
    # A throwaway GPU pool + map dispatched once before timing, so the one-time Vulkan device/pipeline init
    # is absorbed here instead of landing on (and inflating) whichever arm runs first.
    o += ["wc :: float;", "W :: arche { wc }", "[64]W(64);", "@gpu",
          "warm :: map (query { wc }) (wc) { wc = wc + 1.0; }",
          "seedw :: system { W.wc = { 1.0 }; }"]
    o += ["ms :: i64;", "T :: arche { ms }", f"[{4*len(levels)}]T({4*len(levels)});"]
    for i, lv in enumerate(levels):
        cb = f"ca{i} = ca{i} + 1.0;" if lv == 0 else f"ca{i} = {horner(f'ca{i}', lv)};"
        gb = f"cb{i} = cb{i} + 1.0;" if lv == 0 else f"cb{i} = {horner(f'cb{i}', lv)};"
        o.append(f"cpu{i} :: map (query {{ ca{i} }}) (ca{i}) {{ {cb} }}")
        o.append("@gpu")
        o.append(f"gpu{i} :: map (query {{ cb{i} }}) (cb{i}) {{ {gb} }}")
    for i in range(len(levels)):
        o.append(f"seedca{i} :: system {{ CA{i}.ca{i} = {{ 1.0 }}; }}")
        o.append(f"seedcb{i} :: system {{ CB{i}.cb{i} = {{ 1.0 }}; }}")
    narms = 2 * len(levels)
    for a in range(narms):
        o.append(f"s{a} :: system eff {{ os.now_ms()(now:); T.ms[{2*a}] = now; }}")
        o.append(f"e{a} :: system eff {{ os.now_ms()(now:); T.ms[{2*a+1}] = now; }}")
    parts, args = [], []
    for i in range(len(levels)):
        parts.append(f"ROW {2*levels[i]} %d %d")
        args += [f"T.ms[{2*(2*i)+1}] - T.ms[{2*(2*i)}]", f"T.ms[{2*(2*i+1)+1}] - T.ms[{2*(2*i+1)}]"]
    o.append('report :: system eff {\n  fmt.printf("' + "\\n".join(parts) + '\\n", ' + ", ".join(args) + ");\n}")
    sched = ["seedw", "warm"] + [f"seedca{i}" for i in range(len(levels))] + [f"seedcb{i}" for i in range(len(levels))]
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
    o = ["// The regime where the GPU wins: @resident data + many dispatches amortize the one-time transfer, so",
         "// the GPU's faster compute accumulates. Explicit @gpu/@resident/gpu.sync arms here PRINT the CPU-vs-GPU",
         "// comparison; derive_residency.arche is the same workload with those annotations DERIVED. Measured on a",
         "// GTX 1650: GPU ~661 ms vs CPU ~1921 ms (~2.9x).",
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

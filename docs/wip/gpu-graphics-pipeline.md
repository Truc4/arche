# WIP: GPU rendering (so resident data has an on-device consumer)

Status: **design only, not implemented.** A working doc — why rendering on the CPU blocks GPU residency for a
rendered app, and the *principled* direction for putting the render on the GPU. Companion to
[placement (static mapper)](../design/static-mapper.md): joint placement makes a resident GPU *compute*
pipeline work; this makes a resident GPU *interactive* app work.

## The problem — a CPU consumer is a per-frame host boundary

arche's rendering is a **CPU software framebuffer** (`extras/gfx`): the shim owns a pixel buffer
(`0xRRGGBB` ints), `gfx.circle`/`gfx.rect` (both `system eff`) read each entity's `pos`/`color`/`r` **on the
host** and write pixels, and the buffer is blitted to the window via `XPutImage` (X11) / `wl_surface`
(Wayland) / an in-memory buffer (headless).

That host read every frame is the wall. The coherence analysis is *correct* to refuse residency here: the
producer (physics) may be a GPU map, but the consumer (render) reads the same data on the CPU, so the data
must be downloaded every frame. No placement algorithm — not even the joint optimizer in
[placement (static mapper)](../design/static-mapper.md) — can remove an edge whose consumer genuinely lives on
the CPU. Keeping entity data in VRAM only pays off when **both** producer and consumer are on the GPU:

```
physics (GPU) writes pos → VRAM  →  render (GPU) reads VRAM → framebuffer  →  present
                       (entity data never touches host memory)
```

Separately, arche's GPU support is **compute-only** — a `map` lowers to a compute shader (`gpu_glsl.c`); there
is no vertex/fragment/rasterization pipeline in the language at all.

## The principle — rendering is a query, not a compiler feature

The tempting shortcut is a built-in draw intrinsic (`draw_circles(fb, pos, r, color)`) backed by a
hand-written compute shader. **Reject it.** Every shape and blend mode becomes another compiler built-in; the
compiler accretes a rendering API instead of staying a small set of general mechanisms. That is not a path to
1.0.

arche's thesis (README) is "structure a whole program the way a database structures data." Rendering fits that
thesis exactly — **a frame is a relational join + aggregate:**

```
framebuffer(pixel) = argmax_by_depth { entity.color | entity ∈ scene, covers(entity, pixel) }
```

Entities ⋈ pixels on a coverage predicate, aggregated per pixel by depth (or blended). Every rendering
technique — forward, deferred, tiled, ray-cast — is a *physical plan* for this one logical query. So the
principled capability is **join + aggregate over pools**; rendering is a *library* that expresses the query,
and the compiler learns nothing about graphics. The same shape is collision detection, spatial/range queries,
N-body, particle-in-cell, and database joins. It belongs in the algebra arche already started: map / reduce /
scan / sort unified under one **ParOp IR with pluggable backends** (`codegen/codegen.c`) — rendering slots
*into* that algebra, not beside it.

## The strategies are join plans

The join can be driven from either relation, or partitioned. Three honest options, differing in which relation
is parallel and how an aggregate collision (overlap) is resolved.

**A. Gather — pixel-parallel** (map over pixels, reduce over the scene). Each pixel is one thread that reduces
over the entities, testing coverage with a branchless predicate and keeping the topmost.
- *Pros.* On arche's grain: **each pixel writes its own slot** — no atomics (arche deliberately has none), no
  foreign-pool write, no scatter. **No control flow needed:** coverage is a `select`/SDF (branchless — already
  allowed on the GPU), and "loop over entities" is a *reduction primitive*, not a hand-written loop (which the
  emitter forbids). Reuses map + reduce; deterministic (the monoid fixes collision order — no race, no
  z-fighting). The framebuffer is an ordinary pool; a pixel is an ordinary row.
- *Cons.* O(pixels × entities). Fine at arche's scale (tens–hundreds of primitives); needs tiling (C) to scale.
  Requires two *general* capabilities not present yet — a func-monoid (argmax carrying a payload) and a kernel
  that reduces over a **foreign** pool (the pixel×scene join).

**B. Scatter — entity-parallel** (splat entities → pixels). Each entity thread writes the pixels it covers,
resolving overlap with an atomic depth test.
- *Pros.* Efficient — O(covered pixels), the shape a hardware rasterizer uses. The primitive
  (scatter / reduce-by-key) is **highly general**: histograms, particle-to-grid, binning, voxelization all want
  it — arguably the single most valuable primitive to add.
- *Cons.* Needs **atomics** (an atomic z-buffer) — a break from arche's no-atomics stance (GPU compute atomics
  *could* be scoped to the GPU backend, but it is a real model decision). Needs **variable fanout** (a
  per-entity bounding-box loop) — exactly the control flow GPU kernels forbid — so bounded loops or an `expand`
  primitive. Order-dependent blending needs a sort (the OIT problem). The largest new surface, and it
  contradicts two existing commitments at once.

**C. Tiled / binned — partition then local join.** Bin entities into screen tiles (a scatter or sort by tile),
then each tile's pixels gather over only that tile's entities.
- *Pros.* Scales — O(pixels × entities-per-tile) + binning (tile GPUs / Larrabee / Nanite).
- *Cons.* Strictly *more* machinery: option A **plus** a binning pass that is itself B's problem. But it is an
  **additive optimization on top of A** — a binning pre-pass, not a redesign — so choosing A doesn't foreclose
  it.

**Verdict: gather (A).** It is the only formulation that respects *both* standing commitments — **no atomics**
and **no control flow in GPU kernels** (the loop becomes a reduction, the branch becomes a `select`) — while
reusing the map+reduce algebra and keeping the framebuffer an ordinary pool. Its O(pixels × entities) is
acceptable at arche's scale; tiling is an additive upgrade if scale ever demands it. Scatter is the more
general/efficient primitive and worth adding *eventually* for histograms/particles — but it is a model change
(atomics), a separate decision best made when a workload needs it, not smuggled in through rendering.

## The general capabilities this defines (none rendering-specific)

Building rendering the principled way is really "finish the data-parallel algebra + give it a GPU backend."
Each item stands on its own merits:

1. **2-D buffers as pools** — the framebuffer becomes an ordinary `int` pixel column (today a hidden shim
   `int*`). Pixels are data.
2. **Multi-pool GPU kernels** — a kernel reads one pool and writes another (entities → framebuffer). The
   *runtime dispatch already allows this* (a flat `cols[]`); only codegen's single-pool gate
   (`cg_map_gpu_eligible`, `nmatch==1`) forbids it.
3. **General reductions** — a `func` monoid + identity (the ParOp already anticipates this); "topmost" is an
   argmax carrying a color payload, beyond `+`/`min`/`max`.
4. **Join + aggregate** — a kernel over one pool that aggregates a matched set from another pool (the pixel ×
   scene join). The single most important addition; spatial queries, N-body, and collisions need it too.
5. **A GPU ParOp backend** — today `SCHED_GPU` is a placeholder and the GPU path is hard-wired to the `map`
   kernel kind, separate from the ParOp seam. A real GPU backend lets map/reduce/join run on-device generally.
6. **Present** — the one unavoidable host touch: download the framebuffer pool and blit via the existing
   `XPutImage`/wayland path. A Vulkan WSI swapchain is the zero-copy "real" answer but a large separate
   integration — deferred.

Rendering then becomes: `extras/gfx` declares a framebuffer pool and a `render` kernel that maps over pixels
and join-aggregates the scene — pure library arche, no compiler graphics knowledge. `physics(GPU) →
render(GPU) → present`, entity data resident throughout (only the framebuffer round-trips, at present).

## Layering — what's library, what's compiler, and the order

The point of the query framing is that the **rendering-specific code is small, lives in `extras/gfx`, and is
written once**; the effort is general compiler primitives that graphics is merely the first client of.

**Step 0 — framebuffer as data (library; ships on CPU today, no compiler change).** Declare the framebuffer as
an ordinary pool (`Framebuffer { px: int }`, static max W×H), have the rasterizer write that pool instead of
the shim's hidden `int*`, and present by blitting the pool's column. Pure library + a little FFI glue; CPU
performance unchanged (keep today's entity-outer rasterizer — gather is a GPU strategy, Step 1). This delivers
the structural foundation on its own: the framebuffer is *data* and the whole render path is arche code over
pools, with zero GPU-crash risk.

**Step 1 — the gather query, on the GPU (compiler).** Express the render as a pixel-parallel join+aggregate
and lift it on-device. This needs the four general primitives below — none rendering-specific:

| capability | compiler / library | general beyond gfx? |
|---|---|---|
| framebuffer / 2-D buffer as a pool | **library** | it's just a pool |
| the render query (coverage predicate, depth argmax) | **library** (`extras/gfx`) | written once; ~no per-shape growth |
| present (download + blit) | **library / shim** (exists) | — |
| multi-pool GPU kernels (read entities, write framebuffer) | **compiler** | any cross-pool transform |
| func-monoids (a `func` combine + identity) | **compiler** | every reduce / scan |
| join + aggregate (reduce over a foreign pool) | **compiler** | spatial queries, N-body, collisions, DB joins |
| GPU ParOp backend | **compiler** | all parallel ops on-device |

So the *graphics* is a handful of library kernels; the *work* is four general data-parallel capabilities. The
gfx library doesn't grow per shape — a new shape is a new coverage `func`, not a new compiler intrinsic — and
once the four primitives exist, the same library query runs on the CPU or the GPU by placement, with no library
change.

**Later — scale and zero-copy (deferred, additive).** Tiling (a binning pre-pass on top of the gather) when
O(pixels × entities) actually bites; scatter + atomics as an independent primitive decision for
histograms/particles; a Vulkan WSI swapchain to remove the present round-trip. None of these revisit Steps 0–1
— they layer on top.

## What already exists / current restrictions (do not re-solve)

- Compute shaders (`map` → GLSL compute), one `std430` SSBO per column, dispatch + residency runtime
  (`arche_gpu_dispatch`/`sync`/`upload`). A GPU-placed map's columns are already resident buffers; the missing
  piece is a GPU *consumer* that binds the same buffers.
- The runtime dispatch is **pool-agnostic** (flat `cols[]`), so capability 2 (multi-pool kernels) is a codegen
  change, not a runtime one.
- Restrictions in the way, all deliberate: GPU kernels are **single-pool** (`cg_map_gpu_eligible`), **no
  control flow** in a pure map (E0046), **homogeneous 32-bit** columns, **`select` is the only call**, and the
  language has **no atomics**. The gather plan is chosen precisely because it needs none of these lifted (the
  branch is a `select`, the loop is a reduction) — only capabilities *added* (2-D pools, multi-pool kernels,
  func-monoids, join+aggregate, a GPU ParOp backend).
- The gfx framebuffer is a shim-owned host `int*`, not a pool; circle/rect are CPU `eff` maps with per-pixel
  loops; present is always a host blit (no Vulkan WSI/swapchain anywhere).

## Prior art

- **Rendering as a query / relational GPU work:** the join+aggregate framing is the database view of
  rasterization — GPU join & aggregation (e.g. GPU query engines), and the "z-buffer = argmin aggregate" folk
  identity.
- **Software rasterization on GPU (compute):** Laine & Karras, "High-Performance Software Rasterization on
  GPUs" (HPG 2011) + *cudaraster*; the broad GPU-driven-rendering body. Shows a full rasterizer *can* live in
  compute — and that scatter+tiling is how it scales (option B/C above).
- **Larrabee** (Seiler et al., SIGGRAPH 2008) — rendering as software on wide SIMD; the "rasterize in general
  compute" thesis and the tile-binning plan.
- **Data-parallel primitive algebras:** Blelloch's scan/segmented-scan vector model; the map/reduce/scan/sort/
  scatter vocabulary rendering is expressed in here.
- **Compute→present without a graphics pipeline:** Vulkan compute writing a swapchain/storage image, then
  presenting — the WSI path for capability 6.

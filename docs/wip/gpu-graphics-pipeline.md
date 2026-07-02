# WIP: GPU rendering (so resident data has an on-device consumer)

Status: **design only, not implemented.** A working doc — why rendering on the CPU blocks GPU residency for a
rendered app, the *principled* direction (rendering as a relational join+aggregate), and — after a
minimize-compiler cost analysis — a roadmap that splits a **zero-compiler library foundation (do now)** from a
**deferred GPU-backend generalization (no cheap version)**. Companion to
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
keeping the framebuffer an ordinary pool. Its O(pixels × entities) is acceptable at arche's scale; tiling is an
additive upgrade if scale ever demands it. Scatter is the more general/efficient primitive and worth adding
*eventually* for histograms/particles — but it is a model change (atomics), a separate decision best made when
a workload needs it, not smuggled in through rendering.

**Cost caveat (important).** Gather is minimal in *commitments respected*, **not** in *compiler cost*. Its GPU
form still needs the full backend generalization below — indexed/foreign buffer access, a bounded reduction,
multi-pool binding, and a runtime-ABI change; it only avoids *atomics*. On today's emitter neither gather nor
scatter is close to expressible (see "What already exists"). So the roadmap below splits hard along
compiler-cost, not along formulation.

## Roadmap — split by compiler cost, not by formulation

The rendering-specific code is small and lives in `extras/gfx`; the expensive part is a general GPU-backend
generalization that graphics is merely one client of. So the honest split is *what needs the compiler* vs
*what doesn't* — and almost all the near-term value is on the free side.

### Now — the library foundation (zero compiler change)

Everything that does not require an on-device *renderer* is pure library + FFI, using patterns already in
tree. This is the maximal progress available under "no new compiler work," and it is worth doing on its own:

- **Framebuffer as a pool.** `[W*H]Framebuffer { px: int }` — an ordinary static pool (no compiler cap;
  precedent is the 65536-row `CharBuf` in `tests/unit/language/csv/read_chunk.arche`).
- **Present via an FFI column slice.** Hand `Framebuffer.px[0 : W*H]` (a `[]int` view — `codegen_slice` lowers
  a static-pool column slice to a real `{ptr,len}`) to a `proc` that does the `XPutImage`/wayland blit. Same
  "pool column → C" pattern as `io.read_chunk`. Zero compiler change.
- **Clear is already GPU-free.** `map (query {px}) { px = BG }` is single-pool + branchless → GPU-eligible
  today.
- **Coherence is automatic.** A GPU write of the framebuffer followed by a host `present` read derives the
  `gpu.sync` download from the footprint — the `Framebuffer.px[0:N]` slice is a pool-qualified read the pass
  records.
- **GPU render-prep, resident, today.** Heavy *per-entity* work (projection, culling, particle/skinning
  update) as ordinary **single-pool** GPU maps keeps that data resident via the existing machinery; the CPU
  composites the compact per-entity result. Not "GPU rendering," but "GPU render-prep with residency," with no
  new compiler feature.

This delivers the structural win — *the framebuffer is data; the render path is arche library code over
pools* — with zero GPU-crash risk. It does **not** make the rasterizer itself run on-device (that is below).

*Wart to note:* a 1080p framebuffer pool inlines ECS metadata sized to N (`free_list` i64·N + `gen_counters`
i32·N) → ~33 MB for ~8 MB of pixels. Not a blocker; it motivates a *plain buffer pool* (a pool with no entity
metadata) as a small, separate, general nicety.

### Deferred — the on-device renderer (a GPU-backend generalization, no shortcut)

Running the render *itself* on the GPU (so entity data never round-trips) has **no cheap version**. Today's GPU
emitter is a single-pool, `col[i]`-only, branch-free, call-free, loop-free straight-line transform, and
single-pool is assumed at four layers: the eligibility gate (`cg_map_gpu_eligible`, `nmatch==1`), the dispatch
emit (`matching_count==1`, `cols[]` from one archetype), the GLSL param→SSBO mapping (one pool, `col[i]`
indexing), and the **runtime ABI** (one `count`/`elem_size` for all buffers). A renderer — gather *or* scatter
— needs the same generalization:

| capability | why | general beyond gfx? |
|---|---|---|
| indexed / foreign buffer access in the emitter (`buf[expr]`) | read the scene / write `fb[y*w+x]` | any gather/scatter kernel |
| a bounded reduction (or an unrollable static loop) | "reduce over the scene" — there is no loop/unroll substrate today | every reduce / scan / join |
| multi-pool kernel binding + a runtime-ABI change (per-column count) | bind entities *and* framebuffer in one dispatch | any cross-pool transform |
| general reductions (`func` combine + identity) | "topmost" is an argmax with a payload, beyond `+`/`max` | every reduce |

These are **general data-parallel capabilities**, not graphics — they must be justified on their own merits
(spatial queries, N-body, joins, histograms), and they are only *worth* it once a workload has entities ≫
framebuffer (a CPU renderer already downloads only O(entities) per frame, so the GPU renderer's sole win —
downloading O(framebuffer) instead — pays off only at large N). Gather stays the right *formulation* on top of
them (no atomics), but it is a backend project, not "the next step." Tiling and a Vulkan WSI swapchain (to kill
the present round-trip) layer on top of that, later still.

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

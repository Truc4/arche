# WIP: GPU rendering (so resident data has an on-device consumer)

Status: **design only, not implemented.** A working doc — why rendering on the CPU blocks GPU residency for a
rendered app, and the directions for putting the render on the GPU. Companion to
[`joint-placement.md`](joint-placement.md): joint placement makes resident GPU *compute* pipelines work; this
makes a resident GPU *interactive* app work.

## The problem — a CPU consumer is a per-frame host boundary

arche's rendering is a **CPU software framebuffer** (`extras/gfx`): the shim owns a pixel buffer
(`0xRRGGBB` ints), `gfx.circle`/`gfx.rect` (both `system eff`) read each entity's `pos`/`color`/`r` **on the
host** and write pixels, and the buffer is blitted to the window via `XPutImage` (X11) / `wl_surface`
(Wayland) / an in-memory buffer (headless).

That host read every frame is the wall. The coherence analysis is *correct* to refuse residency here: the
producer (physics) may be a GPU map, but the consumer (render) reads the same data on the CPU, so the data
must be downloaded every frame. No placement algorithm — not even the joint optimizer in
[`joint-placement.md`](joint-placement.md) — can remove an edge whose consumer genuinely lives on the CPU.
Keeping entity data in VRAM only pays off when **both** producer and consumer are on the GPU:

```
physics (GPU) writes pos → VRAM  →  render (GPU) reads VRAM → framebuffer  →  present
                       (entity data never touches host memory)
```

Separately, arche's GPU support is **compute-only** — a `map` lowers to a compute shader (`gpu_glsl.c`); there
is no vertex/fragment/rasterization pipeline in the language at all. So rendering cannot currently run on the
GPU: there is nothing for it to run on.

## What already works (do not re-solve)

- Compute shaders (`map` → GLSL compute), one `std430` SSBO per column, dispatch + residency runtime
  (`arche_gpu_dispatch`, `arche_gpu_sync`). **When a map is GPU-placed, its columns are already resident GPU
  buffers.** The missing piece is a GPU consumer that reads those same buffers.
- The gfx backends are cleanly separated (x11/wayland/headless shims behind one datasheet), so a new present
  path can slot beside them.

## Direction — rendering as compute (the arche-fit step), then present

Because arche's GPU is compute-only and gfx is *already a software rasterizer*, the smallest coherent step is
to lower the rasterizer to **compute** rather than build a full graphics pipeline:

1. The **framebuffer becomes a resident GPU buffer** (an `int` pixel column — now expressible, given
   [integer GPU shaders](../design/static-mapper.md)).
2. **Drawing becomes a GPU kernel** over the resident entity buffers, writing the resident framebuffer:
   physics(compute) → clear+draw(compute) → present, with entity data never leaving VRAM.
3. **Present** the framebuffer buffer to the window.

This closes the loop without a vertex/fragment pipeline, and it reuses the compute path and residency machinery
that already exist. A full graphics pipeline (VkRenderPass, vertex/fragment shaders, swapchain) is the larger
alternative; compute-only rasterization is the incremental one and matches arche's existing GPU model.

## Blockers / open problems

- **The compute emitter forbids control flow.** `gpu_glsl.c` whitelists branchless arithmetic + `select`; a
  rasterizer has bounds-clamping branches and per-pixel loops (`gfx.circle`'s nested `for`). Compute
  rasterization needs either (a) the emitter extended to lower loops/`if` (a significant step — today control
  flow is exactly what keeps a map off the GPU), or (b) a **built-in draw primitive** (a
  `draw_circles(framebuffer, pos, r, color)`-style intrinsic backed by a hand-written compute shader), which
  sidesteps emitting a general rasterizer.
- **The present path.** The framebuffer must reach the window. Two options:
  - **Download-then-blit**: copy only the *framebuffer* buffer host-side each frame and present via the
    existing `XPutImage`/wl path. The entity data stays resident (the win); only the framebuffer round-trips —
    smaller and fixed-size, but still a host touch per frame.
  - **Vulkan WSI swapchain**: present the GPU buffer directly, zero host touch — the "real" answer, but a
    large new integration (surface/swapchain/present-queue, per windowing system) that the current
    software-framebuffer shims deliberately avoid.
- **`gfx.circle` is effectful.** It's a `system eff` doing host framebuffer + windowing writes. A GPU form
  needs a GPU-framebuffer abstraction and to be re-expressed as a kernel over resident buffers — a different
  shape from the current per-pixel host loops.
- **Interop.** The rasterizer kernel must bind the *same* resident SSBOs the physics map wrote (pos/color) —
  which the joint-placement / coherence machinery must recognize as a producer→consumer edge that stays
  on-device (no sync inserted between them).
- **Scope.** This is a rendering subsystem, not a codegen tweak. It only matters once
  [`joint-placement.md`](joint-placement.md) can keep the physics resident in the first place — the two
  together are what make a GPU-resident game loop possible; neither alone does.

## Prior art

- **Software rasterization on GPU (compute):** Laine & Karras, "High-Performance Software Rasterization on
  GPUs" (HPG 2011) + *cudaraster*; the broad body of compute-shader / GPU-driven rendering. Shows a full
  rasterizer *can* live in compute — the model this direction follows.
- **Larrabee** (Seiler et al., SIGGRAPH 2008) — rendering as software on wide SIMD; the "rasterize in general
  compute" thesis.
- **Compute→present without a graphics pipeline:** Vulkan compute writing a swapchain/storage image, then
  presenting — the WSI path for option (b) above.
- **Compute/graphics interop:** CUDA–OpenGL / CUDA–Vulkan buffer sharing, GPU-driven rendering — the
  established pattern of a compute stage and a draw stage sharing on-device buffers with no host round-trip,
  which is the coherence property this doc is chasing.

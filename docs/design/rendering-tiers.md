# Rendering tiers — arche owns the renderer; the platform is a surface

**Status: roadmap.** This describes the target rendering architecture, not shipped behaviour. Tier 0 (native
software framebuffer) ships today (`extras/gfx`, arche-rpg). Tier 1 (the same, blitted to a browser canvas)
is planned. Tier 2 (GPU) is the destination this document specifies so the design is fixed before we build
toward it.

## The philosophy

An arche visual app **renders its own world** — every pixel, including text — and hands the finished result
to whatever surface is in front of it. The platform (an X11/Wayland window, a browser `<canvas>`, later a
GPU swapchain) is a **dumb blit target**, never the master. Consequences:

- **The arche program is 100% portable.** It never calls a platform widget, a DOM node, or a browser API.
  Only the `gfx` **backend** differs between native and web; the world, physics, layout, and interaction
  code are byte-identical. arche-rpg already proves this natively.
- **You are not beholden to the browser.** "Runs in the browser" is one deployment of a desktop app, via
  wasm — not an architecture the browser dictates. Text is affected by physics because text is *ours*, a
  thing in the world, not a DOM `<span>` the compositor owns.

The only axis that actually varies is **how the world is rasterized**. That is the tier ladder.

## The tier ladder

| Tier | Rasterizer | Native surface | Web surface | Status |
|------|-----------|----------------|-------------|--------|
| 0 | CPU → `[W*H]int` framebuffer | X11/Wayland blit | — | **ships** (`extras/gfx`) |
| 1 | CPU → framebuffer | X11/Wayland blit | `canvas.putImageData` | planned (see the wasm-gfx plan) |
| 2 | **GPU draw calls + glyph atlas** | Vulkan | WebGPU | **this document** |

Every tier presents the **same arche-facing `gfx` device API** (open a surface, submit a frame, read input).
Moving up a tier swaps the rasterizer under that API — the app does not change. If Tier 1's CPU rasterizer
hits a ceiling (full-screen high-DPI, thousands of physics-transformed glyphs at 60 fps), Tier 2 is an
in-place upgrade, not a rewrite.

## Tier 2 — GPU rendering (the target)

### Why it exists

CPU software rasterization has two ceilings that a physics-driven, text-heavy world hits:

1. **Transformed text.** Rotating/scaling a glyph in software means re-rasterizing it per transform (or
   caching per angle-bucket) — expensive when every letter is a physics body. On the GPU a glyph is a
   textured quad; position/rotation/scale/colour are a per-instance transform applied for free.
2. **Element count + fill.** Thousands of independently-transformed elements, or full-screen overdraw at
   60 fps, saturate a CPU rasterizer + the per-frame framebuffer copy. The GPU eats this.

### The model

arche builds a **frame's worth of instances** and hands them to a GPU backend that issues a small number of
**instanced draw calls**. There is no immediate-mode "draw this glyph now" chatter across the boundary — the
frame is data, submitted once.

- **Everything is a textured quad.** Sprites, UI rects, and **glyphs** are all quads sampling a region of a
  **texture atlas**. One draw call renders every quad that shares an atlas + pipeline; batching is by atlas,
  not by element.
- **Per-instance attributes** = `{ x, y, rotation, scale, u0,v0,u1,v1 (atlas rect), rgba }`. A physics body's
  transform *is* its instance data — "text affected by physics" is just the sim writing `rotation`/`x`/`y`
  into the instance columns.
- **The columnar payoff.** These per-instance attributes are exactly arche's DOD **pool columns**. The world
  sim already stores `pos.x`, `pos.y`, `angle`, `color` as contiguous columns; the GPU instance buffer is a
  view over those columns (or a cheap pack of them). Near-zero marshaling — the same data the physics writes
  is the data the GPU reads. This is why arche is unusually well-suited to Tier 2.

### Text

- **Glyph atlas.** Each glyph is rasterized **once** into the atlas texture (via a font rasterizer — a
  `stb_truetype`-style outline rasterizer, portable C compiled into both backends — or a prebaked atlas),
  keyed by `(font, size, glyph)`. Drawing a string is emitting one quad per glyph, each pointing at its
  atlas rect. No per-frame glyph rasterization.
- **Scale independence (optional):** a signed-distance-field (SDF) atlas lets one rasterization scale/rotate
  crisply across sizes — worth it if type ranges over many sizes; a plain bitmap atlas per size is simpler
  and fine to start.
- **Metrics for layout + collision.** Glyph advance widths / bounding boxes come from the same rasterizer
  (or, on web, a one-time `measureText` at load) and are fed back into arche so the sim can lay out lines and
  collide letters. Measured once, then the sim is self-contained.

### The portable GPU abstraction

One arche-facing device, two backends behind it:

- **Vulkan** (native) — arche already ships Vulkan for `@gpu` *compute* (SPIR-V embed + an in-binary
  dispatcher, `runtime/gpu_runtime.c`, `codegen/gpu_embed.c`). Tier 2 adds the *rendering* half: a render
  pass, vertex/instance buffers, the atlas texture, and a **window/swapchain surface**. Shaders are the same
  GLSL→SPIR-V path (`codegen/gpu_glsl.c`) extended with a vertex + fragment stage for the quad pipeline.
- **WebGPU** (web) — the browser's modern GPU API (or a WebGL2 fallback). A host-side JS backend that owns
  the `GPUDevice`, the atlas texture, the instance buffer, and the canvas render target; arche (wasm) writes
  the instance columns into shared linear memory and signals "present frame N", the JS backend uploads the
  changed columns and draws. WGSL shaders mirror the SPIR-V ones.

The `gfx` device's API is identical to Tier 0/1 — `open`, submit a frame (now: instance columns + atlas
handles rather than a pixel buffer), `poll` input. A program written against it runs on all three tiers.

### What exists vs. what's needed

- **Exists:** the native software renderer (Tier 0), the Vulkan *compute* stack (instance/device/queue,
  SPIR-V embedding, GLSL codegen), the DOD columnar world model, the wasm backend (compute; Tier-1 canvas is
  the immediate next step).
- **Needed for Tier 2:** a render pipeline (vertex/fragment, instanced quads) on top of the Vulkan stack; a
  swapchain/canvas surface; a texture atlas + a portable font rasterizer (or prebaked atlas); the WebGPU
  host backend; and the `gfx` device's frame-submission API generalized from "here is a pixel buffer" to
  "here are instance columns + atlas handles."

### Non-goals for Tier 2

- A general retained-mode scene graph / widget toolkit — arche's world *is* the scene (pools of things);
  the renderer walks columns, it does not own a DOM.
- HTML/CSS/DOM interop — deliberately none; the point is to not be beholden to the browser.
- Matching the browser's exact typographic engine — we own type (atlas glyphs), trading pixel-perfect
  system-font fidelity for a world where text is a physical, transformable object.

## See also

- The **wasm-gfx (Tier 1) plan** — framebuffer→canvas blit + input imports; the concrete next step that
  turns "headless compute in the browser" into "the world in the browser," native-parity via the shared
  `gfx` API.
- `extras/gfx` — the shipping Tier 0 software renderer (the `Framebuffer` model, `clear`/`present`/rasterizers).
- `docs/design/data-parallel-backends.md` — the DOD/columnar model the instance buffers reuse.
- [[project_wasm_backend]] / [[project_arche_wasm]] — the wasm compute backend and the browser demo project.

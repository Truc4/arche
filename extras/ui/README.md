# `ui` — an immediate-composed UI element library

A family of composable UI **element devices** built on `gfx`, plus a shared **core** module. A driver owns all the
data (columns) and drives each element's systems; every element renders identically on a native framebuffer
(`window` backend) or as real DOM elements (`dom` backend).

## Layering

```
gfx        window + framebuffer + input
ui/ui      the core: layout constants + funcs + the shared 8x8 font   (import as `ui`)
panel      element device: dark frame + title bar
textedit   element device: multi-line code editor
textview   element device: read-only multi-line text output
button     element device: clickable control
```

`ui/` is a container directory (a lib path), not a module. The core lives at `ui/ui/` so `#import { ui }` resolves
to it unambiguously; the element devices are siblings (`ui/panel`, `ui/textedit`, …). Add both `extras` and
`extras/ui` to `[lib] paths`.

## Design principles

- **ECS-shaped, not IMGUI.** Pool/framebuffer access must live in a `system` (the compiler forbids it in a func),
  so widgets are systems over driver-owned columns, not inline calls. Interaction is a column flag a consumer
  reads (`bclicked`, `submit`) — "a producer writes a column, a consumer reads it."
- **Decoupled from gfx.** No element imports `gfx`. Window backends draw into the driver's shared `{framebuffer}`
  and read the viewport + input from columns (`vpw`/`vph`, `ukey`, `umx`/`umy`/`umd`). The driver bridges: it reads
  `gfx.dims`/`gfx.key`/`gfx.mouse_*` into those columns each frame and hands the framebuffer to `gfx.present`.
- **One data model, two renderers.** The same columns feed both backends, so native and browser match. The dom
  backends reconcile real elements inside the panel `<div>` (`#ui-panel`); the window backends blit into the
  framebuffer with the shared `ui.GLYPHS` font (cp850 0x20–0x7E, indexed `char - 0x20`).

## Core (`ui`)

Layout metrics (render px): `PAD`, `GAP`, `TITLE_H`. Pure layout funcs: `content_top(py)`, `content_x(px)`,
`content_w(pw)`, `next(cy, h)`. Font accessor row: `GLYPHS[gi*8 + row]`.

## Element interfaces

Each element device owns no storage; the driver provides the columns. `[select] <element> = "window" | "dom"`.

### `panel` — framed container with a title bar
- Columns: `{pnx, pny, pnw, pnh}` frame rect, `{ptitle, ptlen}` title, `{vpw, vph}` viewport.
- Systems: `render`. (dom builds `#ui-panel`; the other elements mount inside it.)

### `textedit` — code editor
- Columns: `{ebuf, elen, ecur}`, `{submit}` (raised on Ctrl-R / Cmd-Enter), `{evx, evy, evw, evh}` rect,
  `{ukey}` one key/frame (driver fills from `gfx.key`), `{vpw, vph}`.
- Systems: `open` (seed/focus), `step` (native edit from `ukey`), `text` (dom: textarea → `ebuf`),
  `poll_run` (dom: Cmd-Enter → `submit`), `render` (native blit).
- Key sentinels in `ukey` (same as gfx): Left 1000, Right 1001, Up 1002, Down 1003; Enter 13, Backspace 8,
  Ctrl-R 18; printable = ASCII.

### `textview` — output view
- Columns: `{obuf, olen}`, `{ovx, ovy, ovw, ovh}` rect, `{vpw, vph}`.
- Systems: `render`.

### `button` — clickable control
- Columns: `{bx, by, bw, bh}` rect, `{blabel, blen}`, `{bclicked}` (1 the frame a click lands), `{bwasdown}`
  (window edge state), `{umx, umy, umd}` pointer (driver fills from `gfx.mouse_*`), `{vpw, vph}`.
- Systems: `render` (draw / set label), `poll` (click → `{bclicked}`).

## Composing (driver responsibilities)

1. Declare one archetype per element, all carrying `{vpw, vph}`; seed them.
2. Each frame, a `ui_input` system reads `gfx.dims`/`key`/`mouse_*` and fans them into `{vpw,vph}` (all elements),
   `{ukey}` (editor), `{umx,umy,umd}` (button).
3. A layout system projects/places the panel rect, then stacks the child rects with `ui.content_top`/`next`.
4. Schedule the element systems; render into the framebuffer, then `gfx.present`.

See `arche-portfolio/src/portfolio.arche` for a full composition (playground embedded in a scrolling scene).

## Note: the shared font relies on cross-module array-const access

`ui.GLYPHS[i]` reads a foreign module's array const — historically dropped by the analyzer (mis-typed as the
element scalar) and fixed in `semantic.c` (`analyze_base_chain` + `index_base_type_id`). Regression test:
`tests/unit/language/declarations/cross_module_const_array.arche`.

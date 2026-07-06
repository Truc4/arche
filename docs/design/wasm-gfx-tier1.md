# Tier 1 wasm gfx — implementation record, decisions, and sharp edges

**Status: shipped.** A `--arch=wasm32` build of a visual arche program (one that renders a software
framebuffer via the `gfx` device) now runs in a browser: the same source that drives a native X11/Wayland
window compiles to a **wasi reactor** whose framebuffer is blitted to a `<canvas>`. See
`docs/design/rendering-tiers.md` for the tier ladder (this is Tier 1) and the arche-wasm project
(`~/Code/arche-wasm`, `src/balls.arche`) for the shipped demo + Playwright e2e.

This document logs, per the standing request: (1) every autonomous design decision, and (2) every language
gotcha / compiler sharp edge encountered — so nothing is silently baked in.

## What shipped

- **`extras/gfx/wasm/backend.arche`** — a shim-less gfx variant (mirrors `headless/`). No C shim, no `#link`;
  its six `gfx_be_*` externs are left undefined so the wasm link turns them into `env` imports the JS host
  fulfils.
- **Reactor codegen** (`codegen/codegen.c`) — a `--arch=wasm32` program whose `#run` schedule has a
  top-level `forever` is split into two exported functions: `@arche_run` (everything before the loop, plus
  the pool alloc-init `@main` would normally do) and `@arche_frame` (the loop body, one tick). No `@main`.
  Gated by `g_codegen_target_wasm`; the verdict is exposed via `codegen_was_reactor()`.
- **Reactor link** (`compile/compile.c`) — the wasm clang command gains `-Wl,--allow-undefined` (always) and,
  for a reactor, `-mexec-model=reactor -Wl,--export=arche_run,--export=arche_frame`.
- **Browser host** (`~/Code/arche-wasm/www/gfx.js`) — provides the `env.gfx_be_*` imports (present →
  `putImageData`), reuses `wasi.js` for the WASI imports, and drives `_initialize()` → `arche_run()` once →
  `arche_frame()` per `requestAnimationFrame`.

## Autonomous decisions

1. **Reactor by auto-splitting the existing `forever`, not a new `#frame` directive.** The per-frame systems
   are byte-identical native↔web; only the top-level driver differs. Rather than add language surface, the
   wasm build reinterprets the existing `#run seq({ …init…, forever(body) })` shape — init = pre-loop, frame
   = loop body. This keeps the arche program *the same source* on both targets (the user's "same app" value).
   Alternatives weighed and rejected: an explicit `#frame` directive (more surface, two drivers in source);
   a Web Worker running the blocking loop (needs OffscreenCanvas + SharedArrayBuffer/Atomics for pacing +
   input; rejected with the user in planning).
2. **Host protocol: `_initialize()` → `arche_run()` once → `arche_frame()` per rAF.** Main thread, no worker,
   no SharedArrayBuffer, natural 60fps pacing, and the direct path to per-frame input later.
3. **Alloc-init emitted into `@arche_run`** for the reactor (there is no `@main` to run it); the host calls
   `arche_run` once at startup, so pools are initialized before the first frame.
4. **Codegen owns reactor detection; the linker reads the verdict.** `codegen_was_reactor()` lets
   `compile.c` choose `-mexec-model=reactor` + the exports after codegen has walked the schedule tree —
   codegen is the only place that has the tree.
5. **`-Wl,--allow-undefined` applied to every wasm link,** not a per-symbol allowlist — simplest thing that
   makes the `gfx_be_*` imports work. (Trade-off noted below.)
6. **Integer ball positions + branch-free `select` bounce.** `gfx.circle` indexes the framebuffer with ints,
   and a `map` rejects control flow (E0046), so the demo bounces with `select(next-out-of-band, -v, v)`.
7. **Canvas host uses main-thread `putImageData`** (no OffscreenCanvas/worker — unnecessary for a rAF
   reactor). `#screen[data-status="live"]` after the first `present` is the deterministic e2e signal (no
   arbitrary timeouts); animation is asserted by two canvas fingerprints differing.
8. **Screenshot captured via a direct Chromium script,** not the Playwright MCP: the MCP is registered and
   connects, but its `browser_*` tools only load at session start and were added mid-session, so they never
   surfaced. The `@playwright/test` e2e is the real gate regardless.
9. **The GPU (Tier 2) work is NOT started on this branch** — it belongs on its own branch, and per the
   standing "user owns all git" constraint the branch must be created by the user; the compiler is not run
   against git here.

## Language gotchas (all resolved cleanly — no standing workarounds in the shipped code)

1. **`move` is a reserved keyword** (the ownership `move`). A system named `move` fails to parse with a
   cryptic cascade (`Expected declaration` on later lines, not "reserved word"). Renamed to `step`.
2. **Distinct scalar component types need the `T(x)` cast on assignment from a plain int** — `r = r(rr)`,
   `color = color(0x…)`. Tuple *sub*columns (`pos.x`) collapse to their backing and take a bare int. This is
   the type-identity model working as designed; the asymmetry (scalar needs a cast, subcolumn doesn't) is
   the surprise.
3. **Component types must be declared** — `handle :: window; bg :: int;`. An *undeclared* component name
   silently lowers to an empty `%struct.<name>` and blows up in `opt` with `invalid type for function
   argument` rather than a clean semantic error. (See follow-ups — this is a diagnostic gap, not a language
   limitation.)
4. **`window` (opaque) lowers to i64** → the JS host import must use `BigInt` (`return 1n`; the handle
   arrives back as a BigInt). A number `1` throws `Cannot convert 1 to a BigInt` from inside the wasm.
5. **Non-issue, verified:** unary minus on a member (`-vel.x`) *does* work — an earlier `0 - vel.x`
   workaround was written while chasing the `move`-keyword parse cascade and was removed after a direct test.
   Recorded so it is not mistaken for a real limitation.

## Sharp edges / trade-offs introduced (candidates for follow-up)

1. **`--allow-undefined` is a blunt instrument** — it makes *every* undefined symbol a wasm import, so a
   typo'd extern silently becomes an import instead of a link error. Tighten to
   `-Wl,--allow-undefined-file=<list>` generated from the device's `#foreign` contract.
2. **Reactor trigger is a structural heuristic** (`--arch=wasm32` + a top-level `forever`). Only the *first*
   top-level `forever` is split; one nested deeper or a second one is not handled (it falls back to the
   command model / is left in `arche_run`). Fine for the arche-rpg/demo shape; document or generalize.
3. **Reactor coherence/joint-placement run per half** (init and frame emitted as separate schedule
   functions), so GPU residency across the init↔frame boundary is not tracked. Inert for CPU programs; a
   real limitation if a reactor ran `@gpu` maps — revisit when Tier 2 lands.
4. **`dprintf` signature-mismatch warning** on the wasm link (io.c's decl vs wasi-libc's) — pre-existing,
   harmless (the .wasm links and runs); not introduced here.
5. **`gfx_be_close` is gc'd from the reactor** — the `[1]` window singleton is never dropped, so `@drop`'s
   `gfx_be_close` is unreferenced and wasm-ld drops the import. The host provides a no-op anyway.

## Suggested follow-ups (not done here)

- A semantic diagnostic for an **undeclared component type** (gotcha #3) instead of the downstream `opt`
  crash.
- `--allow-undefined-file` from the `#foreign` contract (sharp edge #1).
- Per-frame **input** (pointer/scroll/keyboard) — the reactor makes this the natural next step: the host
  writes input state into wasm memory / an import the `frame` reads.
- Generalize / make explicit the reactor trigger (sharp edge #2).

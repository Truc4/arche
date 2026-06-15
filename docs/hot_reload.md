# Hot reload (`arche run`)

`arche run` is the dev loop: you edit a device and the change appears in the already-running program,
without restarting it. `arche build` is the release path: one static binary, every device call a direct
call, **zero** reload machinery. There is **no flag** and **no function-pointer type in the language** —
the run/build distinction is the only switch, and the indirection that makes reload possible is a
compiler-internal detail of the dev build that is compiled out of release.

## Why it works at all

A device is **behavior-only**; the driver owns **all** state (its pools, and any C-side state reached
through an `opaque` it holds). So a device's *code* can be swapped while its *state* lives on untouched —
the property hand-rolled hot-reload (Handmade Hero / Odin / Jai) works hard to get, that arche gets from
its device/driver split.

## How it works

- **Build.** Under `arche run`, each imported device compiles to its own reloadable `.so` under
  `$ARCHE_HOT_DIR` (default `<project>/build/.arche-hot`); the driver becomes the host exe, linked
  `-rdynamic` so the thin device `.so`s resolve the runtime / C shims / pools / `arche_hot_resolve` from
  it at load.
- **Dispatch.** A driver→device call lowers to a trampoline (`codegen.c`, `ctx->hot`): `arche_hot_resolve(unit, "sym")`
  + one indirect call. It is the only indirect call arche emits, and only in hot mode. Systems
  (`run dev.sys`) are emitted in their declaring device's unit and trampolined the same way, so editing a
  *system* body reloads live too — the system writes the driver's pool through the run-site pointer, so
  pool state survives.
- **Reload.** The host (`runtime/hotreload.c`) `stat`s each device `.so`; when its mtime changes it
  copies to a versioned temp (dodging `dlopen`'s realpath cache) and `dlclose`+`dlopen`s. mtime is read
  at **nanosecond** precision — a save within the same wall-clock second as the last build must still be
  seen (seconds granularity silently misses fast edits). The watcher in `cli/cmd_run.c` uses the same
  resolution; the two **must** match.
- **Watcher.** While the host is alive, `cmd_run` polls the project tree's newest `.arche` mtime every
  200 ms; on a bump it recompiles. The per-device `.so` build is content-hash gated (`build_unit_so`), so
  only the *edited* device's `.so` is rewritten — its mtime bumps, the host reloads just that one, every
  other device is left alone. The host exe is rebuilt too but never re-exec'd (the live child runs a
  staged copy, so the relink can't hit `ETXTBSY`). A one-shot program just exits, so the watcher returns
  immediately — no hang.

## Release purity (the invariant)

`arche build` never sets `ARCHE_HOT_DIR`, so `ctx->hot` is off: plain `declare` + direct calls,
byte-identical to a no-reload world. `tests/unit/compiler/per_unit/release_no_hot.arche` greps the emitted
IR and asserts **zero** `arche_hot_resolve`.

## Tests

- `tests/unit/compiler/per_unit/release_no_hot.arche` — release has no reload indirection.
- `tests/unit/compiler/per_unit/run_hot_dispatch.arche` — dev builds device `.so`s + uses the trampoline.
- `tests/integration/hot_reload.py` — live edit of a device **proc** reloads into the running host.
- `tests/integration/hot_reload_system.py` — live edit of a device **system**; the driver pool survives.
- `tests/integration/hot_reload_gfx.py` — device→device reload (a draw device through `gfx`), pixel-verified
  headless; proves the `gfx` window `opaque` + its C framebuffer survive while only the draw device reloads.
- `tests/integration/gfx_raster.py` — `gfx.clear/rect/circle` pixel correctness via the headless backend.

The reload integration tests are timing-sensitive (a live build + reload, possibly under heavy parallel CI
load), so each retries once: a real regression fails every attempt, a scheduling blip does not.

## Deferred rebuild work (why / why-not)

These are intentionally **not** done. They are tracked here and in a `TODO` block in `cli/cmd_run.c`.

| Item | What it is | Why it's fine for now | What would change that |
|------|------------|-----------------------|------------------------|
| **Front-end incrementality** | An edit re-runs the whole front-end (parse + analyze every unit), then relinks only the changed device `.so` (objects are cached). | Whole-program analysis is sub-second for real projects; reload latency is dominated by the `cc -shared` link, not analysis. It's a dev-only path. | A large project where edit→pixel latency becomes noticeable. **Benchmark first** — incremental analysis is real complexity. |
| **Per-call dispatch cost** | Every cross-device call does `stat` + `dlsym` via `arche_hot_resolve`. | Device calls are coarse: one `run game.sys` processes *all* entities in a single call; the per-entity loop is *inside* the system. ~tens of device calls/frame → ~0.2% at 60 fps. | A workload making many fine-grained per-entity cross-device calls per frame. Then cache the resolved pointer, invalidated by a per-unit reload-generation counter. |
| **Edit debounce** | A burst of saves triggers a rebuild per 200 ms poll tick. | Partial writes are already retried (the watcher advances its baseline only on a successful rebuild), so a torn save self-heals; extra rebuilds are just wasted work, never wrong output. | An editor that writes many times per save, making rebuilds churn. Then coalesce events over a short window. |
| **`.so` cleanup** | `$ARCHE_HOT_DIR` accumulates `unit_N.so`, `.hash` sidecars, and the runtime's versioned `.hot.<gen>` copies across a session. | They're small and live under `build/` (gitignored); a fresh `run` overwrites the live ones. | Long sessions on a small disk. Then prune stale generations (keep the live one) on startup/exit. |
| **Stdout=DEVNULL launch wedge** | A Python `subprocess` launching `arche run` with `stdout=DEVNULL` + `stderr=file` + `start_new_session` can wedge the host on some boxes. | Not a real arche bug: shell `arche run >/dev/null` and a piped stdout both work; only this exact fd combo trips it. The integration tests route stdout to a file. | Worth root-causing the fd-inheritance interaction in the fork/exec path; low priority. |

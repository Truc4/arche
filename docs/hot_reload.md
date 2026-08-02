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
  + one indirect call. It is the only indirect call arche emits, and only in hot mode. Maps
  (`run dev.map`) are emitted in their declaring device's unit and trampolined the same way, so editing a
  *map* body reloads live too — the map writes the driver's pool through the run-site pointer, so
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
- `tests/integration/hot_reload_system.py` — live edit of a device **map**; the driver pool survives.
- `tests/integration/hot_reload_gfx.py` — device→device reload (a draw device through `gfx`), pixel-verified
  headless; proves the `gfx` window `opaque` + its C framebuffer survive while only the draw device reloads.
- `tests/integration/gfx_raster.py` — `gfx.clear/rect/circle` pixel correctness via the headless backend.

The reload integration tests are timing-sensitive (a live build + reload, possibly under heavy parallel CI
load), so each retries once: a real regression fails every attempt, a scheduling blip does not.

## Concurrent sessions: one owns the shared dir

The hot dir holds each device's live `unit_N.so`, and the running host polls those paths and re-dlopens on
change. Two sessions sharing it would therefore publish over each other, and a host would load a library
built for a different program — unit indices map to different devices, so this segfaults rather than
misbehaving quietly.

**One session owns `<project>/build/.arche-hot` at a time.** `arche run` takes an exclusive `flock` on
`<hotdir>/.lock`; a session that cannot take it falls back to `<project>/build/.arche-hot-<pid>` and
removes that tree on exit. So:

- the normal single-session workflow is unchanged and keeps reusing `unit_N.so` across runs — that reuse
  is the point of a stable dir, since the `cc -shared` link dominates reload latency;
- a second concurrent session (another developer, a second terminal, a parallel test runner) is isolated
  automatically, with no environment set up by the caller;
- `flock` is released by the kernel on process exit, so an interrupted or killed run leaves no stale lock.

The inspect socket lives under the hot dir, so it follows the same rule: the owning session serves the
project's socket, a concurrent one serves its own.

An explicit `ARCHE_HOT_DIR` still wins and is *not* locked — pointing two processes at one dir is
supported (the integration tests and `tests/unit/compiler/per_unit/singleton_read.arche` do exactly that).
For that case the runtime's reload copy is per-process: `ensure_loaded` copies to
`<path>.hot.<pid>.<gen>` before `dlopen`, so two hosts at the same generation cannot truncate each other's
image mid-load.

Guarded by `tests/unit/runtime/hot_concurrent_run.py` (64 concurrent runs of one project, default
environment). Note that a crashed host is easy to miss: the SIGSEGV handler
(`runtime/stack_check.c`) prints `stack overflow` for **any** segfault and exits **0** — a deliberate
choice (see `tests/unit/language/errors/stack_overflow.arche`) — so the only symptom is missing stdout.

## Deferred rebuild work (why / why-not)

These are intentionally **not** done. They are tracked here and in a `TODO` block in `cli/cmd_run.c`.

| Item | What it is | Why it's fine for now | What would change that |
|------|------------|-----------------------|------------------------|
| **Front-end incrementality** | An edit re-runs the whole front-end (parse + analyze every unit), then relinks only the changed device `.so` (objects are cached). | Whole-program analysis is sub-second for real projects; reload latency is dominated by the `cc -shared` link, not analysis. It's a dev-only path. | A large project where edit→pixel latency becomes noticeable. **Benchmark first** — incremental analysis is real complexity. |
| **Per-call dispatch cost** | Every cross-device call does `stat` + `dlsym` via `arche_hot_resolve`. | Device calls are coarse: one `run game.map` processes *all* entities in a single call; the per-entity loop is *inside* the map. ~tens of device calls/frame → ~0.2% at 60 fps. | A workload making many fine-grained per-entity cross-device calls per frame. Then cache the resolved pointer, invalidated by a per-unit reload-generation counter. |
| **Edit debounce** | A burst of saves triggers a rebuild per 200 ms poll tick. | Partial writes are already retried (the watcher advances its baseline only on a successful rebuild), so a torn save self-heals; extra rebuilds are just wasted work, never wrong output. | An editor that writes many times per save, making rebuilds churn. Then coalesce events over a short window. |
| **`.so` cleanup** | `$ARCHE_HOT_DIR` accumulates `unit_N.so`, `.hash` sidecars, and the runtime's versioned `.hot.<gen>` copies across a session. | They're small and live under `build/` (gitignored); a fresh `run` overwrites the live ones. | Long sessions on a small disk. Then prune stale generations (keep the live one) on startup/exit. |
| **Stdout=DEVNULL launch wedge** | A Python `subprocess` launching `arche run` with `stdout=DEVNULL` + `stderr=file` + `start_new_session` can wedge the host on some boxes. | Not a real arche bug: shell `arche run >/dev/null` and a piped stdout both work; only this exact fd combo trips it. The integration tests route stdout to a file. | Worth root-causing the fd-inheritance interaction in the fork/exec path; low priority. |

# WIP: proc-elimination & io-as-data — decisions (TEMPORARY)

> **This file is scratch.** It tracks the in-flight proc-elimination / io-as-data conversion so we don't
> lose the thread between sessions. Delete it once the conversion lands and the durable bits have moved
> into `docs/` (patterns.md, language.md, docs/design/). Not a spec, not durable.

## North star

**arche-rpg is the model.** The whole program is `#run { seq({ boot, forever(seq({ systems… })) }) }` over
pool declarations — no `proc`, no `main`. Systems/maps/eaches operate on pools; data flows through
**columns** (system 1 writes a column, system 2 reads it); effects run inside systems at the run-site;
branching is **pool membership** (a consumer `each` over an empty pool is a no-op — that IS the conditional).

## Decisions made

- **`proc` is reserved for primitives only** — `#foreign` / `@syscall` / `@intrinsic`. Everything else:
  pure logic → a `func` (single return); effects or pool access → a `system` / `each` / `map`; a
  result-dependent sequence → decompose across systems (producer writes a pool, consumer reads it). No
  `routine` kind, no value-level `bind`, no pool-reads-inside-funcs. `func_impure` stays strict.
- **FFI pointer → bounded slice.** `rawptr` is an inert `i64` alias (no deref/index/length — the type
  system forbids it). `mem.bound(p, n) -> []char` (an `@intrinsic`) is the SINGLE audited door:
  `inttoptr` + a checked slice (reuses `codegen_slice`). Slicing a scalar/`rawptr` is a hard error
  (`E0201`, now extended to slice exprs) — so "bound is the only flow" is **structural**, not a lint.
  The unsafe ptr↔len pairing stays in the C glue (strlen / fstat), where it already lives.
- **Buffer-column idiom.** A buffer column (`[N]char`) is **filled only by an Eff out-slot**
  (`net.recv`, `io.read`, `fmt.sprintf`) and **read by slicing** (`raw[a:b]`). Never manual element
  writes (`buf[i] = …` in a query is rejected — buffers are read-only borrows there), never direct pool
  access (`Conn.col` — "never access the archetype directly"). Lengths ride in scalar columns.
- **Producer/consumer over guard-branching** — documented in `docs/patterns.md`. Route rows into the pool
  for each case; consumers process whatever is there. Replaces `if (result) { … } else { … }` chains.
- **`assert` → `fmt.fail`** (a pure func that builds the abort Eff); the condition is system control flow.
  `fmt.assert` kept but deprecated.
- **`@intrinsic` now works on funcs** (was proc-only): a bodyless pure primitive usable as a `|>`
  finalizer (`mem.bound`). `is_intrinsic` added to `HirFuncDecl`.

### Stdlib converted (all green: 870 tests + corpus -Werror + fmt idempotent)

- `os.argv` → pure func: `zip(os_argv(i), os_argv_len(i)) |> mem.bound`.
- `io.file_map` → pure func: `zip(arche_file_map(path), arche_file_size()) |> mem.bound`.
- `io.fread_line` / `io.skip_header` → **removed** (io-as-data: bulk-read + columnar scan; `io.read`/
  `read_chunk` fill a buffer column, the scan finds `\n`).
- `router.resolve` → flat-table `each` (no trie; the app owns the `Route`/request pools).
- `http.parse_request_line` → `each` writing SPANS (method/target start+len) onto each request row.
- `http.respond` → `each` over a response query `{ sock, status, ctype, body, body_len, send_body, hdr }`
  (like `gfx.circle` over `Player`); `respond_text` dissolved (the app fills columns + runs the each).
- New `mem` module: `rawptr` (datasheet) + `bound` (`@intrinsic`).

### Tooling fixes (incidental, green)

- Formatter: `|>` formats top-to-bottom (leading-operator continuation); `([` / `((` spacing quirk fixed;
  a long `each`/`system`/`map` **query-header** no longer gets an invalid trailing comma (was producing
  empty output on re-format).
- Codegen: array-column index bounds against the column WIDTH (not pool count) + re-adds the binding
  fan's row offset (`loop_idx`) so an outer-fan column read inside a nested fan indexes the outer row.

## Decisions still to make

1. **[ON HOLD] Web-server target span vs `path` buffer.** Within the buffer idiom, how the request
   target reaches `router.resolve` / `paths.build_path`: (a) **spans everywhere** — keep target as two
   scalar `int`s, router matches `raw[start:len]`, `build_path` reads from `raw`+span (changes router /
   build_path to span APIs + updates router's test); or (b) a **copy-into-buffer Eff** (`str.copy(slice)
   -> Eff([]char,int)` filling a buffer via out-slot) so router/build_path keep their `path` buffer API.
2. **`csv.load`** — a compile-time-reflective archetype-generic loader (`proc(arch: archetype)` +
   `#each_field`). Fits neither func (pure) nor system (no args). Either a new "archetype-targeted load
   system" feature, or decompose into per-app load systems + pure helpers (`find_byte`/`name_eq`/
   `field_at`/`cell` already funcs). Many benchmark callers. **User said: "could be a system, maybe needs
   new features."**
3. **`insert` requires every column** (no partial insert). Intended, or should unset columns default?
   Surfaced writing the web-server `boot` (a wide archetype).
4. **Full web-server rewrite** to the rpg shape (a `Conn` pipeline of systems). Blocked on (1); also needs
   a body/response representation (variable-length body in a pool column) decided.
5. **When to promote the proc lints** (`W0030 proc_not_primitive` + `func_impure` W0003 +
   `pool_index_outside_query` W0029) to error-by-default — only after ALL stdlib is proc-clean
   (csv.load/respond resolved).

## Next up

- Web-server / `bound` work (this file's topic) is on hold — see the open decisions above.
- Separately, removing `main` as an entry point is tracked in `WIP_MAIN_REMOVAL.md`.

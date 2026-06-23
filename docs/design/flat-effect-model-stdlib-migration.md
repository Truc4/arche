# Migrating the stdlib to the flat effect model — the wrapper recipe

### the mechanical, per-wrapper conversion from `proc` to `func → Eff`

> The model is [the-flat-effect-model.md](the-flat-effect-model.md) (§4 is the convenience-layer rule).
> This doc is the **how-to**: classify a stdlib wrapper, convert it, migrate its call sites. It is the
> *settled, implementable* recipe — it needs no `Result` type, no generics, no result-reshaping `fmap`,
> and no new compiler features. (The `*-web-server.md` / `*-etl.md` / `*-rpg.md` docs are whole-program
> before/afters — *what moves where* for a program; this doc is the per-wrapper conversion.)

## The one rule

> **The lib builds the raw effect; the caller cooks the raw result.**
>
> A convenience wrapper becomes `func(args…) -> Eff(<the primitive's raw out-slots>)` — a thin builder
> over one extern/`syscall`, nothing more. Any pure post-processing the old proc did (clamp `r<0`, slice
> `buf[0:n]`, timespec→ms) moves to **where the result is consumed**: a pure `func` the caller applies,
> or a line in the calling **system**. Running the `Eff` and reacting to what came back is the system's
> job, so the cook lives there — not hidden in the builder.

Why this needs nothing new:

- **Status/error is just an out-slot**, not an `Eff(Result(…))`. The caller branches on it (failure as
  data, in the proc/system). No sum type, so no generics.
- **The buffer is caller-owned.** Whatever a syscall fills through a pointer arg (`read`'s `buf`,
  `now_ms`'s timespec) is passed *in* by the caller, so the pure cook that reads it back is an ordinary
  `func` over a value the caller already holds — never a finalizer reaching inside the builder.

## Decision tree — classify the proc, then convert

1. **Single extern, no result-shaping** (`os.write`, `os.exit`, `os.close`, `os.lseek`, `net.send`,
   `net.listen/accept/connect`, `io.fwrite`, `io.fclose`).
   → `func(args…) -> Eff(<raw out-slot>)` with body `return <extern/syscall>(args…);`. **Done for the
   `os` write-family.**

2. **Result-shaping** — the proc post-processed a syscall result/buffer (`os.read`, `os.open`,
   `os.now_ms`, `os.argv`, `net.recv`, `io.read`).
   → split into a `func → Eff(raw)` (the primitive) **plus a pure `func`** for the cook; the buffer
   becomes a **caller-owned param**. The calling system does run-then-cook.

3. **Guarded / conditional effect** (`fmt.assert` runs print+exit only on failure; `os.sleep_ms` skips
   when `ms<=0`).
   → there is no no-op `Eff` to return on the skip branch, so the **condition moves to the call site**;
   the effect is a `func → Eff`. (`fmt.print`/`fmt.assert` may stay procs as thin leaves — running an
   `Eff` in a proc is legal; converting them is optional.)

4. **Result-dependent loop** (`io.fread_line`, a retry/framed read).
   → **stays a `proc`** — this is the genuine monadic case (§5). Expose the *leaf* as a `func → Eff`
   (`read1 -> Eff(char,int)`); the loop is written in the consuming proc/system. The future home is the
   `routine` construct (model §9).

## Worked example — `os.now_ms` (case 2, the one that looks hardest)

```arche
// impure: build the clock_gettime effect; the timespec is the CALLER's buffer, like read's buf
clock_get :: func(ts: []i64) -> Eff(i64) { return syscall(228, 1, ts, 0, 0, 0, 0); }
// pure: cook the filled buffer into ms — no effect, just arithmetic
ms_of     :: func(ts: []i64) -> i64 { return ts[0] * 1000 + ts[1] / 1000000; }

// caller (a system) sequences run-then-cook — the run-then-react the proc used to hide:
ts: [2]i64;  clock_get(ts)(_:);  now := ms_of(ts);
```

`os.read` / `net.recv` are the same: `read :: func(fd, buf, len) -> Eff(i64)` returns the raw count; the
calling system slices `buf[0:n]` (it owns `buf`) and branches on `n<0`. `os.argv` is two `func → Eff`
reads plus a caller-side `raw[0:n]`. `os.open` returns `Eff(i64)`; the caller casts `i32(r)` (or use the
optional same-shape `|> i32` scalar fmap).

## Call-site migration (mechanical)

Running an `Eff` is the **same surface syntax** as the old proc-with-out-slots, so most call sites are
unchanged:

- `name(args)(out:)` — **unchanged.** Binding an out-slot already runs the `Eff`.
- `name(args);` (void statement, e.g. `os.write(1, buf, n);`) — **add `(_:)`** to run it:
  `os.write(1, buf, n)(_:);`. Without it you build an `Eff` and discard it — silently no effect.
- A proc that *post-processed* a wrapper (case 2) now runs the raw `Eff` and applies the pure cook /
  slices the caller's buffer inline.

## In-out (read-write) buffers — the named out-param

A custom extern that fills a caller buffer declares it **in-out**: the same name in the in-list AND
out-list (`net_recv :: proc(s, buf: []char, n)(buf: []char, r: int)`). This is arche's "read and write the
same buffer in place" — NOT a move, copy, or vestigial echo. As a `func → Eff` the buffer's named
out-param carries through, and running the Eff binds it to the same memory the extern wrote:
```arche
recv :: func(s: socket, buf: []char) -> Eff([]char, int) { return net_recv(s, buf, buf.length); }
recv(conn, buf)(filled:, r:);  data := filled[0: r];   // `filled` IS `buf`, now filled
```
A **syscall** buffer is the same — and must be, for soundness. The generic scalar `@intrinsic syscall`
takes only i64s; a syscall that touches a buffer is a typed **`@syscall(N)`** extern that declares the
buffer's mutability, exactly like `net_recv`:
```arche
@syscall(0) sys_read :: proc(fd: i64, buf: []char, n: i64)(buf: []char, r: i64); // buf IN-OUT (written)
@syscall(1) sys_write :: proc(fd: i64, buf: []char, n: i64)(r: i64);             // buf IN  (read-only)
```
A kernel-written buffer is **in-out** (`os.read` → `Eff([]char, i64)`); the caller binds the filled buffer
(`read(fd, buf, len)(filled:, n:); filled[0:n]` — or `(_:, n:)` to read its own `buf` in place). A buffer
the kernel only reads is plain-in (`os.write`). Passing a buffer to the generic `syscall` is **rejected**
(`syscall_buffer_rejected.arche`) — that is the old read-back-through-a-read-only-borrow hole, now closed.

## What the compiler supports (all landed this migration)

- **`func → Eff` fusion**, now **recursive** — a builder may wrap another `func → Eff`
  (`io.read → fread → syscall`), and `seq` composes func-built Effs.
- **Empty `Eff()`** from a void extern, run with a bare `()`.
- **`|> fin` finalizers reshape** — a type-changing finalizer (`syscall(2,…) |> to_fd`, `i64 → fd`)
  retypes the Eff and binds the cooked type (tycheck + codegen).
- **In-out out-param wiring** — running a `func → Eff` binds an in-out out-param to its in-arg buffer
  (`net.recv`). Regression: `eff_inout_buffer.arche`.
- **`syscall` pointer args** (`move`-wrapped names, string/`[]char` consts, **slice exprs** like
  `CharBuf.cb[0:cap]`) decay with `ptrtoint`, not `sext`.
- **Typed `@syscall(N)` externs** — a buffer syscall declares its buffer in/in-out (so the kernel can't
  write a read-only borrow); the generic scalar `syscall` rejects buffer args. Fusion substitutes params
  through a cast (`i64(f)`). Regressions: `eff_syscall_inout_buffer`, `syscall_buffer_rejected`.
- Purity sees Eff-runs as effects and respects param shadowing.

Still **not** required: `Result(T,E)`, generic sums, inline-lambda finalizers, capturing finalizers.

## Status — migration done

Converted to `func → Eff`: **all of `term`, `net`** (`listen`/`accept`/`connect`/`send`/`recv`/`close`),
the **`os`/`io` write-read-open-close families** (`write`/`read`/`open`/`close`/`lseek`/`exit`/`fwrite`/
`fread`/`fopen_*`/`fclose`/`stdin_read`/`stdout_write`/`read`/`read_chunk`/`file_size`/`file_unmap`/…) and
**`fmt.print`/`print_float`**. `@drop` is now on the **extern** (`gfx_be_close`, `net_close`) — no wrapper
proc.

Genuine **stays-a-proc** exceptions (non-applicative by nature, matching the model):
- result-dependent loops — `io.fread_line`, `io.skip_header`, `csv.load`;
- stateful pool/global algorithms — `router.*`, `csv.load`;
- conditional / multi-send — `fmt.assert`, `http.respond`/`respond_text`;
- local-buffer-into-Eff would dangle — `os.now_ms`, `os.sleep_ms`;
- multi-extern compose — `os.argv`, `io.file_map`;
- pure multi-return — `http.parse_request_line` (a func returns one value).

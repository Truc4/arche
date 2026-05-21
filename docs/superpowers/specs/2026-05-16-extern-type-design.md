# Extern Type — Design

**Date:** 2026-05-16
**Status:** Draft (pre-implementation)
**Project:** Arche language

## Summary

Add `extern type` to Arche: a declaration form for opaque, fixed-capacity types whose instances are int handles into a compiler-generated runtime table. Extern types let Arche reference foreign resources (OS windows, file handles, sound sources, font atlases) without exposing pointers and without requiring per-library handle-table boilerplate in C.

This is the only mechanism by which Arche programs name C-allocated resources. Arche has no other custom types — types exist *only* for the FFI boundary.

## Motivation

Arche prohibits pointers and runtime allocation. Programs operate on archetype columns whose layout is fully visible. But real programs need to reference *external* things — OS windows, file descriptors, GPU buffers, sockets — whose lifetimes and storage live outside Arche's model.

The current convention (see `runtime/io.c`) is hand-written per-library: each C wrapper declares a `static T *table[N]`, returns `int` handles (1-based, 0 = error), and guards every entry with `if (handle < 1 || ...)`. This works but:

- Boilerplate scales linearly with libraries.
- Type safety is by convention — a window handle and a file handle are both `int`, indistinguishable to Arche's type checker.
- Each library invents its own table size, error behavior, and guarding pattern.
- Use-after-close goes undetected; the slot is silently reused and operations apply to whatever resource happens to live there now.

`extern type` formalizes the pattern as a language feature. One declaration per resource kind. Compiler-generated tables. Compiler-checked type discrimination. Runtime-checked use-after-consume.

## Design philosophy

Three principles, in priority order:

1. **No pointers exposed to Arche.** Handles are integers from Arche's perspective. The C-side pointer never crosses the FFI boundary into Arche memory.
2. **Fixed capacity at compile time.** Every extern type declares its maximum live instance count statically, mirroring `static Particle(N)`.
3. **Loud failures.** Invalid handles abort with a stderr message. No silent guards, no error codes, no exception system. (Future work: replace abort with tagged-union returns once enums exist.)

The existing `runtime/io.c` silent-guard pattern is *not* a model for this feature — it's a pragmatic patch that predates the language spec. `extern type` is the canonical replacement.

## Syntax

### Type declaration

```arche
extern type Window(8);
extern type Sound(64);
extern type FontAtlas(4);
```

Grammar addition:

```
Decl       ← ArchetypeDecl / ProcDecl / SysDecl / FuncDecl / ExternTypeDecl / ...
ExternTypeDecl ← "extern" _ "type" _ Identifier _ "(" _ Number _ ")" _ ";"
```

Capacity is required. There is no default.

### Use in extern signatures

```arche
extern func window_open(title: char[], w: int, h: int) -> Window;
extern proc window_present(w: Window, fb: int[], width: int, height: int);
extern proc window_close(consume w: Window);
```

The type name is used like any other type identifier in parameter and return positions.

### Parameter modifier: `consume`

Mark parameters whose handle is ended by the call:

```arche
extern proc window_close(consume w: Window);
```

Grammar addition (extern parameter list):

```
ExternParam ← ("out" / "consume")? _ Identifier _ ":" _ Type
```

`consume` is mutually exclusive with `out`. Multiple `consume` parameters in one signature are legal (rare, but supported).

## Semantics

### Type discipline

An `extern type T` instance is an opaque scalar. Arche programs can:

- Bind it: `let w = window_open("title", 800, 600);`
- Pass it to procs, sys, func (extern or user-defined).
- Return it from func.
- Store it in archetype columns: `arche Voice { handle: Sound, volume: float }`.
- Compare it to zero: `if w { ... }` — non-zero is valid, zero is null/failure.
- Test equality between two values of the *same* extern type: `if w1 == w2 { ... }`.

Arche programs cannot:

- Inspect its bit pattern, cast it to `int`, or print it as a number.
- Perform arithmetic on it (`w + 1` is a type error).
- Index with it (`fb[w]` is a type error).
- Mix distinct extern types (passing a `Sound` to `window_close` is a type error).
- Construct one from scratch — instances only flow out of extern calls.

### Liveness and `consume`

`consume t: T` in an extern signature ends `t`'s lifetime at the call.

**Local scope:** the compiler tracks `consume` calls within a function and rejects further uses of the consumed binding:

```arche
let w = window_open("foo", 800, 600);
window_close(consume w);
window_present(w, fb, 800, 600);  // compile error: w consumed
```

**Escaped handles:** when a handle is stored in an archetype column or returned from a function, the compiler loses static visibility. Runtime generation counters catch the stale use (see *Runtime model*).

### Null and failure

`0` is the null handle for every extern type. Extern functions returning an extern type return 0 to indicate failure. Arche code checks with `if w` (non-zero is valid, zero is null).

Passing a null handle to a non-`consume` extern function is a runtime abort. Passing a null handle to a `consume` extern function is a no-op (no slot to free, no C function called).

## Runtime model

### Handle encoding

Handles are 32-bit ints with the layout:

```
bits 31..16 : generation counter (uint16)
bits 15..0  : slot index + 1 (uint16; 0 reserved for null)
```

Encoding: `handle = (generation << 16) | (slot_index + 1)`.
The all-zeroes handle (`0`) is null and never matches any allocated slot.

### Per-type table

For each `extern type T(N)`, the compiler emits in C:

```c
#define ARCHE_T_CAPACITY N

typedef struct {
    void    *ptr;
    uint16_t gen;
    uint16_t in_use;
} __arche_T_slot;

static __arche_T_slot __arche_T_slots[ARCHE_T_CAPACITY];
```

Plus three helpers:

```c
int32_t __arche_T_alloc(void *ptr);
void   *__arche_T_get(int32_t handle);
void    __arche_T_free(int32_t handle);
```

- `alloc` finds an unused slot, stores `ptr`, returns the packed handle. Aborts if capacity exhausted.
- `get` decodes the handle, validates the slot is in use and the generation matches, returns the pointer. Aborts on any mismatch.
- `free` decodes the handle, marks the slot unused, increments its generation. Safe to call with 0 (no-op).

### Bit allocation

The 16/16 split gives 65,535 slots per type and 65,536 generation values per slot before wraparound. For all currently anticipated uses, this is ample. High-churn cases (network sockets, audio voices in a long session) may eventually want a 24-gen/8-slot or 20-gen/12-slot variant. This is **deferred** — out of scope for v1.

### Error behavior

All runtime failures call `abort()` after printing to stderr in this format:

```
arche fatal: <reason> (type=<Type> handle=0x<hex> slot=<n> gen=<n> expected=<n>)
```

Reasons:

- `null handle` — handle was 0 and call was not `consume`.
- `slot out of range` — decoded slot index ≥ capacity.
- `freed slot` — slot is not in use.
- `stale generation` — slot in use but generation mismatch.
- `capacity exhausted` — `alloc` could not find a free slot.

This is intentionally crude. Once Arche has enums or tagged unions, these become structured returns the program can pattern-match.

## C author contract

The C library author writes plain C with native C types. They do not know about handles, slots, generations, or `__arche_` helpers.

```c
// gfx_win32.c — the entire C side of a window library

#include <windows.h>

HWND window_open(const char *title, int w, int h) {
    return CreateWindowExA(0, "ArcheGFX", title, WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                           NULL, NULL, GetModuleHandleA(NULL), NULL);
}

void window_present(HWND w, int *fb, int width, int height) {
    HDC hdc = GetDC(w);
    BITMAPINFO bmi = { /* ... */ };
    StretchDIBits(hdc, 0, 0, width, height, 0, 0, width, height,
                  fb, &bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(w, hdc);
}

void window_close(HWND w) {
    DestroyWindow(w);
}
```

Contract rules:

- C function names match the Arche `extern` declaration names exactly.
- Functions returning extern types return the raw C pointer (or `NULL` for failure).
- Functions taking extern types accept the raw C pointer.
- The C author writes nothing about handle allocation, lookup, freeing, or generation tracking.
- The C author may use whatever pointer-typed value the OS API gives them (`HWND`, `FILE *`, `void *`); the compiler treats them uniformly as `void *` at the marshal boundary.

## Compiler responsibilities

For each `extern type T(N)`:

1. Reserve the type identifier `T` as opaque scalar in the type system.
2. Emit the static slot table and the three helpers (`alloc`, `get`, `free`).
3. Track distinctness: `T1` and `T2` are not assignable to each other.

For each `extern func`/`extern proc` mentioning an extern type:

1. **Return position (`-> T`):** generate a marshaler that calls the user's C function, and on non-null return, calls `__arche_T_alloc(ptr)` to wrap the result. On null return, returns 0 to Arche.
2. **Parameter position (`p: T`):** generate marshal glue that calls `__arche_T_get(handle)` to obtain the pointer, then forwards it to the C function. Abort on invalid handle.
3. **Parameter position (`consume p: T`):** same as above, then call `__arche_T_free(handle)` *after* the user's C function returns. Calling with handle 0 is a no-op (no `_get`, no C function call, no `_free`).

For each call site:

1. **Static use-after-consume tracking:** within a single function body, track which bindings have been passed to a `consume` parameter. Subsequent uses of those bindings are compile errors.
2. **Cross-function consume tracking:** out of scope. Handles that escape via archetype columns or function returns are guarded only by runtime generation counters.

## Worked example

### Arche source

```arche
extern type Window(8);

extern func window_open(title: char[], w: int, h: int) -> Window;
extern proc window_present(w: Window, fb: int[], width: int, height: int);
extern proc window_poll(w: Window);
extern func window_should_close(w: Window) -> int;
extern proc window_close(consume w: Window);

arche Frame {
    pixels: int[]
}
static Frame(1, 800 * 600);

proc main() {
    let w = window_open("Hello", 800, 600);
    if w {
        for _ in 0..1000 {
            window_poll(w);
            // ... draw into Frame.pixels in pure Arche ...
            window_present(w, Frame.pixels, 800, 600);
            if window_should_close(w) {
                break;
            }
        }
        window_close(consume w);
    }
}
```

### C source (the user writes this)

```c
// gfx_win32.c
#include <windows.h>

HWND window_open(const char *title, int w, int h) { ... }
void window_present(HWND w, int *fb, int width, int height) { ... }
void window_poll(HWND w) { /* PeekMessage drain */ }
int  window_should_close(HWND w) { /* return stored flag */ }
void window_close(HWND w) { DestroyWindow(w); }
```

### What the compiler generates (conceptually)

```c
// Compiler-generated runtime support for `extern type Window(8)`
typedef struct { void *ptr; uint16_t gen; uint16_t in_use; } __arche_Window_slot;
static __arche_Window_slot __arche_Window_slots[8];

int32_t __arche_Window_alloc(void *ptr) { /* find free slot, return packed handle */ }
void   *__arche_Window_get(int32_t h)   { /* validate, return ptr or abort */ }
void    __arche_Window_free(int32_t h)  { /* mark free, bump gen */ }

// Compiler-generated marshalers (inline in LLVM IR, but conceptually):
int32_t __arche_marshal_window_open(const char *title, int32_t w, int32_t h) {
    HWND ptr = window_open(title, w, h);
    return ptr ? __arche_Window_alloc(ptr) : 0;
}

void __arche_marshal_window_present(int32_t w_h, int32_t *fb, int32_t width, int32_t height) {
    HWND ptr = (HWND)__arche_Window_get(w_h);
    window_present(ptr, fb, width, height);
}

void __arche_marshal_window_close(int32_t w_h) {
    if (w_h == 0) return;
    HWND ptr = (HWND)__arche_Window_get(w_h);
    window_close(ptr);
    __arche_Window_free(w_h);
}
```

Arche's code generator calls the marshalers, not the raw C functions. The user's C file exports the raw names; the compiler emits marshalers that link against those names.

## Open questions / future work

These are deliberately deferred from v1:

1. **Tagged-union or enum error returns.** Replace `abort()` with a returnable error value once Arche has a sum-type construct. Until then, loud aborts are the only failure mode.
2. **Bit allocation tuning.** 16/16 split is the v1 default. High-churn extern types may want 24/8 or 20/12. Could be parameterized: `extern type Sound(64) gen=24`.
3. **Cross-function consume tracking.** Static analysis could in principle track consumption across function boundaries (via flow analysis or linearity in the type system). Out of scope for v1; runtime generation counters cover the gap.
4. **Multi-handle consume.** `consume a, b: T` syntactic sugar if it becomes common.
5. **Equality semantics.** `==` and `!=` between two handles of the same extern type compare the full 32-bit handle value. Two aliases of the same allocation (`let w1 = window_open(...); let w2 = w1;`) are equal. A stale handle and a fresh handle that happen to share the same slot index are *not* equal (their generations differ). This matches expected intuition; documented here so the implementation doesn't accidentally compare only the slot bits.
6. **Compile-time warnings for unbalanced lifecycle.** Could warn if an extern type is allocated in a `proc` and never reaches a `consume` call. Useful but not essential.
7. **Reflection / debug.** A way to enumerate live slots of a given extern type. Useful for leak hunting. Deferred.

## Non-goals

This feature explicitly does not:

- Introduce user-defined types of any other kind. Arche has no `struct`, no nominal records, no algebraic data types (yet). `extern type` is the *only* type-declaration form besides `arche`.
- Expose pointers to Arche code.
- Provide a generic FFI escape hatch beyond what's described. C authors cannot reach the slot table or generation counter directly; the contract is exactly "plain C, native types."
- Replace `extern func` / `extern proc` for value-type FFI (int, float, char, primitive arrays). Those continue to work without any handle machinery.
- Tie handle lifetime to lexical scope (no RAII, no `defer`-style auto-cleanup). Lifecycle is explicit via `consume`.

## Implementation effort estimate

Compiler changes:

- Parser: ~30 LOC (`extern type` decl, `consume` keyword in extern param lists).
- Type checker: ~80 LOC (opaque scalar, distinctness, consume tracking, no-arithmetic guard).
- Codegen: ~200 LOC (per-type table emission, marshaler emission for each extern signature).

Runtime support: ~80 LOC of static C templates the compiler instantiates per extern type.

Tests: ~10 example/regression programs covering happy path, null, stale handle, capacity exhaustion, archetype-column storage.

Total: roughly 2–4 days of focused compiler work given Arche's existing extern infrastructure.

## First consumer

The `gfx` library in `arche-libs/gfx/` is designed as the first consumer of this feature. Its full spec is a separate document. The split layering is:

- **`extern type Window(8)`** and a thin Win32 C shim providing `window_open` / `window_present` / `window_poll` / `window_close`.
- **Everything else** — shape rasterization, color packing, framebuffer math — written in Arche, operating on a caller-owned `int[w*h]` column.

This layering uses `extern type` exactly once (for `Window`), validating that the feature solves the real problem with minimal C surface.

# String-literal variable passed to a `char[]` parameter

Date: 2026-06-01
Status: ✅ implemented

## Problem

A variable bound to a string literal is stored as a bare `i8*` (type 2):

```arche
x := "hello";        // x : i8* pointing at the string constant
```

Passing `x` to a proc whose parameter is `char[]` (lowered as `%struct.arche_array*`) crashed at
runtime ("stack overflow"):

```arche
f :: proc(s: char[])(r: int) { r = strlen(s); }
f(x)(r:);            // CRASH
```

The call site passed the raw `i8*` **as** an `arche_array*`. The callee then read the first
24 bytes of the string (`"hello\0…"`) as the `{ptr, len, cap}` struct, loaded a garbage data
pointer from field 0, and walked it in `strlen` until it faulted.

Only this one form broke. A string literal passed **directly** (`f("hello")`) was wrapped in a
stack `arche_array` correctly, and a `char[N]` **buffer** variable (type 7) was wrapped correctly.
The gap was specifically a type-2 string *variable* reaching a non-extern `char[]` parameter.

## Fix (`codegen/codegen.c`)

- **Call-arg marshalling** (the `arg_is_string` branch, `callee_wants_arr` case): a type-2 string
  variable bound for a non-extern `char[]` parameter is now wrapped in a stack `arche_array`
  (`store` the `i8*` into field 0, length into fields 1/2), mirroring the string-literal and
  type-6-forwarding paths — instead of passing the bare pointer as a struct.
- **Binding** (`x := "literal"`): the literal's length is now recorded in the variable's
  `string_len` (was hard-coded `-1`), so the wrap above stores a real length. Length is unused by
  `strlen` (it scans to NUL), but correct for any callee that reads the length field.

## Test

`tests/unit/language/strings/string_var_to_char_param.arche` — literal-direct, literal-bound
variable, and `char[N]` buffer all passed to the same `char[]` param; asserts all three lengths.

## Found via

Building `stdlib/router/router.arche`: the router test bound paths to locals
(`up := "/users/42"`) and passed them to `route_resolve(path: char[])`, which crashed until this
fix.

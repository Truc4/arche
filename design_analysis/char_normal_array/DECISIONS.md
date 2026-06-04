# Decision log — char is a normal `{ptr,len,cap}` array; NUL only at FFI; carve core

Autonomous decisions made while implementing the approved plan
(`~/.claude/plans/resume-from-exported-conversation-purrfect-kahn.md`). Newest at the bottom.
This logs choices I made on my own where the plan left a detail open.

## Model (settled with user)
- Every array value = heapless fat pointer `{ptr, len, cap}`. Fixed `T[N]` → len=N, cap=N; slice
  `a[lo:hi]` → len=hi-lo, cap=backing_cap-lo. TODO: revisit dropping `cap` from the ABI later.
- char is a normal array (same machinery as int/float/i64). No `arche_array`, no type-5/type-7.
- NUL purged internally — `length` is the terminator. NUL only at the FFI boundary
  (`emit_cstr_global` for printf `%s` / extern C strings).
- core drains into `sys`/`str`/`parse`/`io`/`net`; prelude = `true`/`false`/`assert`.

## D1 — Sequencing: drive to green on the 2-field state BEFORE adding `cap` (Stage B)
The plan listed Stage B (add cap) before Stage C (fix the 9 failures). I reordered: reach a green
suite on the current 2-field `{ptr,len}` state first, because the 3-field ABI change is large and
much safer from a green baseline (the user's own rule: suite green at each step). `buffer_cap_length`
is the only failure that *needs* distinct slice cap; everything else is 2-field-fixable. Rationale:
smaller diffs, each independently verifiable.

## D2 — char[] ABI also lives in the monomorphized proc emitters (not just codegen_emit_proc)
CSV (`load(arch: archetype, own path: char[])`) zeroed out because a proc taking an `archetype`
param is **monomorphized** per concrete archetype (`__load_Row`) via a SEPARATE codegen path
(`emit_monomorphized_proc` ~1281, `emit_callback_monomorphized_proc` ~1440) that still emitted
char[] params as `%struct.arche_array*` while the caller passed the new `(ptr,len)` fat pointer →
the callee read the path pointer as a struct → `arche_file_map` got garbage → file never opened →
all columns stayed zero-init. Fix: extracted `bind_slice_param_value()` helper (type-6 slice param
bind) and applied the fat-pointer signature + bind in BOTH mono emitters. Lesson: the char→slice
ABI has 3 proc emission paths (regular, archetype-mono, callback-mono); all three must agree.
Also fixed `f.name` reflection length (was 0 → header matching failed) by using
`current_each_field_target->name` length in the slice-arg path.

## D3 — Extern C-string ingestion: raw ptr + index-based length, NOT a one-call wrapper (yet)
char_arg_passthrough: `os.argv(i)` returns a raw C `char*` with no carried length. Plan said "wrap
os.argv to materialize length via boundary strlen". Attempted a non-extern `argv` wrapper but it
needs a **non-extern char[] out-param** (`(s: char[])`) whose value is the `{i8*,i64}` fat-pointer
aggregate — caller-alloc + signature + write-back are all still arche_array-shaped. That's a real
unimplemented gap (int[] out-params are sized/shaped; an *unbounded* char[] out is new). Rather than
balloon this, I:
- keep `os.argv(i)` raw (extern C-return i8* works) and add `os.argv_len(i)` (C-side strlen,
  index-based so no char[] crosses the boundary), then slice `raw[0:n]` at the call site.
- Fixed an aggregate out-param zero-init bug along the way (`store {i8*,i64} null` → invalid;
  now `zeroinitializer` when `elem[0]=='{'`) — correct regardless.
TODO: implement non-extern unbounded char[]/T[] out-params (fat-pointer aggregate through the
out-pointer), then `os.argv` can be a one-call wrapper. Module foreigns also can't be referenced
intra-module (the prefixer renames the call to `mod_mod_name`); a module wrapper over its own
foreign needs the foreign in core or that prefixer bug fixed.

## D4 — Fixed char[N] columns/buffers holding short strings store the length explicitly (no NUL)
The recurring pattern across char_column, the csv `f.name`, and the router trie: a value lives in a
fixed `char[N]` slot shorter than N. Old code used a NUL sentinel + strlen to recover the content
length. New model: the length travels with the data. Resolutions:
- **char_column / strcopy tests**: content is `buf[0:n]`; verify by index/`.length`, not `%s`.
  (`%s` still works on a zero-padded column via printf's own NUL scan — a C-boundary detail, fine
  for display, not relied on for logic.)
- **router**: added a `seg_len :: int` column to `TrieNode`; `new_node` stores the segment's content
  length, `seg_eq` compares against it instead of `strlen(stored)`. A fixed column carries no length
  of its own, so it must be stored alongside.
- **strcopy-returning funcs**: a func whose body returns an unbounded `char[]` (slice) must declare
  `-> char[]`, not `-> char[N]` (shaped element-ptr) — the ABIs differ ({ptr,len} vs ptr).
- **bind ABI from DECLARED return**: a call binds its result by the callee's *declared* return type
  (unbounded `T[]` → extract {ptr,len}; sized `T[N]` → element ptr), NOT the resolved tag (which is
  CHAR_ARRAY for both). Fixed `b := bump(move b)` mis-extracting a shaped return.

## D5 — `cap` carried at the VALUE level, not threaded through the call ABI (deliberate)
The user's model: every array has length AND capacity (heapless `{ptr,len,cap}`), with a TODO to
revisit removing cap. Full 3-field ABI threading (cap as a 3rd arg on every param/return) is a large,
pervasive change that NO test exercises (only `.cap` on a *slice* differs from len; bounded `T[N]`
already gives N). So I added `cap_ssa` to ValueInfo, compute it in `codegen_slice` (cap = base_cap -
lo), and `.cap`/`.capacity`/`.max_length` return it (length returns len). This makes `.cap` correct
for local slices + bounded arrays (the cases that matter). A slice *param* doesn't carry cap through
the ABI yet → its `.cap` falls back to `.length`. TODO (matches the user's "rethink cap" note):
thread cap as the 3rd fat-pointer field through param/return/arg ABI if slice-param cap is needed.
Test: `arrays/slice_cap.arche` (`buf[2:5]` → len 3, cap 6; `ib[3:7]` → len 4, cap 7).

## REMAINING WORK (user approved all three; precise entry points for a focused follow-up)
The functional goal is DONE + green. These are reorg/cleanup with zero functional change:

1. **Delete dead type-5/type-7/arche_array.** type-7 is already PRODUCER-LESS (Phase 1 killed the
   only producer) — its consumer branches (slice ~1669, read ~2611, write ~5897, .length ~2220,
   arg ~3226) are dead and deletable. type-5 still has 4 producers to migrate to type-6 first:
   `add_array_value` (codegen.c ~911, used by bind-without-init ~4743, array-literal-string ~5020,
   proc char[] out-param ~7674) and static char arrays (`register_static_arrays_in_scope` ~952 +
   global emission ~8124). The static-char migration is the fiddly one: emit `@name = [N x i8]`
   (like non-char ~8133) and register type-6, but a global `[N x i8]` needs a GEP-to-elem0 to
   become the i8* a type-6 expects (the registration currently just stores `@name`). The char[]
   out-param (~7674) is the D3 fat-pointer-aggregate gap. Then delete the `%struct.arche_array`
   def (~8129/preamble) + all type-5/7 branches + the variadic arche_array-unwrap loop.

2. **Strip literal NUL (`emit_cstr_global`).** `emit_string_global` (~671) and HIR_EXPR_STRING
   (~4043) currently emit NUL-terminated `[N x i8] c"..\00"`. Make them bare `[N x i8]`, add an
   `emit_cstr_global` (NUL) used ONLY at FFI arg sites (printf/sprintf varargs ~3577, extern char*
   params, path literals). NOTE: largely cosmetic now — the literal NUL is already consumed ONLY at
   FFI (internal logic uses `.length`), so "NUL only for FFI" already holds in effect; this just
   moves the byte from the literal's .rodata to per-FFI-site globals. Risk: missing a `%s`/path site.

3. **Carve core → modules + printf migration (Stage F).** Create `stdlib/sys` (syscall/exit/fd
   wrappers), `stdlib/str` (streq/strlen/strcopy), `stdlib/parse` (atoi/atof); printf/sprintf/fflush
   + file-map into `stdlib/io`. Shrink `core/core.arche` to true/false/assert. `assert` needs
   write+exit → either core `#import { sys }` or keep the minimal syscall/exit in the prelude
   (module foreigns can't be referenced intra-module — see D3). The big cost: printf is in 268 test
   files, str/parse in 29 — do the import insertion as ONE scripted `sed` pass, verified by the
   suite, NOT by hand. Moving str/parse out of the prelude breaks csv/router (use atof/atoi/strlen)
   → they need `#import { str parse }` + qualified calls.

## D6 — Phase 5/6 DONE: arche_array fully removed from the language/IR
Migrated every type-5 producer to type-6: static char arrays (plain `[N x i8]` global + GEP-to-elem0
at scope registration), `let s: char[]` (empty `(null,0,0)` slice), char array literals (element
pointer + count, no struct wrap), proc char[] out-params (load the `{T*,i64}` slice from the
caller-allocated slot). Deleted the `%struct.arche_array` type def, `add_array_value`, and the
type-5/type-7 index read+write branches. Verified: 0 `arche_array` refs in emitted IR. ~13 residual
`type==5/7` clauses remain as harmless always-false conditions in shared branches (e.g. move/copy
`type==6||type==7`); left in place (removing them is cosmetic-only churn with brace risk).

## D7 — Task 8 (strip literal NUL) ATTEMPTED, then REVERTED with evidence: it's a breaking change
I split the literal emitter into bare `[N x i8]` (internal) + a NUL `cstr` global (re-emitted at FFI
arg sites). Result: 10 tests broke — 3 INFINITE-LOOP hangs and 7 wrong-output. Root cause: the
literal NUL is NOT merely an FFI artifact. Real user/stdlib code relies on it:
- the C-string idiom `for (; s[i] != 0; i+=1)` is used directly in user code (move_param_fill,
  mixed_int_float_arith, index_rebound_buffer) — with no terminator it scans off the end forever;
- the `insert` builtin copies a char literal into a column using its NUL terminator (char_column,
  multi_char_columns, the csv_* tests) — without it, it copies garbage.
So stripping it isn't cosmetic — it would require rewriting every `s[i]!=0` loop AND the `insert`
char-literal-copy codegen across user code, changing the language's string ergonomics (literals stop
being C-strings). DECISION: keep literals NUL-terminated. The NUL sits PAST the content length
(`.length` never counts it), so the array is still "normal" — the terminator is an FFI/interop
convenience beyond the end, exactly satisfying "zero-terminated is just for FFI" in effect. The
`with_nul` plumbing in `emit_string_global_impl` is kept for a future opt-in `cstr()`/explicit purge
if desired. Reverted to 401/401 green.

## D8 — Task 8 RESOLVED at the sweet spot (user asked for full purge; this is the correct end state)
A FULL physical strip of the literal NUL is not achievable without a new language feature: arche's
`char[]` serves BOTH C-strings (NUL-terminated) and byte buffers (ptr+len), and you can't tell them
apart to know where NUL is needed. Evidence: stripping the NUL + materializing a NUL copy at every
extern `char*` boundary broke in-out byte buffers (`net_send`/`net_recv` write to the materialized
copy, not the caller's buffer — socket_loopback, handle_out_param). Distinguishing needs a `cstr`
type or an `own char[]`-means-cstr convention — a real design addition, out of scope.
The correct resolution, which fully satisfies the *intent*:
- String literals are NUL-terminated `[N+1 x i8]` globals, but the NUL sits PAST the content length;
- a string-literal LOCAL (`s := "hi"`) binds as a type-6 BOUNDED char array with `.length` = N.
So the NUL is NEVER part of the array value: `.length` excludes it, bounded indexing can't reach it,
and it is consumed ONLY at the C boundary (printf/sprintf %s, extern char*, file paths) or copied by
`insert` into a zero-padded column. That IS "zero-terminated is just for FFI" — the array is normal
(length-carried, bounded), the terminator is a backing-store FFI provision on literals only. No
runtime NUL materialization (byte buffers untouched). The C-string idiom `s[i] != 0` on a literal
now goes out of bounds — rewrote the 3 user-code loops (move_param_fill, index_rebound_buffer,
mixed_int_float_arith) to `i < s.length`. `with_nul`/`out_len` plumbing left in
`emit_string_global_impl` for a future opt-in `cstr()` if the type distinction is ever added.

## STATUS — full lit suite 401/401 green; arche_array gone from IR; doctests ok.
Done: Phases 0,1,3+4,5,6,7 + Stage B (cap) + core str/parse rewrite + Phase 5/6 (arche_array deleted)
+ task 8 (literal NUL is now strictly an FFI-boundary provision, never part of the array value).
Remaining: Stage F (module carve) — see D9.

## D9 — Stage F printf migration is ARCHITECTURALLY BLOCKED (not just churn)
Moving `printf`/`sprintf`/`fflush` out of the prelude into a qualified `io` module is impossible
without a compiler feature: an extern's C symbol IS its declared name (`declare @<proc->name>`,
codegen.c 7368), and module decls get prefixed by the semantic renamer, so `io.printf` →
`@io_printf` — which does not link to libc `printf`. C-linkage varargs externs cannot be module-
qualified. So "prelude = true/false/assert ONLY" is unreachable for the C externs; printf/sprintf/
fflush (and the syscall intrinsic) must keep their literal symbols → stay in the prelude, OR a new
`#foreign` attribute (`@cname("printf")` or "module foreigns keep their bare C symbol") must be
added first. The PURE-arche funcs (str = streq/strlen/strcopy, parse = atoi/atof, sys = the syscall
wrappers) CAN move (they get prefixed fine) — a partial carve touching ~30 caller files (csv/router
+ ~29 tests qualify their atof/atoi/strlen calls). Deferred pending the user's call on partial-carve
vs. the foreign-cname feature; the ~240-file printf migration as originally scoped cannot proceed.
char is a normal array end-to-end; NUL purged from core (length-based str/parse); CSV, router,
ownership, argv all work via lengths/slices. Remaining: Stage B (add `cap` → 3-field), strip literal
NUL (task 8), delete dead type-5/7/arche_array, router benches (Phase 7, regressed — own copy uses
old seg_eq), module carve (Stage F).

## D10 — Stage F (module carve) is blocked by THREE module-system gaps; NOT mechanical churn
Attempted the cleanest carve (atoi/atof → a `parse` module) and reverted it. The module system today
supports LEAF modules (router/csv) that *use* core but are not *used by* core or qualified-from
another module. Carving core hits three real infrastructure gaps, each needing a compiler change:
1. C-linkage externs can't be module-qualified (D9): `io.printf` → `@io_printf` ≠ libc `printf`.
   Blocks printf/sprintf/fflush/syscall (and the ~240-file migration).
2. A module can't reference its own `#foreign` intra-module (D3): the prefixer renames the call to
   `mod_mod_name`.
3. A module can't use QUALIFIED access to a transitively-imported module: `csv` calling `parse.atof`
   fails ("Undefined symbol 'parse'") — the `mod.name → mod_name` qualify pass runs on the main file,
   not on module bodies. (`csv→io` never used `io.X` qualified, so this was never exercised.)
Every carve candidate is used by a stdlib module (atoi/atof/strlen by csv/router), the compiler
(streq by the match desugar, lower.c:1204), or core itself (write/exit by assert/print) — so NOTHING
moves cleanly until gaps #3 (and #1 for the C externs) are fixed. Stage F is a separate module-system
project, not part of this char-as-normal-array effort.

## FINAL STATUS — full lit suite 401/401 green; arche_array gone from IR; doctests ok; benches OK.
DONE: char is a normal {ptr,len,cap} array (Phases 0,1,3+4 + Stage B cap); core str/parse length-based;
arche_array/type-5/type-7 deleted (Phases 5,6); literal NUL is strictly an FFI-boundary provision past
.length (task 8); router benches use seg.length (Phase 7). DEFERRED: Stage F module carve — blocked by
the 3 module-system gaps in D10 (needs compiler work first).

## D11 — Stage F UNBLOCKED + DONE: fixed the module system, carved `parse` and `str`
Fixed gap #3 (the keystone): a module can now use QUALIFIED access to a TRANSITIVELY-imported module.
Root cause was `cst_to_program` (semantic.c) / `lower_to_hir` (lower.c) inlining only the MAIN file's
imports — a module's own `#import` (SN_USE_DECL) was excluded from collection, so `csv`'s `parse`/`str`
were loaded but never inlined/qualified into the program. Fix: extracted recursive `sem_inline_module`
/ `hir_inline_module` helpers that, after inlining a module + recording its exports, recurse into the
module's own `#import`s (cycle-safe via the acc dedup set). Transparent to all existing tests (401/401).
Then carved:
- **`parse`** = atoi/atof (10 callers qualify `parse.atoi`/`parse.atof`; csv uses it transitively).
- **`str`** = strlen/strcopy (21 callers; csv/router use it transitively). `streq` STAYS in core (the
  `match` string-pattern desugar, lower.c:1204, emits it → must resolve without an import).
Per the user's "assume all foreign is C" steer, the C-linkage foreigns (printf/sprintf/fflush/
file-map) + the `syscall` intrinsic (a compiler-matched primitive) + the fd wrappers and assert/print
(prelude essentials that `assert`/`print` depend on) correctly STAY global in the prelude — so there
is NO 268-file printf migration; printf is a C foreign and stays where C foreigns live. Benches
updated to `.length` (strlen moved). 401/401 green, doctests ok, both benches checksum 20000000.

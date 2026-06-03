# `.cap`/`.length` on fixed buffers + const-before-comment value corruption

Date: 2026-06-01
Status: ✅ implemented

Two small compiler fixes found while de-magicking the web server (`arche-web-server/src/main.arche`)
and adding `true`/`false` to core.

## 1. `.cap`/`.length`/`.capacity`/`.max_length` on a fixed `char[N]`/`T[N]` buffer

**Want:** derive sizes from the buffer itself (`net_recv(conn, req, req.cap)`) instead of repeating
the literal `8192`. The size is *already tracked* — it's the `string_len` (declared N) that `buf[i]`
bounds-checks against — but the field accessor wired `.length`/`.max_length`/`.capacity` only for
archetypes (instances / `arch.capacity` param) and `arche_array`, not for a `char[N]` stack buffer
(type 7). Const-sized arrays aren't an alternative (`ArrayType` size is a Number literal only).

**Fix:**
- `codegen/codegen.c` HIR_EXPR_FIELD: added a `base_val->type == 7` branch emitting the compile-time
  constant `string_len` for `cap`/`capacity`/`length`/`max_length` (a fixed array has no dynamic
  length, so all four are N; content length stays `strlen`). `cap` is accepted as an alias of
  `capacity`.
- `semantic/semantic.c`: `.length`/`.max_length`/`.cap`/`.capacity` type as `int`; the
  shaped-array/array field gate now allows these metadata fields before its no-field error.
- Test: `tests/unit/language/types/buffer_cap_length.arche`.

## 2. A top-level const before a line comment had its value corrupted

**Symptom:** `B :: 0` immediately followed by a line comment, then any declaration, miscompiled —
the value read as float ("double 0") and leaked the comment text into codegen
(`opt: integer constant must have integer type`). No comment between const and next decl → fine.
Triggered by adding the first core consts (`true`/`false`), which sit right before the `// Raw
Linux…` comment + `#foreign` block.

**Cause:** a CST node's `length` runs to the next token, so it includes a trailing comment leaf.
Both CST→AST builders dup the literal's *node span* (`cv_dup`/`sem_cv_dup`) for its lexeme, swallowing
the comment; `strchr(lexeme, '.')` then sees a `.` in the comment and types the literal float.

**Fix:** take the literal from its first **token** leaf, not the node span — `cv_dup_first_token`
(`lower/lower.c`) and `sem_cv_dup_first_token` (`semantic/semantic.c`), used in each builder's
`SN_LITERAL_EXPR` case. Token offsets/lengths are exact, so trailing trivia is excluded.
- Test: `tests/unit/language/types/const_before_comment.arche` (const followed by a comment, then a
  const / `#foreign` / proc; values + `true`/`false` round-trip).

Both confirmed: full unit suite 346/346 green; web server's 13 integration tests pass after the
de-magic refactor (`req.cap`, `body.cap`, `method.cap - 1`, `target.cap - 1`, `true`/`false`).

# Complete Implementation Plan: Whole-Column Array Operations

## Context

Implementing core "array-first" design: operations on archetype columns work across entire collections without explicit loops. Allocate once, fixed size, no resizing.

`alloc Particle(1000)` creates 1000 slots with `count = capacity = 1000`. All slots are live immediately. Array ops iterate `count` elements.

---

## Files Modified

| File | Changes |
|---|---|
| `README.md` | Remove meta/col, update examples to match actual syntax |
| `docs/GRAMMAR.peg` | Fix ArchField (no meta/col prefix), fix Allocation rule |
| `codegen/codegen.c` | `CodegenContext` struct, `EXPR_FIELD`, `EXPR_NAME`, `STMT_ASSIGN`, new helper |
| `tests/data/test_soa.arche` | Rewrite to use Particle, non-zero alloc, array ops |

---

## Phase 0 — Docs, Examples, Tests ✅ COMPLETED

### Step 0a — Doc fixes ✅

**`docs/GRAMMAR.peg`**:
- Removed `("meta" / "col")` prefix from ArchField rule
- Updated Allocation rule syntax to keyword form: `alloc Identifier "(" Expression ")"`
- Made `in World` optional in RunSystem

**`README.md`**:
- Removed all `meta`/`col` language
- Updated archetype examples to bare fields
- Updated array-op examples to show `p.pos = p.pos + p.vel` (handle) and `pos = pos + vel` (inside sys)
- Added section on tuple columns with labeled syntax
- Clarified fixed allocation model (no dynamic resizing)

### Step 0b — Fix examples/ ✅

All example files updated:
- `examples/simple/simple.arche` — let bindings + output
- `examples/hello_world/hello_world.arche` — string literal + write
- `examples/with_params/with_params.arche` — proc with params
- `examples/simple_with_print/simple_with_print.arche` — archetype alloc
- `examples/archetype/test_archetype.arche` — **uses default world** (run move; run dampen;)
- `examples/archetype/test_archetype_verbose.arche` — systems with scalar fields
- `examples/archetype/multidim_example.arche` — multi-dimensional arrays

All use lowercase primitives, labeled tuple syntax, and output via write().

### Step 0c — Write TDD tests ✅

Created test data files:

**`tests/data/test_array_ops.arche`** — outside-sys array op:
```arche
arche Particle {
  pos: float,
  vel: float
}

proc main() {
  let p = alloc Particle(1000);
  p.pos = p.pos + p.vel;
  let msg = "array ops done\n";
  write(1, msg, msg.length);
}
```

**`tests/data/test_sys_ops.arche`** — sys body without explicit for:
```arche
arche Body {
  position: float,
  velocity: float
}

sys move(position, velocity) {
  position = position + velocity;
}

proc main() {
  let b = alloc Body(500);
  run move;
  let msg = "system executed\n";
  write(1, msg, msg.length);
}
```

**`tests/data/test_soa.arche`** — combined array ops + system:
```arche
arche Particle {
  pos: float,
  vel: float
}

sys move(pos, vel) {
  pos = pos + vel;
}

proc main() {
  let p = alloc Particle(1000);
  p.pos = p.pos + p.vel;
  run move;
  let msg = "test passed\n";
  write(1, msg, msg.length);
}
```

### C Reference Implementations ✅

Created matching C implementations for all examples to establish test baseline:
- `examples/hello_world/hello_world.c`
- `examples/simple/simple.c`
- `examples/with_params/with_params.c`
- `examples/simple_with_print/simple_with_print.c`
- `examples/archetype/test_archetype.c`
- `examples/archetype/test_archetype_verbose.c`
- `examples/archetype/multidim_example.c`
- `tests/data/test_array_ops.c`
- `tests/data/test_sys_ops.c`
- `tests/data/test_soa.c`

All compile and run successfully, producing expected output.

### Test Harness ✅

Created `tests/run_example_tests.sh`:
- Compiles C reference implementations
- Compares output with Arche implementations (once .length is implemented)
- Color-coded PASS/FAIL/SKIP output
- Stores intermediates in `tests/tmp/`

---

## Phase 1 — Implementation

All examples use `msg.length` syntax for array length queries. Parser/semantic/codegen must be implemented to support this.

### Step 1: Parser & Semantic Support for `.length`

**Status:** ✅ COMPLETED

Must support array property access syntax:
```arche
let msg = "Hello, World!\n";
write(1, msg, msg.length);  /* msg.length evaluates to string length */
```

Parser already supports EXPR_FIELD for struct field access. Extend to handle `.length` on array types.

Semantic analysis must:
- Recognize `.length` on array-typed expressions
- Return `int` type for length queries
- Evaluate length at compile time for literals, or generate runtime length load for allocations

Codegen must:
- For string literals: compile-time constant (length of lexeme)
- For archetype columns: load `count` field from archetype struct
- For other arrays: track array allocation size (future work)

---

### Step 2: CodegenContext — Add implicit loop index

**Status:** ✅ COMPLETED

**File:** `codegen/codegen.c`

Added to `struct CodegenContext`:
```c
char implicit_loop_index[64];  /* SSA reg name for current implicit loop ("" = not in loop) */
```

Initialize to empty string `""` in `codegen_create()`.

**Purpose:** Track current loop variable when inside whole-column loop, enabling auto-indexed element loads.

---

### Step 3: ValueInfo — Add field_type for type-4 parameters

**Status:** ✅ COMPLETED

**File:** `codegen/codegen.c`

Added to `struct ValueInfo`:
```c
char *field_type;  /* Arche type name for type-4 column pointer, e.g. "float" */
```

In `codegen_sys_decl()` where type-4 entries are added to scope, after `add_value()` call:
```c
ValueInfo *vi = find_value(ctx, param_name);
if (vi) vi->field_type = field->type->data.name;
```

**Purpose:** Store element type for sys parameters so Step 5 can determine correct LLVM type for element loads.

---

### Step 4: EXPR_FIELD — Element load inside implicit loop

**Status:** ✅ COMPLETED

**File:** `codegen/codegen.c` — `case EXPR_FIELD` (~line 478)

In `FIELD_COLUMN` branch, after loading column pointer into `ptr_val`, before `strcpy(result_buf, ptr_val)`:

Loads element with GEP to scalar position, then bitcasts pointer to vector type if `vector_lanes > 0`:

```c
if (ctx->implicit_loop_index[0]) {
    const char *load_type = elem_llvm_type(ctx, fdecl->type->data.name);
    char *idx_gep = gen_value_name(ctx);
    buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n",
        idx_gep, llvm_type, llvm_type, ptr_val, ctx->implicit_loop_index);
    char *elem = gen_value_name(ctx);
    int align = ctx->vector_lanes > 0 ? 8 : 4;

    if (ctx->vector_lanes > 0) {
        char *vec_ptr = gen_value_name(ctx);
        buffer_append_fmt(ctx, "  %s = bitcast %s* %s to %s*\n", vec_ptr, llvm_type, idx_gep, load_type);
        buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", elem, load_type, load_type, vec_ptr, align);
    } else {
        buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", elem, load_type, llvm_type, idx_gep, align);
    }
    strcpy(result_buf, elem);
} else {
    strcpy(result_buf, ptr_val);
}
```

**Purpose:** When referencing `p.pos` inside a whole-column loop, auto-index by loop variable instead of returning raw pointer.

---

### Step 5: EXPR_NAME — Element load for type-4 in implicit loop

**Status:** ✅ COMPLETED

**File:** `codegen/codegen.c` — `case EXPR_NAME`

After resolving a type-4 `ValueInfo`:

Loads element with GEP to scalar position, then bitcasts pointer to vector type if `vector_lanes > 0`:

```c
if (ctx->implicit_loop_index[0] && val->type == 4) {
    const char *arche_type = val->field_type ? val->field_type : "float";
    const char *scalar_type = llvm_type_from_arche(arche_type);
    const char *load_type = elem_llvm_type(ctx, arche_type);
    char *idx_gep = gen_value_name(ctx);
    buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n",
        idx_gep, scalar_type, scalar_type, val->llvm_name, ctx->implicit_loop_index);
    char *elem = gen_value_name(ctx);
    int align = ctx->vector_lanes > 0 ? 8 : 4;

    if (ctx->vector_lanes > 0) {
        char *vec_ptr = gen_value_name(ctx);
        buffer_append_fmt(ctx, "  %s = bitcast %s* %s to %s*\n", vec_ptr, scalar_type, idx_gep, load_type);
        buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", elem, load_type, load_type, vec_ptr, align);
    } else {
        buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", elem, load_type, scalar_type, idx_gep, align);
    }
    strcpy(result_buf, elem);
    break;
}
```

**Purpose:** When referencing `pos` (sys parameter) inside whole-column loop, auto-index by loop variable.

---

### Step 6: emit_whole_column_loop helper

**Status:** ✅ COMPLETED

**File:** `codegen/codegen.c` — Added before `codegen_statement()`

Strip-mined SIMD loop. All labels get unique `_N` suffix (from `gen_value_name()` counter) to avoid conflicts.

**Signature:**
```c
static void emit_whole_column_loop(CodegenContext *ctx,
    const char *col_ptr,     /* SSA reg: double* column data */
    const char *count,       /* SSA reg: i64 element count */
    const char *scalar_type, /* "double" or "i32" */
    const char *arche_type,  /* "float" etc. */
    Expression *rhs,         /* RHS — evaluated inside loop with implicit index set */
    int op);                 /* AssignOp: OP_NONE = store, OP_ADD/SUB/MUL/DIV = load+op+store */
```

**Loop structure:**

```llvm
count_aligned = and i64 count, -4

; Vector loop setup
v_ctr = alloca i64
store 0, i64* v_ctr
br %vec_loop

vec_loop:
  vi = load i64, i64* v_ctr
  vcond = icmp slt i64 vi, count_aligned
  br i1 vcond, label %vec_body, label %scalar_loop

vec_body:
  ctx->vector_lanes = 4
  ctx->implicit_loop_index = vi_reg
  rhs_val = codegen_expression(rhs)  ← auto-indexed via Steps 4/5
  [if compound op: load col[vi] as <4xdouble>, apply fadd/fsub/etc]
  target_gep = getelementptr double, double* col_ptr, i64 vi
  store <4xdouble> rhs_val, <4xdouble>* target_gep, align 8
  vi_new = add i64 vi, 4
  store vi_new, i64* v_ctr
  br %vec_loop

; Scalar loop setup
scalar_loop:
  s_ctr = alloca i64
  store count_aligned, i64* s_ctr
  br %scalar_check

scalar_check:
  si = load i64, i64* s_ctr
  scond = icmp slt i64 si, count
  br i1 scond, label %scalar_body, label %done

scalar_body:
  ctx->vector_lanes = 0
  ctx->implicit_loop_index = si_reg
  rhs_val = codegen_expression(rhs)
  [if compound op: load col[si] scalar, apply op]
  target_gep = getelementptr double, double* col_ptr, i64 si
  store double rhs_val, double* target_gep, align 4
  si_new = add i64 si, 1
  store si_new, i64* s_ctr
  br %scalar_check

done:
  ctx->implicit_loop_index[0] = '\0'
  ctx->vector_lanes = 0
```

**Purpose:** Generate vectorized + scalar-tail loop for whole-column operations without explicit source-level for loops.

**Implementation note:** Currently runs scalar-only (vector_lanes = 0). Loop structure is in place but SIMD operations not yet enabled. Store operations include bitcast for vector type compatibility.

---

### Step 7: STMT_ASSIGN — Whole-column dispatch

**Status:** ✅ COMPLETED

**File:** `codegen/codegen.c` — `case STMT_ASSIGN` (~line 1075)

Two dispatch paths:

#### Path A: `p.pos = p.pos + p.vel` — target is EXPR_FIELD of FIELD_COLUMN

```c
} else if (target->type == EXPR_FIELD &&
           target->data.field.base->type == EXPR_NAME) {
    ValueInfo *inst = find_value(ctx, target->data.field.base->data.name.name);
    if (inst && inst->type == 3 && inst->arch_name) {
        ArchetypeDecl *arch = find_archetype_decl(ctx, inst->arch_name);
        const char *fname = target->data.field.field_name;
        /* find fdecl by name, verify FIELD_COLUMN */
        /* GEP + load col_ptr from struct at field_idx */
        /* GEP + load count from struct at field_count_idx */
        /* emit_whole_column_loop(ctx, col_ptr, count, scalar_type, arche_type, rhs, op) */
    }
}
```

#### Path B: `pos = pos + vel` in sys — target is EXPR_NAME, type-4

```c
if (val && val->type == 4) {
    const char *arche_type = val->field_type ? val->field_type : "float";
    const char *scalar_type = llvm_type_from_arche(arche_type);
    /* load count from %archetype: GEP at field_count index using val->arch_name */
    /* emit_whole_column_loop(ctx, val->llvm_name, count, scalar_type, arche_type, rhs, op) */
}
```

**Purpose:** Dispatch assignment operations to emit whole-column loops instead of scalar assignments.

---

## Verification Tests

**Status:** ✅ PHASE 1 COMPLETE (Apr 16, 2026)

All codegen tests passing (6/6):
- `examples/simple/simple.arche` ✓
- `examples/hello_world/hello_world.arche` ✓ — `.length` property on string literals
- `examples/with_params/with_params.arche` ✓
- `examples/archetype/test_archetype.arche` ✓ — multiple archetypes + systems
- `examples/archetype/test_archetype_verbose.arche` ✓ — world declaration
- `examples/simple_with_print/simple_with_print.arche` ✓

Integration tests verified:
- `tests/data/test_array_ops.arche` — whole-column array ops (Path A): `p.pos = p.pos + p.vel`
- `tests/data/test_soa_array_only.arche` — combined multiple column accesses

**Bug fixes applied:**
1. Block comment support — lexer now handles `/* ... */` syntax (was causing parser hang)
2. Builtin registration — `write()` now recognized as builtin without declaration

Known limitation:
- `run sys;` requires world dispatch (Phase 2), marked as TODO in test files

After Step 7, test with:

```bash
make
./arche tests/data/test_soa.arche
./tests/data/test_soa
cat tests/tmp/ir_last.ll
```

**Expected IR (scalar-only implementation):**
- `@main` body for `p.pos = p.pos + p.vel`: strip-mined loop (vec/scalar bodies present but vec_lanes=0)
- Vector body loop structure in place (for future SIMD), all vector load/store ops use bitcast to handle pointer type
- Scalar loop operates on single elements
- Both have cleanup/initialization code
- No explicit for loops in Arche source

**Expected output:** `array ops done\n` (test_array_ops) or `test passed\n` (test_soa_array_only)

---

## Implementation Notes

**RHS Evaluation Timing:** RHS expressions are evaluated BEFORE dispatch for scalar ops, but AFTER implicit loop setup for whole-column ops. This prevents returning pointers instead of loaded values when evaluating field accesses without loop context.

**Bitcast for Vector Loads:** When `vector_lanes > 0`, loads emit `bitcast` instruction to reinterpret scalar pointer as vector pointer type before loading. This avoids type mismatches in LLVM IR (e.g., `load <4 x double>` from `double*`).

**Scalar-Only Fallback:** Loop infrastructure supports vector lanes, but currently runs scalar-only (vector_lanes = 0). SIMD ops not yet implemented. Full vectorization (vector_lanes = 4) is future work.

**System Call Limitation:** `run sys;` in proc context requires world dispatch (Phase 2). Currently marked as TODO in test files.

---

## Phase 2 — World Dispatch (✅ COMPLETE)

Enable `run system;` to call systems from proc context on matching archetypes.

### Overview

Systems operate on all archetypes that have the required columns. Current state:
- Multiple archetypes can be defined ✓
- Systems with column parameters defined ✓
- `run system;` in proc context → **fully implemented** ✓

Calling `run move;` where `move` takes `(pos, vel)` parameters will call the move system on all archetype instances in the current scope that have `pos` and `vel` fields.

### Implementation Summary

✅ **Step 1: Semantic analysis — world validation** (semantic/semantic.c)
- Validates world names exist when specified
- Empty world_name defaults to implicit "default world"

✅ **Step 2: Helper functions for dispatch** (codegen/codegen.c)
- `find_sys_decl(ctx, name)` — locate system declaration
- `archetype_has_field(arch, field)` — check field existence
- `archetype_matches_system(arch, sys)` — verify all required fields present

✅ **Step 3: STMT_RUN parsing** (parser/parser.c)
- Correctly parses `run system;` and `run system in world;` syntax
- **Bug fixed:** world_name was string literal, now properly allocated

✅ **Step 4: System dispatch code generation** (codegen/codegen.c)
- Iterates through variables in scope
- For each archetype instance matching the system
- Emits `call void @system_name(archetype_ptr)`

### Memory Management Fix

**Bug:** Parser initialized world_name to string literal `""`, causing "free(): invalid pointer" when statement was freed later.

**Fix:** Allocate proper memory for empty world name:
```c
char *world_name = malloc(1);  // allocate
world_name[0] = '\0';
if (match(parser, TOK_IN)) {
    free(world_name);          // free old, replace with new
    world_name = token_text(parser->current);
}
```

**Lesson:** Simple memory rule: allocate what you free, free what you allocate. No string literals in structures that will be freed.

---

## Phase 3 — World Removal (✅ COMPLETE - Apr 16, 2026)

Removed world-related parsing, semantic analysis, and code generation. Worlds are now documented as a planned feature, not yet implemented.

### Changes Made

**Parser (parser/parser.c):**
- Removed `parse_world_decl()` function
- Modified RunSystem rule to remove `in World` syntax — now just `run system;`
- Set world_name to NULL in run statement parsing

**Semantic (semantic/semantic.c):**
- Removed `WorldInfo` structure and related fields from `SemanticContext`
- Removed `find_world()` function
- Removed `analyze_world_decl()` function
- Removed DECL_WORLD case from `analyze_decl()`
- Simplified semantic_analyze() — removed world collection pass
- Removed world cleanup from `semantic_context_free()`
- Removed world validation from STMT_RUN analysis

**Codegen (codegen/codegen.c):**
- Removed `codegen_world_decl()` function
- Removed DECL_WORLD case from declaration dispatch

**Documentation (README.md, docs/GRAMMAR.peg):**
- Updated GRAMMAR.peg — removed WorldDecl rule, simplified RunSystem
- Updated README.md — marked Worlds as "Planned, Not Yet Implemented"
- Removed world examples from README
- Updated all system execution examples to use `run system;` instead of `run system in World;`

**Tests:**
- Updated `examples/archetype/test_archetype_verbose.arche` — removed `world Simulation()` declaration
- Updated `tests/semantic_tests.c` — removed automatic world prepending in analyze_string()
- All test files checked — no world declarations remain

### Verification

All tests passing after removal:
- ✓ 6/6 codegen tests
- ✓ 20/20 semantic tests
- ✓ Parser tests
- ✓ No compilation warnings related to undefined functions

---

## Out of Scope (Phase 4+)

- Multiple worlds for parallel computations (planned but not implemented)
- insert/delete redesign for fixed-size model
- Dynamic resizing
- Full SIMD vectorization
- Complex type systems

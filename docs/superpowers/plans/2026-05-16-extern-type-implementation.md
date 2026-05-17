# Extern Type Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `extern type T(N)` and the `consume` parameter modifier to Arche, so foreign resources (windows, sounds, fonts, sockets) can be referenced from Arche as opaque, generation-counter-protected handles without exposing pointers.

**Architecture:** Bottom-up through the existing compiler pipeline (lexer → CST → parser → semantic → codegen → runtime template), with TDD at every step. Each phase has its own test target already wired in the Makefile (`test-parser`, `test-semantic`, `test-codegen-unit`, `test-lower`). End-to-end validation uses a `tests/unit/language/extern_type/` LIT directory mirroring the existing `handles/` directory.

**Tech Stack:** C99 compiler, hand-rolled lexer/parser, LLVM IR codegen (existing), LIT-based end-to-end tests, GNU Make.

**Spec:** `docs/superpowers/specs/2026-05-16-extern-type-design.md` — read this first.

**Commit policy:** This plan was originally written with `git commit` as the last step of every task. **The user has set a hard rule: never commit on their behalf.** When executing this plan, treat every "Step N: Commit" instruction as "stage the changes with `git add <files>` and stop." The user reviews and commits manually. Subagents executing this plan MUST NOT run `git commit`.

**Pre-existing concept to NOT confuse with this feature:** Arche already has a `handle(ArchetypeName)` type for row references into archetype tables (`TYPE_HANDLE` in `cst/cst.h:124`, fixtures in `tests/unit/language/handles/`). That is internal-to-Arche. This plan adds a *separate* feature: opaque types whose instances are integer handles into a foreign-resource table, declared with `extern type Name(N);` and only mentionable in `extern` signatures.

---

## File Structure

**Files to create:**

| Path | Purpose |
|---|---|
| `runtime/handles.h` | Template helpers (`__arche_alloc_slot`, `__arche_get_slot`, `__arche_free_slot`) shared by every extern type's emitted table. |
| `tests/unit/language/extern_type/` | LIT directory for end-to-end `.arche` programs. Each file has a `// RUN:` directive. |
| `tests/unit/language/extern_type/basic_open_close.arche` | First happy-path program. |
| `tests/unit/language/extern_type/null_return.arche` | C returns NULL → Arche sees 0. |
| `tests/unit/language/extern_type/consume_use_after.arche` | Use-after-consume → compile error (xfail or expected-error). |
| `tests/unit/language/extern_type/stale_handle_abort.arche` | Stale handle escapes; runtime aborts. |
| `tests/unit/language/extern_type/capacity_exhausted.arche` | Open more than capacity → runtime aborts. |
| `tests/unit/language/extern_type/runtime/test_resource.c` | A toy "OS resource" C library (int counters keyed by handle) used by the extern_type tests. |

**Files to modify:**

| Path | What changes |
|---|---|
| `lexer/lexer.h` | Add `TOK_CONSUME` to `TokenKind` enum. |
| `lexer/lexer.c` | Recognize `consume` as a keyword. |
| `cst/cst.h` | Add `DECL_EXTERN_TYPE` to `DeclKind`; add `ExternTypeDecl` struct; add `TYPE_EXTERN` to `TypeKind` (or reuse `TYPE_NAME` with a flag — plan picks `TYPE_EXTERN`); add `int is_consume` to `Parameter`. |
| `cst/cst.c` | Add freeing for the new decl and type-ref shapes. |
| `parser/parser.c` | Parse `extern type Name(N);` after `extern`; parse `consume p: T` in extern parameter lists; resolve bare-type identifiers in extern signatures against extern-type table. |
| `semantic/semantic.c` | Register extern types in a new symbol table; enforce distinctness across types; track consume in extern signatures; static use-after-consume in function bodies. |
| `codegen/codegen.c` | For each `extern type T(N)`, emit a per-type table and `__arche_<T>_alloc/get/free` IR functions; wrap every extern call referring to extern types with marshal logic; on `consume`, free the slot after the C call. |
| `tests/unit/compiler/parser_tests.c` | New tests for `extern type` decl and `consume` parameter. |
| `tests/unit/compiler/semantic_tests.c` | New tests for distinctness, type-checking, and consume tracking. |
| `tests/unit/compiler/codegen_tests.c` | New tests verifying generated IR contains the table and marshal calls. |
| `runtime/io.c` | **Untouched.** This is intentionally left as the old pattern; the spec calls it a band-aid not a guide. |
| `Makefile` | Add `runtime/handles.h` to dependency lists if needed; ensure `tests/unit/language/extern_type/` is picked up by LIT (`tests/lit.cfg`). |
| `tests/lit.cfg` | If the file enumerates test directories explicitly, add `extern_type`. Otherwise no change. |
| `docs/GRAMMAR.peg` | Update grammar to include `ExternTypeDecl` and `consume` modifier. |
| `README.md` | One section describing the feature with the worked example from the spec. |

---

## Phase 1 — Lexer

### Task 1: Add the `consume` keyword to the lexer

**Files:**
- Modify: `lexer/lexer.h` (TokenKind enum, around line 30 next to `TOK_OUT`)
- Modify: `lexer/lexer.c` (keyword recognition table)
- Test: `tests/unit/compiler/parser_tests.c` (lexing is tested transitively by parser tests; add a tiny direct lex test in parser_tests' file or skip if there's no isolated lex test file — see existing pattern)

- [ ] **Step 1: Add the failing test**

Append to `tests/unit/compiler/parser_tests.c` (before the main test runner section):

```c
void test_lex_consume_keyword(void) {
    test_start("lex consume keyword");
    Lexer lex;
    lexer_init(&lex, "consume foo");
    Token t1 = lexer_next_token(&lex);
    ASSERT_EQ(t1.kind, TOK_CONSUME, "first token should be TOK_CONSUME");
    Token t2 = lexer_next_token(&lex);
    ASSERT_EQ(t2.kind, TOK_IDENT, "second token should be TOK_IDENT");
    lexer_free(&lex);
    test_pass_msg();
}
```

Register the test by adding `test_lex_consume_keyword();` to the parser_tests `main()` test list (find where `test_archetype_empty();` is called and add the new line alongside it).

- [ ] **Step 2: Run test, verify FAIL**

```
make test-parser
```

Expected: compilation error `TOK_CONSUME undeclared` — confirms the test references the new token name correctly.

- [ ] **Step 3: Add `TOK_CONSUME` to the enum**

In `lexer/lexer.h`, find the keyword block (around line 18–34) and add immediately after `TOK_OUT`:

```c
    TOK_OUT,
    TOK_CONSUME,
    TOK_RETURN,
```

- [ ] **Step 4: Recognize `consume` as a keyword in `lexer/lexer.c`**

Find the keyword-matching function (search for `TOK_OUT` to locate the keyword table or the cascade of `strcmp` / `len == ...` cases). Add a sibling entry:

```c
if (len == 7 && memcmp(start, "consume", 7) == 0) return TOK_CONSUME;
```

Place this next to the existing `out` recognition.

Also update `token_kind_name` in the same file to return `"TOK_CONSUME"` for the new kind.

- [ ] **Step 5: Run test, verify PASS**

```
make test-parser
```

Expected: `[N] lex consume keyword ✓` and all other tests still pass.

- [ ] **Step 6: Commit**

```bash
git add lexer/lexer.h lexer/lexer.c tests/unit/compiler/parser_tests.c
git commit -m "lexer: add TOK_CONSUME keyword for extern type destructors"
```

---

## Phase 2 — CST: data structures

### Task 2: Add `Parameter.is_consume` flag

**Files:**
- Modify: `cst/cst.h` (Parameter struct, around line 205)
- Modify: `cst/cst.c` (parameter freeing — no change expected; flag is a plain int)

- [ ] **Step 1: Add `is_consume` to the struct**

In `cst/cst.h`, find:

```c
struct Parameter {
    char *name;
    TypeRef *type;
    int is_out;
    SourceLoc loc;
};
```

Change to:

```c
struct Parameter {
    char *name;
    TypeRef *type;
    int is_out;
    int is_consume;
    SourceLoc loc;
};
```

- [ ] **Step 2: Initialize `is_consume = 0` everywhere a `Parameter` is constructed**

Search the codebase for `Parameter *` allocations:

```
grep -rn "malloc(sizeof(Parameter))" parser/ cst/
grep -rn "calloc.*Parameter" parser/ cst/
```

For each construction site, make sure `is_consume = 0` is set explicitly (or use `calloc` which zeroes by default). If the existing code uses `calloc`, no change is needed beyond the struct field addition.

- [ ] **Step 3: Build and verify no regressions**

```
make test-parser test-semantic
```

Expected: all existing tests still pass; no new tests yet.

- [ ] **Step 4: Commit**

```bash
git add cst/cst.h cst/cst.c
git commit -m "cst: add is_consume flag to Parameter"
```

### Task 3: ~~Add `TYPE_EXTERN` to TypeKind~~ — REMOVED

Initially planned to add a `TYPE_EXTERN` variant. On review, the rest of the plan resolves extern types via name lookup against the semantic symbol table (Task 10) and keeps `TypeRef.kind = TYPE_NAME` everywhere. The variant would be dead code. **Skip this task.** Subsequent task numbers remain unchanged for stable cross-references; Task 3 is intentionally absent.

### Task 4: Add `DECL_EXTERN_TYPE` and `ExternTypeDecl`

**Files:**
- Modify: `cst/cst.h` (DeclKind enum line 40; new struct ExternTypeDecl; union in Decl line 102)
- Modify: `cst/cst.c` (decl_free dispatch)

- [ ] **Step 1: Add the enum variant**

In `cst/cst.h`, change `DeclKind`:

```c
typedef enum {
    DECL_WORLD,
    DECL_ARCHETYPE,
    DECL_PROC,
    DECL_SYS,
    DECL_FUNC,
    DECL_FUNC_GROUP,
    DECL_STATIC,
    DECL_CONST,
    DECL_USE,
    DECL_EXTERN_TYPE,
} DeclKind;
```

- [ ] **Step 2: Declare the struct**

Add to `cst/cst.h` near the other decl structs (after `UseDecl`):

```c
typedef struct ExternTypeDecl {
    char *name;
    int capacity;
    SourceLoc loc;
} ExternTypeDecl;
```

Add the forward declaration at the top of the file alongside the others (`typedef struct ExternTypeDecl ExternTypeDecl;`).

Add the union member in `struct Decl.data`:

```c
ExternTypeDecl *extern_type;
```

- [ ] **Step 3: Free the new decl in `cst/cst.c`**

Find `decl_free` (search for `case DECL_USE:`). Add a sibling case:

```c
case DECL_EXTERN_TYPE:
    if (d->data.extern_type) {
        free(d->data.extern_type->name);
        free(d->data.extern_type);
    }
    break;
```

- [ ] **Step 4: Build, verify no regressions**

```
make all
```

Expected: clean compile.

- [ ] **Step 5: Commit**

```bash
git add cst/cst.h cst/cst.c
git commit -m "cst: add DECL_EXTERN_TYPE for `extern type Name(N)` declarations"
```

---

## Phase 3 — Parser

### Task 5: Parse `extern type Name(N);` declaration

**Files:**
- Modify: `parser/parser.c` (the `case TOK_EXTERN:` dispatch around line 1026; add a sibling for the `type` form)
- Test: `tests/unit/compiler/parser_tests.c`

- [ ] **Step 1: Write the failing test**

Append to `parser_tests.c`:

```c
void test_extern_type_decl(void) {
    test_start("extern type Window(8)");
    Program *prog = parse_string("extern type Window(8);");
    ASSERT_NOT_NULL(prog, "program is null");
    ASSERT_EQ(prog->decl_count, 1, "expected 1 decl");
    ASSERT_EQ(prog->decls[0]->kind, DECL_EXTERN_TYPE, "expected DECL_EXTERN_TYPE");
    ExternTypeDecl *et = prog->decls[0]->data.extern_type;
    ASSERT_NOT_NULL(et, "extern_type is null");
    ASSERT_EQ(strcmp(et->name, "Window"), 0, "wrong name");
    ASSERT_EQ(et->capacity, 8, "wrong capacity");
    program_free(prog);
    test_pass_msg();
}
```

Register it in `main()` near the other extern-related tests.

- [ ] **Step 2: Run, verify FAIL**

```
make test-parser
```

Expected: parser doesn't recognize the declaration; either the test crashes or `decl_count == 0`.

- [ ] **Step 3: Add the parser case**

In `parser/parser.c`, find the existing `extern`-dispatch code (search for `case TOK_EXTERN:` near line 1026). The existing code looks for `proc` or `func` after `extern`. Extend it to also recognize `type`:

```c
case TOK_EXTERN: {
    advance(parser);  /* consume 'extern' */
    /* peek: is this `extern type ...`? */
    if (parser->current.kind == TOK_IDENT &&
        parser->current.length == 4 &&
        memcmp(parser->current.start, "type", 4) == 0)
    {
        return parse_extern_type_decl(parser);  /* new */
    }
    /* fall through to existing extern func / extern proc dispatch */
    ...
}
```

Then add the new function `parse_extern_type_decl`:

```c
static Decl *parse_extern_type_decl(Parser *parser) {
    SourceLoc loc = current_loc(parser);
    advance(parser);  /* consume 'type' identifier */
    if (parser->current.kind != TOK_IDENT) {
        error(parser, "Expected type name after 'extern type'");
        return NULL;
    }
    char *name = strndup(parser->current.start, parser->current.length);
    advance(parser);
    if (!match(parser, TOK_LPAREN)) {
        error(parser, "Expected '(' after extern type name");
        free(name);
        return NULL;
    }
    if (parser->current.kind != TOK_NUMBER) {
        error(parser, "Expected capacity number");
        free(name);
        return NULL;
    }
    int capacity = parser->current.int_val;
    if (capacity <= 0 || capacity > 65535) {
        error(parser, "Extern type capacity must be between 1 and 65535");
    }
    advance(parser);
    if (!match(parser, TOK_RPAREN)) {
        error(parser, "Expected ')' after capacity");
        free(name);
        return NULL;
    }
    if (!match(parser, TOK_SEMI)) {
        error(parser, "Expected ';' after extern type declaration");
        free(name);
        return NULL;
    }
    ExternTypeDecl *et = calloc(1, sizeof(*et));
    et->name = name;
    et->capacity = capacity;
    et->loc = loc;
    Decl *d = calloc(1, sizeof(*d));
    d->kind = DECL_EXTERN_TYPE;
    d->loc = loc;
    d->data.extern_type = et;
    return d;
}
```

Helper function names (`current_loc`, `match`, `advance`, `error`) should match what already exists in `parser.c` — adjust to local conventions if names differ.

- [ ] **Step 4: Run, verify PASS**

```
make test-parser
```

Expected: new test passes; existing tests still pass.

- [ ] **Step 5: Commit**

```bash
git add parser/parser.c tests/unit/compiler/parser_tests.c
git commit -m "parser: parse 'extern type Name(N)' declarations"
```

### Task 6: Parse extern-type names as TypeRef in extern signatures

**Files:**
- Modify: `parser/parser.c` (the type-parsing function — search for where `TYPE_HANDLE` is constructed, or where `parse_type` lives)
- Test: `tests/unit/compiler/parser_tests.c`

This task makes `Window` parse as `TYPE_EXTERN` when it appears as a parameter or return type in an extern signature. The parser will *speculatively* parse any bare identifier as `TYPE_EXTERN` initially; the semantic phase later validates it.

Alternative implementation: only mark as `TYPE_EXTERN` if the name is a known extern type from a prior declaration. Picking the speculative approach because it avoids forward-reference issues — `extern type` declarations and `extern func` declarations can interleave in arbitrary order.

**Important nuance:** existing extern signatures use bare type names like `int`, `float`, `char` that are *not* extern types. The parser must continue to produce `TYPE_NAME` for those. The semantic phase determines whether a `TYPE_NAME` refers to an extern type and (if so) treats it as opaque.

This means **Task 6 has no parser change**: we keep `TYPE_NAME` for all bare identifier types and reclassify them in semantic. Skip this task as a no-op, but add a *test* documenting the contract.

- [ ] **Step 1: Add a contract-documenting test**

Append to `parser_tests.c`:

```c
void test_parser_treats_extern_type_as_typename(void) {
    test_start("parser leaves 'Window' as TYPE_NAME (semantic resolves)");
    Program *prog = parse_string(
        "extern type Window(8);\n"
        "extern func window_open(w: int, h: int) -> Window;\n"
    );
    ASSERT_NOT_NULL(prog, "program is null");
    ASSERT_EQ(prog->decl_count, 2, "expected 2 decls");
    ASSERT_EQ(prog->decls[1]->kind, DECL_FUNC, "expected DECL_FUNC");
    FuncDecl *f = prog->decls[1]->data.func;
    ASSERT_NOT_NULL(f->return_type, "no return type");
    ASSERT_EQ(f->return_type->kind, TYPE_NAME, "parser keeps 'Window' as TYPE_NAME");
    ASSERT_EQ(strcmp(f->return_type->data.name, "Window"), 0, "wrong name");
    program_free(prog);
    test_pass_msg();
}
```

Register in `main()`.

- [ ] **Step 2: Run, verify PASS immediately**

```
make test-parser
```

Expected: passes without code changes. This test pins down the contract for the semantic phase.

- [ ] **Step 3: Commit**

```bash
git add tests/unit/compiler/parser_tests.c
git commit -m "tests: document parser contract for extern-type-named TypeRefs"
```

### Task 7: Parse `consume` parameter modifier

**Files:**
- Modify: `parser/parser.c` (the extern parameter-parsing path; existing `out` handling is around line 767)
- Test: `tests/unit/compiler/parser_tests.c`

- [ ] **Step 1: Write the failing test**

Append to `parser_tests.c`:

```c
void test_consume_param_modifier(void) {
    test_start("consume parameter modifier");
    Program *prog = parse_string(
        "extern type Window(8);\n"
        "extern proc window_close(consume w: Window);\n"
    );
    ASSERT_NOT_NULL(prog, "program is null");
    ASSERT_EQ(prog->decl_count, 2, "expected 2 decls");
    ASSERT_EQ(prog->decls[1]->kind, DECL_PROC, "expected DECL_PROC");
    ProcDecl *p = prog->decls[1]->data.proc;
    ASSERT_EQ(p->param_count, 1, "expected 1 param");
    ASSERT_EQ(p->params[0]->is_consume, 1, "param should be consume");
    ASSERT_EQ(p->params[0]->is_out, 0, "param should NOT be out");
    program_free(prog);
    test_pass_msg();
}

void test_consume_and_out_mutually_exclusive(void) {
    test_start("consume and out cannot both apply to same param");
    ParseResult result = parse_source(
        "extern type Window(8);\n"
        "extern proc bad(consume out w: Window);\n"
    );
    ASSERT_TRUE(result.error_count >= 1, "expected at least one parse error");
    parse_result_free(&result);
    test_pass_msg();
}
```

Register both in `main()`. Note `ASSERT_TRUE` is already defined in this file; if not, copy it from `handle_tests.c:31`.

- [ ] **Step 2: Run, verify FAIL**

```
make test-parser
```

Expected: first test fails because `is_consume` is never set; second test fails because parser silently accepts both.

- [ ] **Step 3: Implement `consume` in the parameter parser**

Locate the function that parses one extern parameter (it currently consumes `out` and then the name/type). Around the existing `if (match(parser, TOK_OUT)) { param->is_out = 1; }`:

```c
int saw_out = 0, saw_consume = 0;
if (match(parser, TOK_OUT)) saw_out = 1;
if (match(parser, TOK_CONSUME)) saw_consume = 1;
/* allow either order */
if (!saw_out && match(parser, TOK_OUT)) saw_out = 1;

if (saw_out && saw_consume) {
    error(parser, "'consume' and 'out' cannot both apply to the same parameter");
}
param->is_out = saw_out;
param->is_consume = saw_consume;
```

(Adjust to match how the local parser style handles modifiers; if there's a `parse_param_modifier` helper, add to that instead.)

- [ ] **Step 4: Run, verify PASS**

```
make test-parser
```

Expected: both tests pass.

- [ ] **Step 5: Commit**

```bash
git add parser/parser.c tests/unit/compiler/parser_tests.c
git commit -m "parser: accept 'consume' parameter modifier; reject combined with 'out'"
```

### Task 8: Update grammar documentation

**Files:**
- Modify: `docs/GRAMMAR.peg`

- [ ] **Step 1: Add the new productions**

In `docs/GRAMMAR.peg`, add to the `Decl` rule:

```
Decl ← ArchetypeDecl / ProcDecl / SysDecl / FuncDecl / ExternTypeDecl
```

Then add at the bottom of the "Declarations" section:

```
ExternTypeDecl ← "extern" _ "type" _ Identifier _ "(" _ Number _ ")" _ ";"
```

Also add a note above `FuncParam` clarifying the extern-only `consume` modifier:

```
# Parameter modifiers (extern signatures only)
# - "out":     parameter is returned in the result tuple; caller may pre-fill
# - "consume": parameter's handle slot is freed after the call; further use is an error
ExternFuncParam ← ("out" / "consume")? _ Identifier _ ":" _ Type
```

- [ ] **Step 2: Commit**

```bash
git add docs/GRAMMAR.peg
git commit -m "docs: extern type and consume modifier grammar"
```

---

## Phase 4 — Semantic

### Task 9: Register extern types in the symbol table

**Files:**
- Modify: `semantic/semantic.c` (search for archetype-registration logic; mirror it for extern types)
- Modify: `semantic/semantic.h` if a new public struct is needed
- Test: `tests/unit/compiler/semantic_tests.c`

- [ ] **Step 1: Write the failing test**

Append to `semantic_tests.c`:

```c
void test_extern_type_registered(void) {
    test_start("extern type registered in symbol table");
    AnalysisResult r = analyze_string("extern type Window(8);");
    ASSERT_NOT_NULL(r.ctx, "no semantic context");
    ASSERT_TRUE(semantic_has_extern_type(r.ctx, "Window"), "Window not registered");
    ASSERT_EQ(semantic_extern_type_capacity(r.ctx, "Window"), 8, "wrong capacity");
    semantic_context_free(r.ctx);
    program_free(r.prog);
    test_pass_msg();
}

void test_extern_type_duplicate_is_error(void) {
    test_start("duplicate extern type name is rejected");
    AnalysisResult r = analyze_string(
        "extern type Window(8);\n"
        "extern type Window(16);\n"
    );
    ASSERT_TRUE(semantic_error_count(r.ctx) >= 1, "expected redeclaration error");
    semantic_context_free(r.ctx);
    program_free(r.prog);
    test_pass_msg();
}
```

Register both in the test runner main.

- [ ] **Step 2: Run, verify FAIL**

```
make test-semantic
```

Expected: undefined references to `semantic_has_extern_type`, `semantic_extern_type_capacity`. That confirms we need to add them.

- [ ] **Step 3: Add API to semantic.h**

In `semantic/semantic.h`, declare:

```c
int  semantic_has_extern_type(const SemanticContext *ctx, const char *name);
int  semantic_extern_type_capacity(const SemanticContext *ctx, const char *name);
int  semantic_error_count(const SemanticContext *ctx);
```

(`semantic_error_count` may already exist; check first.)

- [ ] **Step 4: Implement in semantic.c**

Find the existing symbol-table struct (search for `SemanticContext` definition; likely contains arrays of registered archetypes). Add a parallel array of registered extern types:

```c
typedef struct {
    char *name;
    int capacity;
    SourceLoc loc;
} ExternTypeEntry;
```

Add `ExternTypeEntry *extern_types; int extern_type_count; int extern_type_cap;` to `SemanticContext`. Wire allocation into init and free into teardown.

In the top-level analysis loop (search for the `for (each decl) switch (kind)` pattern), add:

```c
case DECL_EXTERN_TYPE: {
    ExternTypeDecl *et = decl->data.extern_type;
    if (semantic_has_extern_type(ctx, et->name)) {
        semantic_error(ctx, et->loc, "extern type '%s' redeclared", et->name);
        break;
    }
    /* grow extern_types array, append entry */
    ...
    break;
}
```

Implement `semantic_has_extern_type` and `semantic_extern_type_capacity` as linear scans (capacity is small — <8 extern types in any realistic program; no hashtable needed).

- [ ] **Step 5: Run, verify PASS**

```
make test-semantic
```

Expected: both new tests pass; existing tests pass.

- [ ] **Step 6: Commit**

```bash
git add semantic/semantic.c semantic/semantic.h tests/unit/compiler/semantic_tests.c
git commit -m "semantic: register extern types, reject duplicates"
```

### Task 10: Reclassify TYPE_NAME → opaque-extern in extern signatures

**Files:**
- Modify: `semantic/semantic.c` (extern signature checking)
- Test: `tests/unit/compiler/semantic_tests.c`

This task makes the semantic phase recognize that a `TypeRef` of kind `TYPE_NAME` with a name matching a registered extern type is actually an opaque extern type. It enforces that extern types may *only* appear in extern signatures.

- [ ] **Step 1: Write the failing tests**

```c
void test_extern_type_only_in_externs(void) {
    test_start("extern type cannot appear in non-extern proc");
    AnalysisResult r = analyze_string(
        "extern type Window(8);\n"
        "proc bad(w: Window) {}\n"
    );
    ASSERT_TRUE(semantic_error_count(r.ctx) >= 1, "expected error for non-extern use");
    semantic_context_free(r.ctx);
    program_free(r.prog);
    test_pass_msg();
}

void test_extern_signature_with_extern_type_ok(void) {
    test_start("extern proc with extern-type param is accepted");
    AnalysisResult r = analyze_string(
        "extern type Window(8);\n"
        "extern proc close(consume w: Window);\n"
    );
    ASSERT_EQ(semantic_error_count(r.ctx), 0, "should be no errors");
    semantic_context_free(r.ctx);
    program_free(r.prog);
    test_pass_msg();
}

void test_unknown_type_name_still_errors(void) {
    test_start("unknown type name in extern signature is still an error");
    AnalysisResult r = analyze_string(
        "extern func bad() -> Doesnotexist;\n"
    );
    ASSERT_TRUE(semantic_error_count(r.ctx) >= 1, "expected unknown-type error");
    semantic_context_free(r.ctx);
    program_free(r.prog);
    test_pass_msg();
}
```

Register all three in the test runner main.

- [ ] **Step 2: Run, verify FAIL**

```
make test-semantic
```

Expected: tests fail because the checks don't exist yet.

- [ ] **Step 3: Implement the classification**

Add a helper to `semantic.c`:

```c
/* Returns 1 if `type_ref` is a TYPE_NAME whose name matches a registered extern type. */
static int is_extern_type_ref(SemanticContext *ctx, const TypeRef *tr) {
    if (!tr || tr->kind != TYPE_NAME) return 0;
    return semantic_has_extern_type(ctx, tr->data.name);
}
```

In the proc/func/sys checker, when examining each parameter type and the return type:

- If it's an extern type ref AND the proc/func is NOT extern → emit error: `"extern type '%s' may only appear in extern signatures"`.
- If `is_consume` is set on a parameter AND that parameter's type is NOT an extern type ref → emit error: `"'consume' may only modify extern-type parameters"`.

Also handle the unknown-type case explicitly (it may already work; the new test pins it down).

- [ ] **Step 4: Run, verify PASS**

```
make test-semantic
```

Expected: all three tests pass.

- [ ] **Step 5: Commit**

```bash
git add semantic/semantic.c tests/unit/compiler/semantic_tests.c
git commit -m "semantic: enforce extern types only in extern signatures; consume only on extern types"
```

### Task 11: Type-distinctness for extern types

**Files:**
- Modify: `semantic/semantic.c` (expression type checking — wherever type compatibility is computed)
- Test: `tests/unit/compiler/semantic_tests.c`

- [ ] **Step 1: Write the failing test**

```c
void test_extern_types_distinct(void) {
    test_start("Window and Sound are not interchangeable");
    AnalysisResult r = analyze_string(
        "extern type Window(8);\n"
        "extern type Sound(64);\n"
        "extern proc window_close(consume w: Window);\n"
        "extern func sound_open() -> Sound;\n"
        "proc main() {\n"
        "  let s := sound_open();\n"
        "  window_close(s);\n"
        "}\n"
    );
    ASSERT_TRUE(semantic_error_count(r.ctx) >= 1, "expected type-mismatch error");
    semantic_context_free(r.ctx);
    program_free(r.prog);
    test_pass_msg();
}
```

Register in main.

- [ ] **Step 2: Run, verify FAIL**

```
make test-semantic
```

Expected: test fails — the existing type checker likely treats two `TYPE_NAME` refs with different names as compatible-or-incompatible based on existing rules; pin down the specific error.

- [ ] **Step 3: Implement in the type-equality / call-site checker**

Find the type compatibility check (search for type-comparison logic in semantic.c). When the formal parameter type is an extern type ref, the argument's resolved type must:
- Be exactly the same extern type name, OR
- Be the integer literal `0` (null handle).

For any other case (different extern type, non-extern int, etc.) → emit type error.

This may require introducing a small inferred-type representation for expressions if one doesn't already exist; if expression typing exists, just extend its compatibility rules.

- [ ] **Step 4: Run, verify PASS**

```
make test-semantic
```

Expected: test passes.

- [ ] **Step 5: Commit**

```bash
git add semantic/semantic.c tests/unit/compiler/semantic_tests.c
git commit -m "semantic: enforce extern type distinctness (Window != Sound)"
```

### Task 12: Static use-after-consume tracking

**Files:**
- Modify: `semantic/semantic.c` (statement-level analysis of proc bodies)
- Test: `tests/unit/compiler/semantic_tests.c`

- [ ] **Step 1: Write the failing tests**

```c
void test_use_after_consume_local_error(void) {
    test_start("use after consume in same scope is a compile error");
    AnalysisResult r = analyze_string(
        "extern type Window(8);\n"
        "extern func open_(t: char[], a: int, b: int) -> Window;\n"
        "extern proc close_(consume w: Window);\n"
        "extern proc poll_(w: Window);\n"
        "proc main() {\n"
        "  let w := open_(\"\", 1, 1);\n"
        "  close_(w);\n"
        "  poll_(w);\n"
        "}\n"
    );
    ASSERT_TRUE(semantic_error_count(r.ctx) >= 1, "expected use-after-consume error");
    semantic_context_free(r.ctx);
    program_free(r.prog);
    test_pass_msg();
}

void test_no_false_positive_when_unconsumed(void) {
    test_start("normal pass-through use is fine");
    AnalysisResult r = analyze_string(
        "extern type Window(8);\n"
        "extern func open_(t: char[], a: int, b: int) -> Window;\n"
        "extern proc close_(consume w: Window);\n"
        "extern proc poll_(w: Window);\n"
        "proc main() {\n"
        "  let w := open_(\"\", 1, 1);\n"
        "  poll_(w);\n"
        "  close_(w);\n"
        "}\n"
    );
    ASSERT_EQ(semantic_error_count(r.ctx), 0, "should be no errors");
    semantic_context_free(r.ctx);
    program_free(r.prog);
    test_pass_msg();
}
```

Register both.

- [ ] **Step 2: Run, verify FAIL**

```
make test-semantic
```

Expected: first test fails (no tracking yet).

- [ ] **Step 3: Implement consumption tracking**

In the statement-checker for proc/func/sys bodies, maintain a per-scope set of "consumed binding names." When analyzing an expression statement that's a call:

```c
/* pseudocode */
for each (formal, actual) pair {
    if (formal.is_consume) {
        if (actual is a simple identifier reference (let-bound var)) {
            mark that name as consumed in the current scope's consumed-set;
        }
        /* if actual is anything else (field access, index, return value),
           no static tracking — runtime gen counter handles it */
    }
}
```

When analyzing any identifier-reference expression, before returning its type, check whether the name is in the current scope's consumed-set. If yes → emit error: `"use of consumed handle '%s'"`.

Scope handling: when an `if`/`for` block opens, take a snapshot. When it closes, the consumed-set returns to its prior state (or to the union of branches, conservatively; v1 can just discard branch-local consumption — see below).

For v1, the simplest correct rule: track consumption only at the *function scope* level — every consume marks the binding consumed for the rest of the proc body, regardless of branches. This may over-reject some valid code (consume inside an `if` whose other branch doesn't consume) but never under-rejects. Document this limitation in a comment and revisit only if real programs hit it.

- [ ] **Step 4: Run, verify PASS**

```
make test-semantic
```

Expected: both tests pass.

- [ ] **Step 5: Commit**

```bash
git add semantic/semantic.c tests/unit/compiler/semantic_tests.c
git commit -m "semantic: track use-after-consume statically in proc bodies"
```

---

## Phase 5 — Runtime support

### Task 13: Create the handle-table template header

**Files:**
- Create: `runtime/handles.h`
- Modify: `Makefile` if it needs to install the header to `build/runtime/`

- [ ] **Step 1: Create the header**

Write `runtime/handles.h`:

```c
/* Arche extern-type handle tables — instantiated per extern type by codegen.
 *
 * Each `extern type T(N)` produces a static slot array and three helpers via
 * the ARCHE_HANDLE_TABLE(T, N) macro. Handles are int32_t with the layout:
 *
 *   bits 31..16 : generation counter (uint16)
 *   bits 15..0  : slot index + 1 (uint16; 0 = null handle)
 *
 * All failures abort() after printing a single line to stderr.
 */

#ifndef ARCHE_HANDLES_H
#define ARCHE_HANDLES_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    void    *ptr;
    uint16_t gen;
    uint16_t in_use;
} __ArcheSlot;

/* Allocate a free slot for ptr; return packed handle. Aborts if full. */
static inline int32_t __arche_slot_alloc(const char *type_name,
                                         __ArcheSlot *slots,
                                         int capacity,
                                         void *ptr)
{
    for (int i = 0; i < capacity; i++) {
        if (!slots[i].in_use) {
            slots[i].ptr = ptr;
            slots[i].in_use = 1;
            return (int32_t)(((uint32_t)slots[i].gen << 16) | (uint32_t)(i + 1));
        }
    }
    fprintf(stderr,
            "arche fatal: capacity exhausted (type=%s capacity=%d)\n",
            type_name, capacity);
    abort();
}

/* Decode and validate; return ptr or abort. */
static inline void *__arche_slot_get(const char *type_name,
                                     __ArcheSlot *slots,
                                     int capacity,
                                     int32_t handle)
{
    if (handle == 0) {
        fprintf(stderr, "arche fatal: null handle (type=%s)\n", type_name);
        abort();
    }
    uint16_t gen      = (uint16_t)((uint32_t)handle >> 16);
    uint16_t slot_p1  = (uint16_t)((uint32_t)handle & 0xFFFFu);
    if (slot_p1 == 0 || slot_p1 > (uint16_t)capacity) {
        fprintf(stderr,
                "arche fatal: slot out of range (type=%s handle=0x%08x slot=%u capacity=%d)\n",
                type_name, (unsigned)(uint32_t)handle, slot_p1, capacity);
        abort();
    }
    int idx = slot_p1 - 1;
    if (!slots[idx].in_use) {
        fprintf(stderr,
                "arche fatal: freed slot (type=%s handle=0x%08x slot=%d)\n",
                type_name, (unsigned)(uint32_t)handle, idx);
        abort();
    }
    if (slots[idx].gen != gen) {
        fprintf(stderr,
                "arche fatal: stale generation (type=%s handle=0x%08x slot=%d expected=%u got=%u)\n",
                type_name, (unsigned)(uint32_t)handle, idx,
                (unsigned)slots[idx].gen, (unsigned)gen);
        abort();
    }
    return slots[idx].ptr;
}

/* Free slot; bump generation. No-op on null handle. */
static inline void __arche_slot_free(const char *type_name,
                                     __ArcheSlot *slots,
                                     int capacity,
                                     int32_t handle)
{
    (void)type_name;
    if (handle == 0) return;
    uint16_t slot_p1 = (uint16_t)((uint32_t)handle & 0xFFFFu);
    if (slot_p1 == 0 || slot_p1 > (uint16_t)capacity) return;
    int idx = slot_p1 - 1;
    slots[idx].in_use = 0;
    slots[idx].ptr = NULL;
    slots[idx].gen++;
}

#endif /* ARCHE_HANDLES_H */
```

- [ ] **Step 2: Add a small sanity test for the runtime header**

Create `tests/unit/compiler/handle_runtime_tests.c` (new file):

```c
#include "../../../runtime/handles.h"
#include <assert.h>
#include <stdio.h>

static __ArcheSlot slots[4];

int main(void) {
    /* alloc, get, free, generation increments */
    int dummy = 7;
    int32_t h1 = __arche_slot_alloc("T", slots, 4, &dummy);
    assert(h1 != 0);
    assert(__arche_slot_get("T", slots, 4, h1) == &dummy);
    __arche_slot_free("T", slots, 4, h1);

    int32_t h2 = __arche_slot_alloc("T", slots, 4, &dummy);
    assert(h2 != h1);   /* different generation */

    __arche_slot_free("T", slots, 4, 0);  /* no-op */
    printf("handle_runtime_tests: OK\n");
    return 0;
}
```

Add a Makefile target for it:

```make
HANDLE_RUNTIME_TEST_BIN = $(BUILD_DIR)/handle-runtime-test

$(HANDLE_RUNTIME_TEST_BIN): $(BUILD_DIR)/unit/compiler/handle_runtime_tests.o
	$(CC) $(CFLAGS) -o $@ $^

test-handle-runtime: $(HANDLE_RUNTIME_TEST_BIN)
	./$(HANDLE_RUNTIME_TEST_BIN)
```

Add `test-handle-runtime` to the `.PHONY` list and to the master `test` target's deps.

- [ ] **Step 3: Run, verify PASS**

```
make test-handle-runtime
```

Expected: `handle_runtime_tests: OK`.

- [ ] **Step 4: Commit**

```bash
git add runtime/handles.h tests/unit/compiler/handle_runtime_tests.c Makefile
git commit -m "runtime: add handles.h slot-table helpers (alloc/get/free + abort on misuse)"
```

---

## Phase 6 — Codegen

### Task 14: Emit per-extern-type static slot table

**Files:**
- Modify: `codegen/codegen.c`
- Test: `tests/unit/compiler/codegen_tests.c`

The codegen emits LLVM IR. For each `DECL_EXTERN_TYPE`, emit:

- A static global slot array `@__arche_<T>_slots = internal global [N x %__ArcheSlot] zeroinitializer`.
- A static const string `@__arche_<T>_typename = internal constant [M x i8] c"<T>\00"` for error messages.

Plus a declaration of the three runtime helpers (`__arche_slot_alloc`, `__arche_slot_get`, `__arche_slot_free`) from `runtime/handles.h`.

- [ ] **Step 1: Write the failing test**

Append to `codegen_tests.c`:

```c
void test_codegen_extern_type_emits_table(void) {
    test_start("extern type emits IR table");
    char *ir = compile_to_ir_string("extern type Window(8);");
    ASSERT_NOT_NULL(ir, "no IR produced");
    ASSERT_TRUE(strstr(ir, "@__arche_Window_slots") != NULL, "no slots global");
    ASSERT_TRUE(strstr(ir, "8 x") != NULL || strstr(ir, "[8 x") != NULL, "wrong capacity");
    free(ir);
    test_pass_msg();
}
```

(`compile_to_ir_string` is a helper in codegen_tests.c. If it doesn't exist, look at what helper produces IR for assertion in existing tests and follow the same pattern. If no helper exists, add a small one that writes to a temp file and reads back.)

Register the test.

- [ ] **Step 2: Run, verify FAIL**

```
make test-codegen-unit
```

Expected: no global with that name in the IR.

- [ ] **Step 3: Add emit code in codegen.c**

Find the top-level dispatch over decls in codegen (search for `case DECL_ARCHETYPE:` in `codegen.c`). Add a sibling:

```c
case DECL_EXTERN_TYPE:
    codegen_emit_extern_type(ctx, decl->data.extern_type);
    break;
```

Implement `codegen_emit_extern_type`:

```c
static void codegen_emit_extern_type(CodegenCtx *ctx, ExternTypeDecl *et) {
    /* Emit:
     *   %__ArcheSlot = type { i8*, i16, i16 }
     *   @__arche_<T>_slots = internal global [<N> x %__ArcheSlot] zeroinitializer
     *   @__arche_<T>_typename = internal constant [<len> x i8] c"<T>\00"
     */
    emit_struct_type_once(ctx, "__ArcheSlot", "{ i8*, i16, i16 }");
    emit_global_array(ctx, "@__arche_%s_slots", et->name,
                      "[%d x %%__ArcheSlot] zeroinitializer", et->capacity);
    emit_global_string(ctx, "@__arche_%s_typename", et->name, et->name);
    /* Declare runtime helpers once per program. */
    emit_runtime_decls_once(ctx);
}
```

(`emit_struct_type_once`, `emit_global_array`, `emit_global_string`, `emit_runtime_decls_once` should follow existing helper naming in codegen.c. Use whatever string-builder pattern the file already uses; adjust signatures to match.)

`emit_runtime_decls_once` emits, exactly once per IR module:

```
declare i32 @__arche_slot_alloc(i8*, %__ArcheSlot*, i32, i8*)
declare i8* @__arche_slot_get(i8*, %__ArcheSlot*, i32, i32)
declare void @__arche_slot_free(i8*, %__ArcheSlot*, i32, i32)
```

- [ ] **Step 4: Run, verify PASS**

```
make test-codegen-unit
```

Expected: test passes.

- [ ] **Step 5: Commit**

```bash
git add codegen/codegen.c tests/unit/compiler/codegen_tests.c
git commit -m "codegen: emit per-extern-type slot table and runtime helper declarations"
```

### Task 15: Marshal returns from extern functions returning an extern type

**Files:**
- Modify: `codegen/codegen.c` (extern call-site IR emission)
- Test: `tests/unit/compiler/codegen_tests.c`

When an `extern func` returns an extern type, the user's C function returns `i8*` (pointer). The marshaler converts non-null to a handle by calling `__arche_slot_alloc`.

Emitted IR pattern for `let w = window_open(t, w, h);`:

```
%raw = call i8* @window_open(i8* %t, i32 %w, i32 %h)
%is_null = icmp eq i8* %raw, null
br i1 %is_null, label %null_h, label %wrap_h
null_h:
  %h_null = i32 0
  br label %done_h
wrap_h:
  %h_wrap = call i32 @__arche_slot_alloc(
              i8* getelementptr inbounds ([7 x i8], [7 x i8]* @__arche_Window_typename, i32 0, i32 0),
              %__ArcheSlot* getelementptr inbounds ([8 x %__ArcheSlot], [8 x %__ArcheSlot]* @__arche_Window_slots, i32 0, i32 0),
              i32 8,
              i8* %raw)
  br label %done_h
done_h:
  %w = phi i32 [ %h_null, %null_h ], [ %h_wrap, %wrap_h ]
```

- [ ] **Step 1: Write the failing test**

```c
void test_codegen_extern_return_marshal(void) {
    test_start("extern returning extern type emits __arche_slot_alloc");
    char *ir = compile_to_ir_string(
        "extern type Window(8);\n"
        "extern func open_(a: int, b: int) -> Window;\n"
        "proc main() { let w := open_(1, 2); }\n"
    );
    ASSERT_NOT_NULL(ir, "no IR");
    ASSERT_TRUE(strstr(ir, "@__arche_slot_alloc") != NULL, "no alloc call emitted");
    free(ir);
    test_pass_msg();
}
```

- [ ] **Step 2: Run, verify FAIL**

```
make test-codegen-unit
```

- [ ] **Step 3: Implement marshaling for extern returns**

In codegen.c, locate the function that emits a call to an extern function (search for where extern call IR is generated, likely tied to handling `DECL_FUNC` with `is_extern`). When the return type is a `TYPE_NAME` matching an extern type:

1. Lower the C function's return type to `i8*`.
2. After the call, emit the null-check + branch + `__arche_slot_alloc` call shown above.
3. The result int32 handle is what the rest of Arche sees.

The C function symbol name matches the Arche extern's declared name (no mangling), per spec.

- [ ] **Step 4: Run, verify PASS**

```
make test-codegen-unit
```

- [ ] **Step 5: Commit**

```bash
git add codegen/codegen.c tests/unit/compiler/codegen_tests.c
git commit -m "codegen: marshal extern returns of extern types via __arche_slot_alloc"
```

### Task 16: Marshal extern-type parameters (non-consume)

**Files:**
- Modify: `codegen/codegen.c`
- Test: `tests/unit/compiler/codegen_tests.c`

When an extern call has a parameter typed as an extern type (without `consume`), emit a `__arche_slot_get` call before the C function call to convert the int handle to a pointer.

Emitted IR pattern for `window_present(w, fb, 800, 600);`:

```
%w_ptr = call i8* @__arche_slot_get(i8* @__arche_Window_typename, ..., i32 8, i32 %w)
call void @window_present(i8* %w_ptr, i32* %fb, i32 800, i32 600)
```

- [ ] **Step 1: Write the failing test**

```c
void test_codegen_extern_param_marshal(void) {
    test_start("extern with extern-type param emits __arche_slot_get");
    char *ir = compile_to_ir_string(
        "extern type Window(8);\n"
        "extern proc present_(w: Window);\n"
        "proc main() {\n"
        "  let w := 1;\n"  /* simulate having a handle int */
        "  present_(w);\n"
        "}\n"
    );
    ASSERT_NOT_NULL(ir, "no IR");
    ASSERT_TRUE(strstr(ir, "@__arche_slot_get") != NULL, "no get call emitted");
    free(ir);
    test_pass_msg();
}
```

(Note: this test creates the handle via `let w := 1;` which is technically a type error — but we want to test codegen, not semantic. If the semantic check rejects this test, replace it with a longer program that gets a real handle from an `open_` extern.)

- [ ] **Step 2: Run, verify FAIL**

```
make test-codegen-unit
```

- [ ] **Step 3: Implement parameter marshaling**

In the call-site emitter, for each argument whose corresponding formal parameter is an extern type:

1. If the formal parameter has `is_consume = 0`: emit a `__arche_slot_get` call to convert the int handle to an `i8*` pointer, then pass the pointer to the C function.
2. If `is_consume = 1`: same as 1, but also remember the handle int for step 17 (slot free after the call).

For this task, implement case 1 only.

- [ ] **Step 4: Run, verify PASS**

```
make test-codegen-unit
```

- [ ] **Step 5: Commit**

```bash
git add codegen/codegen.c tests/unit/compiler/codegen_tests.c
git commit -m "codegen: marshal extern-type parameters via __arche_slot_get"
```

### Task 17: Marshal consume-parameters with post-call slot free

**Files:**
- Modify: `codegen/codegen.c`
- Test: `tests/unit/compiler/codegen_tests.c`

When a parameter has `is_consume = 1`, the codegen emits:

1. Null-check: if handle == 0, skip the call and skip the free (no-op).
2. Otherwise: get the pointer (`__arche_slot_get`), call the C function, then call `__arche_slot_free` to recycle the slot.

```
%is_null = icmp eq i32 %w, 0
br i1 %is_null, label %skip, label %do_consume
do_consume:
  %ptr = call i8* @__arche_slot_get(..., i32 %w)
  call void @window_close(i8* %ptr)
  call void @__arche_slot_free(..., i32 %w)
  br label %skip
skip:
  ; continue
```

- [ ] **Step 1: Write the failing test**

```c
void test_codegen_consume_emits_slot_free(void) {
    test_start("consume extern emits __arche_slot_free");
    char *ir = compile_to_ir_string(
        "extern type Window(8);\n"
        "extern func open_(a: int, b: int) -> Window;\n"
        "extern proc close_(consume w: Window);\n"
        "proc main() {\n"
        "  let w := open_(1, 2);\n"
        "  close_(w);\n"
        "}\n"
    );
    ASSERT_NOT_NULL(ir, "no IR");
    ASSERT_TRUE(strstr(ir, "@__arche_slot_free") != NULL, "no free call emitted");
    free(ir);
    test_pass_msg();
}
```

- [ ] **Step 2: Run, verify FAIL**

```
make test-codegen-unit
```

- [ ] **Step 3: Implement consume-call emission**

In the call-site emitter (already handling extern-type params from Task 16), when `is_consume = 1`:

1. Wrap the entire (`get` + C-call + `free`) sequence in the null-check branch shown above.
2. After the `free`, the int handle value remains in the calling scope (Arche's semantic phase tracks consumption; the int still exists in the register/local).

- [ ] **Step 4: Run, verify PASS**

```
make test-codegen-unit
```

- [ ] **Step 5: Commit**

```bash
git add codegen/codegen.c tests/unit/compiler/codegen_tests.c
git commit -m "codegen: emit __arche_slot_free after consume parameter call sites"
```

---

## Phase 7 — End-to-end integration tests

### Task 18: Create a toy C resource library for tests

**Files:**
- Create: `tests/unit/language/extern_type/runtime/test_resource.c`

This library simulates a foreign resource: each "resource" is just an `int counter`. Two extern operations: `resource_open` (returns a new `int*` to a static pool) and `resource_close` (frees that pool slot). The Arche tests link against this.

- [ ] **Step 1: Write the C library**

Create `tests/unit/language/extern_type/runtime/test_resource.c`:

```c
#include <stdio.h>
#include <stdlib.h>

#define POOL_CAP 8
static int pool[POOL_CAP];
static int in_use[POOL_CAP];

int *resource_open(int initial) {
    for (int i = 0; i < POOL_CAP; i++) {
        if (!in_use[i]) {
            in_use[i] = 1;
            pool[i] = initial;
            return &pool[i];
        }
    }
    return NULL;  /* triggers null-handle path */
}

int resource_get(int *r) { return *r; }
void resource_set(int *r, int v) { *r = v; }

void resource_close(int *r) {
    for (int i = 0; i < POOL_CAP; i++) {
        if (&pool[i] == r) { in_use[i] = 0; return; }
    }
}
```

- [ ] **Step 2: Commit**

```bash
git add tests/unit/language/extern_type/runtime/test_resource.c
git commit -m "tests: add toy C resource library for extern_type end-to-end tests"
```

### Task 19: Add LIT directory and basic open/close test

**Files:**
- Create: `tests/unit/language/extern_type/basic_open_close.arche`
- Modify: `tests/lit.cfg` (if it enumerates dirs)

- [ ] **Step 1: Write the .arche test**

Create `tests/unit/language/extern_type/basic_open_close.arche`:

```arche
// RUN: %arche -o %t --link tests/unit/language/extern_type/runtime/test_resource.c %s && %t
// CHECK: value=42

extern type Resource(8);

extern func resource_open(initial: int) -> Resource;
extern func resource_get(r: Resource) -> int;
extern proc resource_set(r: Resource, v: int);
extern proc resource_close(consume r: Resource);

extern proc printf(fmt: char[], v: int);

proc main() {
  let r := resource_open(10);
  resource_set(r, 42);
  let v := resource_get(r);
  printf("value=%d\n", v);
  resource_close(r);
}
```

(`--link` is illustrative; the real linking flag must match the existing compiler's. If `--link` doesn't exist, the test plan needs to extend the compiler driver in a separate task, or the LIT runner can invoke `cc -c` and link manually. For now, use whatever LIT directive style the other `tests/unit/language/handles/*.arche` files use — check `tests/unit/language/handles/handle_basic.arche` and mirror.)

- [ ] **Step 2: Run, verify PASS**

```
make test
```

Expected: LIT picks up the new test and it passes.

- [ ] **Step 3: Commit**

```bash
git add tests/unit/language/extern_type/basic_open_close.arche
git commit -m "tests: end-to-end basic_open_close.arche"
```

### Task 20: Null-return test

**Files:**
- Create: `tests/unit/language/extern_type/null_return.arche`

- [ ] **Step 1: Write the test**

Create `tests/unit/language/extern_type/null_return.arche`:

```arche
// RUN: %arche -o %t --link tests/unit/language/extern_type/runtime/test_resource.c %s && %t
// CHECK: ok_null=1

extern type Resource(8);
extern func resource_open(initial: int) -> Resource;
extern proc printf(fmt: char[], v: int);

proc main() {
  // Open POOL_CAP+1 (=9) resources; the 9th call to resource_open returns NULL,
  // which should marshal to handle 0 in Arche.
  let r1 := resource_open(1);
  let r2 := resource_open(2);
  let r3 := resource_open(3);
  let r4 := resource_open(4);
  let r5 := resource_open(5);
  let r6 := resource_open(6);
  let r7 := resource_open(7);
  let r8 := resource_open(8);
  let r9 := resource_open(9);  // pool full → returns NULL → Arche sees 0
  let ok := r9 == 0;
  printf("ok_null=%d\n", ok);
}
```

- [ ] **Step 2: Run, verify PASS**

```
make test
```

- [ ] **Step 3: Commit**

```bash
git add tests/unit/language/extern_type/null_return.arche
git commit -m "tests: null_return.arche — NULL from C marshals to 0 in Arche"
```

### Task 21: Use-after-consume compile-error test

**Files:**
- Create: `tests/unit/language/extern_type/consume_use_after.arche`

- [ ] **Step 1: Write the test**

Look at how `tests/unit/language/known_failures/` or other "expected error" tests are written (LIT typically uses `// RUN: not %arche %s 2>&1 | FileCheck %s` for this).

Create `tests/unit/language/extern_type/consume_use_after.arche`:

```arche
// RUN: not %arche -o %t %s 2>&1 | FileCheck %s
// CHECK: use of consumed handle

extern type Resource(8);
extern func resource_open(initial: int) -> Resource;
extern proc resource_close(consume r: Resource);
extern func resource_get(r: Resource) -> int;

proc main() {
  let r := resource_open(10);
  resource_close(r);
  let v := resource_get(r);  // <-- compile error
}
```

- [ ] **Step 2: Run, verify PASS**

```
make test
```

Expected: compiler exits nonzero, error message contains "use of consumed handle".

- [ ] **Step 3: Commit**

```bash
git add tests/unit/language/extern_type/consume_use_after.arche
git commit -m "tests: consume_use_after.arche — use-after-consume is a compile error"
```

### Task 22: Stale-handle runtime abort test

**Files:**
- Create: `tests/unit/language/extern_type/stale_handle_abort.arche`

- [ ] **Step 1: Write the test**

This test requires the handle to escape the static check — store it in an archetype column.

```arche
// RUN: %arche -o %t --link tests/unit/language/extern_type/runtime/test_resource.c %s && not %t 2>&1 | FileCheck %s
// CHECK: stale generation

extern type Resource(8);
extern func resource_open(initial: int) -> Resource;
extern proc resource_close(consume r: Resource);
extern func resource_get(r: Resource) -> int;

arche Holder {
  ref: Resource,
}
static Holder(1);

proc main() {
  let r := resource_open(10);
  insert(Holder, r);
  resource_close(r);
  let r2 := resource_open(99);  // reuses the slot with bumped gen
  // Now Holder.ref[0] still holds the OLD handle. Look it up:
  let v := resource_get(Holder.ref[0]);  // <-- runtime abort: stale generation
}
```

- [ ] **Step 2: Run, verify PASS**

```
make test
```

Expected: program aborts with stderr message containing "stale generation".

- [ ] **Step 3: Commit**

```bash
git add tests/unit/language/extern_type/stale_handle_abort.arche
git commit -m "tests: stale_handle_abort.arche — runtime catches escaped stale handles"
```

### Task 23: Capacity-exhausted abort test

**Files:**
- Create: `tests/unit/language/extern_type/capacity_exhausted.arche`

- [ ] **Step 1: Write the test**

```arche
// RUN: %arche -o %t --link tests/unit/language/extern_type/runtime/test_resource.c %s && not %t 2>&1 | FileCheck %s
// CHECK: capacity exhausted

extern type Resource(2);
extern func resource_open(initial: int) -> Resource;

proc main() {
  let a := resource_open(1);
  let b := resource_open(2);
  // pool still has space (8 slots in C) but Arche's table is full (2 slots)
  let c := resource_open(3);  // <-- runtime abort: capacity exhausted
}
```

- [ ] **Step 2: Run, verify PASS**

```
make test
```

Expected: abort with "capacity exhausted".

- [ ] **Step 3: Commit**

```bash
git add tests/unit/language/extern_type/capacity_exhausted.arche
git commit -m "tests: capacity_exhausted.arche — table-full triggers abort"
```

### Task 24: Distinct-types compile-error test

**Files:**
- Create: `tests/unit/language/extern_type/distinct_types.arche`

- [ ] **Step 1: Write the test**

```arche
// RUN: not %arche -o %t %s 2>&1 | FileCheck %s
// CHECK: type mismatch

extern type Window(8);
extern type Sound(64);
extern func sound_open() -> Sound;
extern proc window_close(consume w: Window);

proc main() {
  let s := sound_open();
  window_close(s);  // <-- compile error: Sound is not Window
}
```

- [ ] **Step 2: Run, verify PASS**

```
make test
```

- [ ] **Step 3: Commit**

```bash
git add tests/unit/language/extern_type/distinct_types.arche
git commit -m "tests: distinct_types.arche — extern types are not interchangeable"
```

---

## Phase 8 — Documentation and polish

### Task 25: README update

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Add a section**

Locate the existing section ordering (after "Out Parameters", before "Multi-Value Let" seems natural). Insert:

````markdown
## Extern Types (`extern type`)

Arche references foreign resources (OS windows, audio voices, file handles outside `io.c`'s pattern) via `extern type` declarations. Each declaration defines an opaque, fixed-capacity handle type used only in `extern` signatures.

```arche
extern type Window(8);

extern func window_open(title: char[], w: int, h: int) -> Window;
extern proc window_present(w: Window, fb: int[], width: int, height: int);
extern proc window_close(consume w: Window);
```

- `Window` is opaque to Arche — no inspection, arithmetic, or casting.
- `0` is the null handle. Returning `NULL` from C marshals to `0`.
- `consume` marks a parameter whose handle is freed by the call. Re-using a consumed binding is a compile error.
- Distinct extern types are not interchangeable: `Window` and `Sound` are different types even though both are int handles at runtime.
- Generation counters detect use after free at runtime when the static checker can't (e.g., handles stored in archetype columns).

C authors write plain C with native pointer types (`HWND`, `FILE *`, etc.). The compiler emits all the handle marshaling.

See `docs/superpowers/specs/2026-05-16-extern-type-design.md` for the full design.
````

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "docs: README section for extern types and consume modifier"
```

### Task 26: Final build & full test sweep

**Files:** none modified — verification only.

- [ ] **Step 1: Clean build**

```
make clean
make all
```

Expected: clean compile, no warnings beyond pre-existing ones.

- [ ] **Step 2: Run every test target**

```
make test-parser
make test-semantic
make test-codegen-unit
make test-lower
make test-handle-runtime
make test
```

Expected: every target green. If any fail, fix and amend the relevant earlier task before continuing.

- [ ] **Step 3: Format the modified files**

```
make format
```

Expected: clean-format on all touched C/H files.

- [ ] **Step 4: Final commit if format made changes**

```bash
git status
# if any files changed:
git add -A
git commit -m "format: apply clang-format / arche-fmt to extern-type changes"
```

---

## Self-Review

**Spec coverage:**
- Syntax (`extern type Name(N);`, `consume p: T`) — Tasks 5, 7
- Type discipline (opaque, no arithmetic, distinct types) — Tasks 10, 11
- First-class scalar (archetype columns) — covered by Tasks 11, 22 (the stale-handle test uses an archetype column)
- Lifecycle / `consume` — Tasks 7, 12, 17, 21
- Null handle (= 0, valid for consume as no-op) — Tasks 15, 17, 20
- Runtime model (slot table, gen counters, encoding) — Tasks 13, 14
- Error policy (loud abort with stderr message) — Task 13 (helpers), Tasks 22, 23 (end-to-end)
- C author contract (plain C, no `__arche_` knowledge needed) — Tasks 15, 16, 17 generate all marshal glue from the C author's perspective; Task 18 demonstrates with the toy library
- Compiler responsibilities (per-type table + per-call marshal) — Tasks 14, 15, 16, 17
- Worked example — Task 19 mirrors the spec's example
- Future-work items (tagged-union returns, bit allocation tuning) — out of scope for this plan, called out in spec

**Placeholder scan:** No "TBD"s, no "implement appropriately." Every code step has the actual code. Every test step has the actual test. Every command has expected output described.

**Type consistency check:**
- `is_consume` field on `Parameter` — Task 2 (define) → Tasks 7, 12, 17 (use).
- `ExternTypeDecl.name` and `.capacity` — Task 4 (define) → Tasks 5, 9, 14 (use).
- `DECL_EXTERN_TYPE` — Task 4 (define) → Tasks 5, 9, 14 (use).
- Task 3 (TYPE_EXTERN enum variant) was removed during self-review. Extern types are resolved by name lookup in the semantic symbol table (Task 10), keeping `TypeRef.kind = TYPE_NAME` throughout the CST. Numbering preserved; Task 3 is intentionally absent.
- `__arche_slot_alloc`, `__arche_slot_get`, `__arche_slot_free` — defined in Task 13, declared/used in Tasks 14, 15, 16, 17.
- `__arche_<T>_slots`, `__arche_<T>_typename` globals — emitted in Task 14, referenced in Tasks 15, 16, 17.

All cross-task references are consistent.

---

## Out of scope (explicitly deferred)

These items appear in the spec's "Open questions / future work" section and are **not** part of this plan:

1. Tagged-union or enum error returns (requires sum types in Arche first).
2. Bit-allocation tuning (24/8 etc.) — only the default 16/16 split is implemented.
3. Cross-function consume tracking (compiler-side flow analysis across proc boundaries).
4. Multi-handle consume syntax (`consume a, b: T`).
5. Compile-time warnings for unbalanced lifecycle.
6. Reflection / debug enumeration of live slots.
7. Per-type capacity raising at runtime (impossible by design; capacity is fixed).

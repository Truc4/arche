#ifndef TYPE_REF_H
#define TYPE_REF_H

#include "../lexer/lexer.h" /* Trivia, Token (relied on transitively by includers) */
#include <stddef.h>

/* The abstract AST (AstProgram / Decl / Statement / Expression) was deleted in the AST-kill (Phase 2)
 * and the TypeRef type tree in Phase 3 (types are now the interned TypeId of semantic/sem_types.h).
 * What remains here is the set of shared enums the syntax-view-driven analysis and lowering still key
 * on (DeclKind / StaticKind / FieldKind / Operator / ExpressionType / UnaryOperator) and SourceLoc. */

/* =========================
   Source location
   ========================= */

typedef struct {
	int line;
	int column;
} SourceLoc;

/* =========================
   Declaration kinds
   ========================= */

typedef enum {
	DECL_WORLD,
	DECL_ARCHETYPE,
	DECL_PROC,
	DECL_SYS,
	DECL_SYSTEM, /* `Name :: system { body }` — the composer (distinct from DECL_SYS = map) */
	DECL_EACH,   /* `Name :: each(<query>) { body }` — the per-element fan (scalars, control flow + effects) */
	DECL_FUNC,
	DECL_FUNC_GROUP,
	DECL_STATIC,
	DECL_CONST,
	DECL_USE,
	DECL_ENUM,
	DECL_SUM,   /* `Name :: sum { a(T), b([]Self) }` — a tagged-union type; compile-time only for now */
	DECL_QUERY, /* `Name :: query {cols}` — a compile-time archetype selector; emits no symbol */
} DeclKind;

typedef enum {
	STATIC_KIND_ARCHETYPE,
	STATIC_KIND_ARRAY,
	STATIC_KIND_SCALAR,
} StaticKind;

/* =========================
   Archetype field kinds
   ========================= */

typedef enum {
	FIELD_META,   /* one value for whole archetype */
	FIELD_COLUMN, /* one value per element, aligned with size */
} FieldKind;

/* =========================
   Operators
   ========================= */

typedef enum {
	OP_NONE,
	OP_ADD,
	OP_SUB,
	OP_MUL,
	OP_DIV,
	OP_MOD,
	OP_EQ,
	OP_NEQ,
	OP_LT,
	OP_GT,
	OP_LTE,
	OP_GTE,
	OP_AND,  /* && — logical, eager (arche exprs have no side effects) */
	OP_OR,   /* || */
	OP_FMAP, /* |> — fmap a pure func over an Eff's out-slots (the applicative result-map) */
} Operator;

/* =========================
   Expression kinds
   ========================= */

typedef enum {
	EXPR_LITERAL,
	EXPR_NAME,
	EXPR_FIELD, /* player.pos */
	EXPR_INDEX, /* grid[x, y], player.pos[i] */
	EXPR_SLICE, /* buf[lo:hi] — a read-only borrowed sub-view; lo/hi optional (buf[:hi], buf[lo:], buf[:]) */
	EXPR_BINARY,
	EXPR_UNARY,
	EXPR_CALL,
	EXPR_ALLOC,
	EXPR_ARRAY_LITERAL,
	EXPR_STRING,
} ExpressionType;

typedef enum {
	UNARY_NEG,
	UNARY_NOT,
	UNARY_MOVE, /* `move x` — call-site ownership transfer; transparent value, marks x consumed */
	UNARY_COPY, /* `copy x` — call-site duplication; caller keeps the original (x not consumed) */
} UnaryOperator;

#include "../lexer/lexer.h"
#include <stdio.h> /* widely relied on transitively by includers (sprintf/FILE*) */

#endif /* TYPE_REF_H */

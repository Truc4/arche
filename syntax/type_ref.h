#ifndef TYPE_REF_H
#define TYPE_REF_H

#include "../lexer/lexer.h" /* Trivia, Token (relied on transitively by includers) */
#include <stddef.h>

/* The abstract AST (AstProgram / Decl / Statement / Expression) was deleted in the AST-kill (Phase 2):
 * the lossless syntax tree + SemModel side table + the resolved DeclSummary table are now the single
 * source of truth. What remains here is the TYPE representation (TypeRef) plus the shared enums that
 * the syntax-view-driven analysis and lowering still key on (DeclKind / StaticKind / FieldKind /
 * Operator / ExpressionType / UnaryOperator) and SourceLoc. Phase 3 migrates TypeRef to TypeId and
 * this file is renamed accordingly. */

typedef struct TypeRef TypeRef;

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
	DECL_FUNC,
	DECL_FUNC_GROUP,
	DECL_STATIC,
	DECL_CONST,
	DECL_USE,
	DECL_ENUM,
} DeclKind;

typedef enum {
	STATIC_KIND_ARCHETYPE,
	STATIC_KIND_ARRAY,
	STATIC_KIND_SCALAR,
} StaticKind;

/* =========================
   Types
   ========================= */

typedef enum {
	TYPE_NAME,         /* int, float, char, Vec3, Player, etc. */
	TYPE_ARRAY,        /* nested / jagged array */
	TYPE_SHAPED_ARRAY, /* dense ranked array */
	TYPE_TUPLE,        /* tuple: (x: float, y: float) */
	TYPE_HANDLE,       /* handle(ArchetypeName) */
	TYPE_ARCHETYPE,    /* bare-category `archetype` (parameter type only) */
	TYPE_OPAQUE,       /* opaque: pointer-width C-owned cell, never read/written/forged by Arche */
	TYPE_TYPE,         /* `type`: the meta-type (type-of-types). A type alias is a constant of this
	                      type; compile-time only, erased before lowering. */
	TYPE_PROC,         /* a proc type (bodiless signature): `proc(in)(out)` — structural */
	TYPE_FUNC,         /* a func type (bodiless signature): `func(in) -> T` — structural */
} TypeKind;

struct TypeRef {
	TypeKind kind;
	SourceLoc loc;
	/* TYPE_NAME only: 1 if the alias declaration was marked `:: alias T` (tier-1, transparent —
	 * same identity as the backing). 0 (default) is the tier-2 subtype: a distinct nominal type
	 * usable as its backing. Ignored for non-name kinds. */
	int is_transparent;
	union {
		char *name;

		struct {
			TypeRef *element_type;
		} array;

		struct {
			TypeRef *element_type;
			int rank;
		} shaped_array;

		struct {
			char **field_names;
			TypeRef **field_types;
			int field_count;
		} tuple;

		struct {
			char *archetype_name;
		} handle;

		/* TYPE_PROC / TYPE_FUNC: a callable signature. `results` are a proc's out-params or a
		 * func's single return. Matched structurally (names ignored). */
		struct {
			TypeRef **param_types;
			int param_count;
			TypeRef **result_types;
			int result_count;
			int is_proc;
		} callable;
	} data;
};

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
	OP_EQ,
	OP_NEQ,
	OP_LT,
	OP_GT,
	OP_LTE,
	OP_GTE,
	OP_AND, /* && — logical, eager (arche exprs have no side effects) */
	OP_OR,  /* || */
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

/* =========================
   TypeRef constructors / destructor
   ========================= */

TypeRef *type_name_create(char *name);
TypeRef *type_array_create(TypeRef *element_type);
TypeRef *type_shaped_array_create(TypeRef *element_type, int rank);
void type_ref_free(TypeRef *type);

#include "../lexer/lexer.h"
#include <stdio.h> /* widely relied on transitively by includers (sprintf/FILE*) */

#endif /* TYPE_REF_H */

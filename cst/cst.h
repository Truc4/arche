#ifndef CST_H
#define CST_H

#include "../lexer/lexer.h" /* Trivia, Token */
#include "syntax_tree.h"    /* SyntaxNode (transient Expression->cst_node link) */
#include <stddef.h>

/* =========================
   Forward declarations
   ========================= */

typedef struct AstProgram AstProgram;
typedef struct Decl Decl;
typedef struct WorldDecl WorldDecl;
typedef struct ArchetypeDecl ArchetypeDecl;
typedef struct ProcDecl ProcDecl;
typedef struct SysDecl SysDecl;
typedef struct FuncDecl FuncDecl;
typedef struct FuncGroup FuncGroup;
typedef struct Parameter Parameter;
typedef struct FieldDecl FieldDecl;
typedef struct TypeRef TypeRef;
typedef struct Statement Statement;
typedef struct Expression Expression;
typedef struct StaticArrayDecl StaticArrayDecl;
typedef struct UseDecl UseDecl;

/* =========================
   Source location
   ========================= */

typedef struct {
	int line;
	int column;
} SourceLoc;

/* =========================
   AstProgram / declarations
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

struct AstProgram {
	Decl **decls;
	int decl_count;
	SourceLoc loc;
};

typedef struct {
	StaticKind kind;
	union {
		struct {
			char *archetype_name;
			char **field_names;
			Expression **field_values;
			int field_count;
			Expression *init_length;
		} archetype;
		struct {
			char *name;
			TypeRef *element_type;
			int size;
			Expression *init; /* constant array initializer, or NULL = zero-init */
		} array;
		struct {
			char *name;
			TypeRef *type;    /* declared scalar type (inferred forms still carry a resolved type) */
			Expression *init; /* compile-time-constant initial value; normalized from implicit `= 0` */
		} scalar;
	};
} StaticDecl;

struct UseDecl {
	char *name; /* module name, e.g. "csv" from `use csv;` */
};

/* A classical enum: a distinct int-backed type with named variants. Variants are compile-time
 * int constants (explicit `= N` or auto-incrementing). Erased before codegen (variants become
 * literals; the type resolves to int). */
typedef struct {
	char *name;
	char **variant_names;
	long *variant_values;
	int variant_count;
} EnumDecl;

typedef struct {
	char *name;
	Expression *value;   /* literal RHS (value const) or a bare name (simple type alias) */
	TypeRef *type_value; /* set when the RHS is a type form (e.g. a tuple) — a nominal type alias */
	TypeRef *decl_type;  /* the explicit `T` in `name : T : value` (the declared meta/type); NULL
	                        for the elided `name :: value` form. `T == TYPE_TYPE` marks an alias. */
	int is_transparent;  /* `name :: alias T` — tier-1 transparent alias (same type as backing).
	                        0 (default) is the tier-2 distinct subtype. */
} ConstDecl;

struct Decl {
	DeclKind kind;
	SourceLoc loc;
	/* Trivia attached to this declaration. Leading = comments + blank-line
	 * runs that appeared before this decl's first syntactic token. Trailing =
	 * inline comments on the same line as this decl's last syntactic token.
	 * Owned by the Decl; freed by decl_free. */
	Trivia *leading_trivia;
	int leading_count;
	Trivia *trailing_trivia;
	int trailing_count;
	int last_line; /* line of this decl's last syntactic token */
	/* `@allow(<slug>)` suppressions in the order they appeared on the decl.
	 * Each entry is a diagnostic slug (e.g. "unused-local"). Semantic analysis
	 * pushes the set when entering this decl so its lints can be silenced.
	 * Errors are NEVER suppressible — silently ignored if listed here. */
	char **allow_slugs;
	int allow_slug_count;
	/* 1 if a `@drop(<type>)` decorator was on this decl. Marks the decorated proc as the
	 * destructor for that opaque type (RAII). `drop_type` is the named type (e.g. "socket"),
	 * validated in semantic to match the proc's single `own` parameter. */
	int is_drop;
	char *drop_type;
	union {
		WorldDecl *world;
		ArchetypeDecl *archetype;
		ProcDecl *proc;
		SysDecl *sys;
		FuncDecl *func;
		FuncGroup *func_group;
		StaticDecl *static_decl;
		ConstDecl *constant;
		UseDecl *use;
		EnumDecl *enum_decl;
	} data;
};

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
   Worlds
   ========================= */

struct WorldDecl {
	char *name;
	char **field_names; /* optional fields for the world itself */
	int field_count;
	SourceLoc loc;
};

/* =========================
   Archetypes
   ========================= */

typedef enum {
	FIELD_META,   /* one value for whole archetype */
	FIELD_COLUMN, /* one value per element, aligned with size */
} FieldKind;

struct FieldDecl {
	FieldKind kind;
	char *name;
	TypeRef *type;
	int meta_explicit; /* 1 if written with the explicit meta longhand `name : type : T` (vs `name :: T`) */
	SourceLoc loc;
	Trivia *leading_trivia;
	int leading_count;
	Trivia *trailing_trivia;
	int trailing_count;
};

struct ArchetypeDecl {
	char *name;
	FieldDecl **fields;
	int field_count;
	SourceLoc loc;
};

struct ProcDecl {
	char *name;
	Parameter **params;
	int param_count;
	/* A proc has no return value — it declares its results as OUT-PARAMETERS: caller-provided
	 * places written in place (the `(out)` of `proc f(in)(out)`). An out-param is owned by
	 * definition (no `own` keyword). A name appearing in both `params` and `out_params` is an
	 * in-out param: read in, mutated, handed back (used mainly for extern C signatures). */
	Parameter **out_params;
	int out_param_count;
	int is_extern;
	int is_variadic; /* 1 if last param is `...`; only valid on extern decls. */
	Statement **statements;
	int statement_count;
	int end_line;
	SourceLoc loc;
	int allow_pure_proc; /* 1 if @allow_pure_proc was on the decl; suppresses proc-could-be-func */
};

struct Parameter {
	char *name;
	TypeRef *type;
	int is_own; /* `own` param: function owns it (may mutate/consume); caller passes via `move` or `copy` */
	SourceLoc loc;
};

struct SysDecl {
	char *name;
	Parameter **params;
	int param_count;
	Statement **statements;
	int statement_count;
	int end_line;
	SourceLoc loc;
};

struct FuncDecl {
	char *name;
	Parameter **params;
	int param_count;
	/* A function's return is a list of types; a single return is just count == 1. `-> (T1, …, Tn)`
	 * with n > 1 is returned as an aggregate. (No scalar special-case.) */
	TypeRef **return_types;
	int return_type_count;
	int is_extern;
	int is_variadic; /* 1 if last param is `...`; only valid on extern decls (printf etc.). */
	Statement **statements;
	int statement_count;
	int end_line;
	SourceLoc loc;
};

struct FuncGroup {
	char *name;
	char **member_names;
	int member_count;
	SourceLoc loc;
};

/* =========================
   Statements
   ========================= */

typedef enum {
	STMT_BIND,
	STMT_ASSIGN,
	STMT_FOR,
	STMT_IF,
	STMT_BREAK,
	STMT_CONTINUE,
	STMT_RUN,
	STMT_EXPR,
	STMT_RETURN,
	STMT_MULTI_BIND,
	STMT_EACH_FIELD,
} StatementType;

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

typedef struct {
	char *name;          /* single var name (backward compat) */
	char **names;        /* multiple var names for multi-value let */
	int name_count;      /* 0 = use .name, >0 = use .names[] */
	TypeRef *type;       /* optional, may be NULL — only for single-var */
	Expression *value;   /* optional, may be NULL */
	int is_const;        /* 1 = an immutable local constant (`k :: e` / `k : T : e`) */
	TypeRef *type_value; /* the type RHS of a `k : type : T` local type alias, else NULL */
	int is_type_alias;   /* set by semantic: this const's RHS denotes a type → erased, no runtime */
	int is_transparent;  /* `k :: alias T` — tier-1 transparent local alias; 0 = tier-2 subtype */
} BindStmt;

typedef struct {
	Expression *target; /* must be assignable: name, field, or index */
	Expression *value;
	Operator op; /* OP_NONE for plain =, OP_ADD for +=, etc. */
} AssignStmt;

typedef struct {
	char *var_name;        /* NULL for non-range-based for loops */
	Expression *iterable;  /* NULL for non-range-based for loops */
	Statement *init;       /* NULL unless three-part for loop */
	Expression *condition; /* NULL for infinite loops or range-based */
	Statement *increment;  /* NULL unless three-part for loop - can be assign or expr stmt */
	Statement **body;
	int body_count;
} ForStmt;

typedef struct {
	Expression *cond;
	Statement **then_body;
	int then_count;
	Statement **else_body;
	int else_count;
} IfStmt;

typedef struct {
	char *system_name;
	char *world_name;
} RunStmt;

typedef struct {
	Expression *expr;
} ExprStmt;

typedef struct {
	/* Returned values, in order; a single return is just count == 1. */
	Expression **values;
	int count;
} ReturnStmt;

typedef struct {
	char *name;
	int is_new;    /* 1 = let (declare), 0 = assign to existing */
	TypeRef *type; /* optional explicit type, only valid when is_new=1 */
} BindingTarget;

typedef struct {
	BindingTarget *targets;
	int target_count;
	Expression *value;
	int from_shorthand;
} MultiBindStmt;

typedef struct {
	char *binding_name;    /* `f` in `each_field f in arch` */
	TypeRef *filter_type;  /* optional `: T` filter; NULL = walk every field */
	char *arch_param_name; /* identifier on the right of `in` — must be archetype param at semantic time */
	Statement **body;
	int body_count;
} EachFieldStmt;

struct Statement {
	StatementType type;
	SourceLoc loc;
	Trivia *leading_trivia;
	int leading_count;
	Trivia *trailing_trivia;
	int trailing_count;
	int last_line;   /* line of this statement's last syntactic token */
	uint32_t cst_id; /* TRANSIENT: CST node id (+1; 0 = unlinked) — see Expression.cst_id */
	union {
		BindStmt bind_stmt;
		AssignStmt assign_stmt;
		ForStmt for_stmt;
		IfStmt if_stmt;
		RunStmt run_stmt;
		ExprStmt expr_stmt;
		ReturnStmt return_stmt;
		MultiBindStmt multi_bind;
		EachFieldStmt each_field;
	} data;
};

/* =========================
   Expressions
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

typedef struct {
	char *lexeme;
} LiteralExpr;

typedef struct {
	char *name;
	int is_table_ref; /* 1 if written `table<name>` (value-position table reference); formats back as table<...> */
} NameExpr;

typedef struct {
	Expression *base;
	char *field_name;
} FieldExpr;

typedef struct {
	Expression *base;
	Expression **indices;
	int index_count;
} IndexExpr;

/* buf[lo:hi] — a read-only borrowed sub-slice. lo/hi are NULL when omitted (`buf[:hi]` lo=NULL,
 * `buf[lo:]` hi=NULL, `buf[:]` both NULL → defaults 0 and length). */
typedef struct {
	Expression *base;
	Expression *lo;
	Expression *hi;
} SliceExpr;

typedef struct {
	Operator op;
	Expression *left;
	Expression *right;
} BinaryExpr;

typedef struct {
	UnaryOperator op;
	Expression *operand;
} UnaryExpr;

typedef struct {
	Expression *callee;
	Expression **args;
	int arg_count;
} CallExpr;

typedef struct {
	char *archetype_name;
	char **field_names;
	Expression **field_values;
	int field_count;
	Expression *init_length; /* second arg: how many rows to initialize; NULL = use capacity */
} AllocExpr;

typedef struct {
	Expression **elements;
	int element_count;
} ArrayLiteralExpr;

typedef struct {
	char *value; /* String content without quotes */
	int length;  /* Length excluding quotes */
} StringExpr;

struct Expression {
	ExpressionType type;
	SourceLoc loc;
	union {
		LiteralExpr literal;
		NameExpr name;
		FieldExpr field;
		IndexExpr index;
		SliceExpr slice;
		BinaryExpr binary;
		UnaryExpr unary;
		CallExpr call;
		AllocExpr alloc;
		ArrayLiteralExpr array_literal;
		StringExpr string;
	} data;
	char *resolved_type; /* Semantic analysis populates: "int", "double", "Vec3", etc. NULL if not yet analyzed */
	/* TRANSIENT migration scaffolding (removed in the final stage): id of the CST
	 * node this expression was parsed from (stored +1, so 0 means "not linked").
	 * Stored as a value, not a pointer, so it survives the CST being freed. Lets
	 * semantic/lower key a side model instead of mutating resolved_type. */
	uint32_t cst_id;
};

/* =========================
   Constructors
   ========================= */

AstProgram *ast_program_create(void);
Decl *decl_create(DeclKind kind);

WorldDecl *world_decl_create(char *name);
ArchetypeDecl *archetype_decl_create(char *name);
ProcDecl *proc_decl_create(char *name);
SysDecl *sys_decl_create(char *name);
FuncDecl *func_decl_create(char *name);
FuncGroup *func_group_create(char *name);
void func_group_free(FuncGroup *group);
ConstDecl *const_decl_create(char *name, Expression *value);
StaticDecl *static_decl_archetype_create(char *archetype_name);
StaticDecl *static_decl_array_create(char *name, TypeRef *element_type, int size);
StaticDecl *static_decl_scalar_create(char *name, TypeRef *type, Expression *init);
UseDecl *use_decl_create(char *name);
Parameter *parameter_create(char *name, TypeRef *type);
FieldDecl *field_decl_create(FieldKind kind, char *name, TypeRef *type);

TypeRef *type_name_create(char *name);
TypeRef *type_array_create(TypeRef *element_type);
TypeRef *type_shaped_array_create(TypeRef *element_type, int rank);

Statement *statement_create(StatementType type);
Expression *expression_create(ExpressionType type);

/* =========================
   Destructors
   ========================= */

void ast_program_free(AstProgram *prog);
void decl_free(Decl *decl);

void world_decl_free(WorldDecl *world);
void archetype_decl_free(ArchetypeDecl *archetype);
void proc_decl_free(ProcDecl *proc);
void sys_decl_free(SysDecl *sys);
void func_decl_free(FuncDecl *func);
void parameter_free(Parameter *param);
void field_decl_free(FieldDecl *field);
void static_decl_free(StaticDecl *s);
void use_decl_free(UseDecl *use);
void type_ref_free(TypeRef *type);

void statement_free(Statement *stmt);
void expression_free(Expression *expr);

/* =========================
   Formatting / Pretty-printing
   ========================= */

#include "../lexer/lexer.h"
#include <stdio.h> /* widely relied on transitively by includers (sprintf/FILE*) */

#endif /* CST_H */

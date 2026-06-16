#ifndef HIR_H
#define HIR_H

#include "../syntax/type_ref.h" /* SourceLoc, Operator, UnaryOperator */
#include <stddef.h>

/* =========================
   Type system
   ========================= */

typedef enum {
	HIR_TYPE_UNKNOWN = 0,
	HIR_TYPE_INT,
	HIR_TYPE_FLOAT,
	HIR_TYPE_CHAR,
	HIR_TYPE_VOID,
	HIR_TYPE_CHAR_ARRAY,
	HIR_TYPE_HANDLE,
	HIR_TYPE_NAMED, /* archetype or user-defined; .name points into syntax tree */
	HIR_TYPE_ARRAY, /* element array */
	HIR_TYPE_SHAPED_ARRAY,
	HIR_TYPE_TUPLE,
	HIR_TYPE_ARCHETYPE, /* bare-category `archetype` parameter type */
	HIR_TYPE_OPAQUE,    /* opaque: pointer-width C-owned cell */
	HIR_TYPE_FUNC,      /* a callable value: proc/func type (structural) */
} HirTypeTag;

typedef struct HirType HirType;
typedef struct HirTupleField HirTupleField;

struct HirTupleField {
	const char *name;
	HirType *type;
};

struct HirType {
	HirTypeTag tag;
	const char *name;      /* HIR_TYPE_NAMED: archetype name (ptr into syntax tree) */
	struct HirType *elem;  /* HIR_TYPE_ARRAY / HIR_TYPE_SHAPED_ARRAY */
	int rank;              /* HIR_TYPE_SHAPED_ARRAY */
	HirTupleField *fields; /* HIR_TYPE_TUPLE */
	int field_count;       /* HIR_TYPE_TUPLE */
	int int_width;         /* HIR_TYPE_INT: 8/16/32/64/128 (default 32) */
	int int_signed;        /* HIR_TYPE_INT: 1 signed, 0 unsigned (default 1) */
	/* HIR_TYPE_FUNC: a callable signature. `params`/`results` are owned arrays; `results` are a
	 * proc's out-params or a func's single return. */
	HirType **params;
	int param_count;
	HirType **results;
	int result_count;
	int is_proc;
};

/* =========================
   Forward declarations
   ========================= */

typedef struct HirProgram HirProgram;
typedef struct HirDecl HirDecl;
typedef struct HirStmt HirStmt;
typedef struct HirExpr HirExpr;
typedef struct HirParam HirParam;
typedef struct HirField HirField;

/* =========================
   Declarations
   ========================= */

typedef enum {
	HIR_DECL_WORLD,
	HIR_DECL_ARCHETYPE,
	HIR_DECL_PROC,
	HIR_DECL_MAP,
	HIR_DECL_FUNC,
	HIR_DECL_FUNC_GROUP,
	HIR_DECL_STATIC,
	HIR_DECL_CONST,
	HIR_DECL_DEFAULT, /* `@default(<kind>, <category>, <policy>)` program default directive */
} HirDeclKind;

typedef enum {
	HIR_STATIC_ARCHETYPE,
	HIR_STATIC_ARRAY,
	HIR_STATIC_SCALAR,
} HirStaticKind;

typedef struct {
	char *name;
} HirWorldDecl;

typedef struct {
	char *name;
	HirField **fields;
	int field_count;
} HirArchetypeDecl;

struct HirField {
	FieldKind kind;
	char *name;
	HirType *type;
	SourceLoc loc;
};

struct HirParam {
	char *name;
	HirType *type;
	int is_own; /* `own` param: function owns it (may mutate/consume); caller passes via `move` or `copy` */
	SourceLoc loc;
};

typedef struct {
	char *name;
	HirParam **params;
	int param_count;
	/* A proc has no return value — results are OUT-PARAMETERS written in place (the `(out)` of
	 * `proc f(in)(out)`). A name shared with `params` is an in-out param. count == 0 = no outputs. */
	HirParam **out_params;
	int out_param_count;
	int is_extern;
	int is_drop;      /* 1 if this proc is a `@drop` destructor (own opaque param is the type it destroys) */
	int is_intrinsic; /* 1 if `@intrinsic`: calls lower to a built-in instruction (e.g. raw syscall) */
	HirStmt **stmts;
	int stmt_count;
	SourceLoc loc;
} HirProcDecl;

typedef struct {
	char *name;
	HirParam **params;
	int param_count;
	HirStmt **stmts;
	int stmt_count;
	int is_gpu; /* 1 if `@gpu`: the kernel is emitted as a GPU compute shader (SSBO per column) */
	SourceLoc loc;
} HirMapDecl;

typedef struct {
	char *name;
	char **member_names;
	int member_count;
	SourceLoc loc;
} HirFuncGroupDecl;

typedef struct {
	char *name;
	HirParam **params;
	int param_count;
	/* A function's return is a list of types; a single return is just count == 1, and a
	 * multi-return (count > 1) is handed back as an aggregate. No scalar special-case. */
	HirType **return_types;
	int return_type_count;
	int is_extern;
	int is_policy;       /* lowered from a `policy` form: a failure-policy MACRO, inlined at fallible op sites
	                      * (operands bound as mutable locals), never emitted as its own LLVM function. */
	int policy_category; /* for a policy: 1=bounds (index/slice), 2=pool (insert), 3=divide. So a `clamp`
	                      * (bounds) and an `abort` (both) resolve by name AND category. 0 = unset. */
	HirStmt **stmts;
	int stmt_count;
	SourceLoc loc;
} HirFuncDecl;

typedef struct {
	HirStaticKind kind;
	/* A pool decl lowered from a device datasheet (`.ds.arche`) is a storage REQUIREMENT (minimum
	 * rows the driver must provide), not an allocation: it never emits storage; the driver's own pool
	 * for the same shape must meet the minimum. Set only for HIR_STATIC_ARCHETYPE from a datasheet. */
	int is_requirement;
	union {
		struct {
			char *archetype_name;
			char **field_names;
			HirExpr **field_values;
			int field_count;
			HirExpr *init_length;
			char *overflow_policy; /* `Foo[N] ?handler`: the pool's default insert overflow handler, or NULL */
		} archetype;
		struct {
			char *name;
			HirType *element_type;
			int size;       /* total flat element count (N*M for an N×M matrix) */
			int row_stride; /* N-D: inner-dimension product (row width); 0/1 = plain 1-D */
			HirExpr *init;  /* constant array initializer, or NULL = zero-init */
		} array;
		struct {
			char *name;
			HirType *type;
			HirExpr *init; /* compile-time-constant initial value (implicit `= 0` normalized in) */
		} scalar;
	};
} HirStaticDecl;

typedef struct {
	char *name;
	HirExpr *value;
	HirType *type; /* explicit declared type from `name : T : value` (concrete T); NULL if inferred */
} HirConstDecl;

/* `@default(<kind>, <category>, <policy>)`: the program's failure-policy default for one
 * (effect-kind, op-category) cell. At most one per cell program-wide. */
typedef struct {
	int effect_kind; /* 0 = proc, 1 = func */
	int category;    /* 1 = bounds, 2 = pool, 3 = divide */
	char *policy;    /* the named policy (resolved in core or user code) */
	SourceLoc loc;
} HirDefaultDecl;

struct HirDecl {
	HirDeclKind kind;
	SourceLoc loc;
	int unit; /* owning compilation unit: 0 = entry/root program, >0 = an inlined module. Lets codegen
	           * group/emit per unit (per-unit objects + linkonce_odr shared monomorph instances). */
	union {
		HirWorldDecl *world;
		HirArchetypeDecl *archetype;
		HirProcDecl *proc;
		HirMapDecl *map;
		HirFuncDecl *func;
		HirFuncGroupDecl *func_group;
		HirStaticDecl *static_decl;
		HirConstDecl *constant;
		HirDefaultDecl *default_decl;
	} data;
};

struct HirProgram {
	HirDecl **decls;
	int decl_count;
	SourceLoc loc;
};

/* =========================
   Statements
   ========================= */

typedef enum {
	HIR_STMT_BIND,
	HIR_STMT_ASSIGN,
	HIR_STMT_FOR,
	HIR_STMT_IF,
	HIR_STMT_BREAK,
	HIR_STMT_CONTINUE,
	HIR_STMT_RUN,
	HIR_STMT_EXPR,
	HIR_STMT_RETURN,
	HIR_STMT_MULTI_BIND,
	HIR_STMT_EACH_FIELD,
	HIR_STMT_BLOCK, /* a scoped statement sequence (desugaring target, e.g. match) */
} HirStmtKind;

typedef struct {
	char **names; /* always names[], min 1 entry */
	int name_count;
	HirType *type; /* optional explicit type, only single-var */
	HirExpr *value;
} HirBindStmt;

typedef struct {
	HirExpr *target;
	HirExpr *value;
	Operator op;
} HirAssignStmt;

typedef struct {
	char *var_name;
	HirExpr *iterable;
	HirStmt *init;
	HirExpr *cond;
	HirStmt *incr;
	HirStmt **body;
	int body_count;
} HirForStmt;

typedef struct {
	HirExpr *cond;
	HirStmt **then_body;
	int then_count;
	HirStmt **else_body;
	int else_count;
} HirIfStmt;

typedef struct {
	char *map_name;
	char *world_name;
} HirRunStmt;

typedef struct {
	HirExpr *expr;
} HirExprStmt;

typedef struct {
	HirExpr **values; /* returned values, in order; a single return is just count == 1 */
	int count;
} HirReturnStmt;

typedef struct {
	char *name;
	int is_new;
	HirType *type;
} HirBindingTarget;

typedef struct {
	HirBindingTarget *targets;
	int target_count;
	HirExpr *value;
	int from_shorthand;
} HirMultiBindStmt;

typedef struct {
	char *binding_name;
	HirType *filter_type; /* may be NULL */
	char *arch_param_name;
	HirStmt **body;
	int body_count;
} HirEachFieldStmt;

typedef struct {
	HirStmt **stmts;
	int count;
} HirBlockStmt;

struct HirStmt {
	HirStmtKind kind;
	SourceLoc loc;
	union {
		HirBindStmt bind_stmt;
		HirAssignStmt assign_stmt;
		HirForStmt for_stmt;
		HirIfStmt if_stmt;
		HirRunStmt run_stmt;
		HirExprStmt expr_stmt;
		HirReturnStmt return_stmt;
		HirMultiBindStmt multi_bind;
		HirEachFieldStmt each_field;
		HirBlockStmt block;
	} data;
};

/* =========================
   Expressions
   ========================= */

typedef enum {
	HIR_EXPR_LITERAL,
	HIR_EXPR_NAME,
	HIR_EXPR_FIELD,
	HIR_EXPR_INDEX,
	HIR_EXPR_SLICE,
	HIR_EXPR_BINARY,
	HIR_EXPR_UNARY,
	HIR_EXPR_CALL,
	HIR_EXPR_ALLOC,
	HIR_EXPR_ARRAY_LITERAL,
	HIR_EXPR_STRING,
} HirExprKind;

struct HirExpr {
	HirExprKind kind;
	SourceLoc loc;
	HirType resolved; /* always populated by lowering */
	union {
		struct {
			char *lexeme;
		} literal;
		struct {
			char *name;
		} name;
		struct {
			HirExpr *base;
			char *field_name;
		} field;
		struct {
			HirExpr *base;
			HirExpr **indices;
			int index_count;
			char *policy;      /* `!name` failure policy (bounds category), or NULL → default (!abort) */
			int policy_elided; /* 1 if the bounds prover proved this in-bounds (SemModel verdict, stamped at
			                      lowering): codegen emits NO policy macro. The single elision authority. */
		} index;
		struct {
			HirExpr *base;
			HirExpr *lo;       /* NULL → 0 */
			HirExpr *hi;       /* NULL → base length */
			char *policy;      /* `!name` failure policy (bounds category), or NULL → default */
			int policy_elided; /* 1 if the bounds prover proved this in-bounds (SemModel verdict). */
		} slice;
		struct {
			Operator op;
			HirExpr *left;
			HirExpr *right;
			char *policy; /* `!name` div-by-zero failure policy (divide category), or NULL → default */
		} binary;
		struct {
			UnaryOperator op;
			HirExpr *operand;
		} unary;
		struct {
			HirExpr *callee;
			HirExpr **args;
			int arg_count;
			char *policy;          /* `?name` overflow handler on a pool `insert(...)`, or NULL → default */
			int policy_is_handler; /* 1 if the policy was written with `?` (handler), 0 if `!` (panic) */
		} call;
		struct {
			char *archetype_name;
			char **field_names;
			HirExpr **field_values;
			int field_count;
			HirExpr *init_length;
		} alloc;
		struct {
			HirExpr **elements;
			int element_count;
		} array_literal;
		struct {
			char *value;
			int length;
		} string;
	} data;
};

/* =========================
   Constructors / Destructors
   ========================= */

HirProgram *hir_program_create(void);
void hir_program_free(HirProgram *prog);

HirDecl *hir_decl_create(HirDeclKind kind);
void hir_decl_free(HirDecl *decl);

HirStmt *hir_stmt_create(HirStmtKind kind);
void hir_stmt_free(HirStmt *stmt);

HirExpr *hir_expr_create(HirExprKind kind);
void hir_expr_free(HirExpr *expr);

HirType *hir_type_create(HirTypeTag tag);
void hir_type_free(HirType *type);

/* Recognize a fixed-width int type name (byte, i8/u8 .. i64/u64, i128/u128).
 * Returns 1 and fills width (8/16/32/64/128) + signedness on match. */
int hir_parse_int_width(const char *s, int *width, int *is_signed);

HirField *hir_field_create(FieldKind kind, char *name, HirType *type);
void hir_field_free(HirField *field);

HirParam *hir_param_create(char *name, HirType *type);
void hir_param_free(HirParam *param);

#endif /* HIR_H */

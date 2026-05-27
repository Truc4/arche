#ifndef HIR_H
#define HIR_H

#include "../cst/cst.h" /* SourceLoc, Operator, UnaryOperator */
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
	HIR_TYPE_NAMED, /* archetype or user-defined; .name points into CST */
	HIR_TYPE_ARRAY, /* element array */
	HIR_TYPE_SHAPED_ARRAY,
	HIR_TYPE_TUPLE,
	HIR_TYPE_ARCHETYPE, /* bare-category `archetype` parameter type */
	HIR_TYPE_OPAQUE,    /* opaque: pointer-width C-owned cell */
} HirTypeTag;

typedef struct HirType HirType;
typedef struct HirTupleField HirTupleField;

struct HirTupleField {
	const char *name;
	HirType *type;
};

struct HirType {
	HirTypeTag tag;
	const char *name;      /* HIR_TYPE_NAMED: archetype name (ptr into CST) */
	struct HirType *elem;  /* HIR_TYPE_ARRAY / HIR_TYPE_SHAPED_ARRAY */
	int rank;              /* HIR_TYPE_SHAPED_ARRAY */
	HirTupleField *fields; /* HIR_TYPE_TUPLE */
	int field_count;       /* HIR_TYPE_TUPLE */
	int int_width;         /* HIR_TYPE_INT: 8/16/32/64/128 (default 32) */
	int int_signed;        /* HIR_TYPE_INT: 1 signed, 0 unsigned (default 1) */
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
	HIR_DECL_SYS,
	HIR_DECL_FUNC,
	HIR_DECL_FUNC_GROUP,
	HIR_DECL_STATIC,
	HIR_DECL_CONST,
} HirDeclKind;

typedef enum {
	HIR_STATIC_ARCHETYPE,
	HIR_STATIC_ARRAY,
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
	/* A proc may return a list of types (single = count 1; count > 1 = aggregate), like a func.
	 * count == 0 is a void proc. */
	HirType **return_types;
	int return_type_count;
	int is_extern;
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
	SourceLoc loc;
} HirSysDecl;

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
	HirStmt **stmts;
	int stmt_count;
	SourceLoc loc;
} HirFuncDecl;

typedef struct {
	HirStaticKind kind;
	union {
		struct {
			char *archetype_name;
			char **field_names;
			HirExpr **field_values;
			int field_count;
			HirExpr *init_length;
		} archetype;
		struct {
			char *name;
			HirType *element_type;
			int size;
		} array;
	};
} HirStaticDecl;

typedef struct {
	char *name;
	HirExpr *value;
	HirType *type; /* explicit declared type from `name : T : value` (concrete T); NULL if inferred */
} HirConstDecl;

struct HirDecl {
	HirDeclKind kind;
	SourceLoc loc;
	union {
		HirWorldDecl *world;
		HirArchetypeDecl *archetype;
		HirProcDecl *proc;
		HirSysDecl *sys;
		HirFuncDecl *func;
		HirFuncGroupDecl *func_group;
		HirStaticDecl *static_decl;
		HirConstDecl *constant;
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
	HIR_STMT_RUN,
	HIR_STMT_EXPR,
	HIR_STMT_FREE,
	HIR_STMT_RETURN,
	HIR_STMT_MULTI_BIND,
	HIR_STMT_EACH_FIELD,
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
	char *system_name;
	char *world_name;
} HirRunStmt;

typedef struct {
	HirExpr *expr;
} HirExprStmt;

typedef struct {
	HirExpr *value;
} HirFreeStmt;

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
		HirFreeStmt free_stmt;
		HirReturnStmt return_stmt;
		HirMultiBindStmt multi_bind;
		HirEachFieldStmt each_field;
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
		} index;
		struct {
			Operator op;
			HirExpr *left;
			HirExpr *right;
		} binary;
		struct {
			UnaryOperator op;
			HirExpr *operand;
		} unary;
		struct {
			HirExpr *callee;
			HirExpr **args;
			int arg_count;
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

#ifndef AST_H
#define AST_H

#include "../cst/cst.h" /* SourceLoc, Operator, UnaryOperator */
#include <stddef.h>

/* =========================
   Type system
   ========================= */

typedef enum {
	AST_TYPE_UNKNOWN = 0,
	AST_TYPE_INT,
	AST_TYPE_FLOAT,
	AST_TYPE_CHAR,
	AST_TYPE_VOID,
	AST_TYPE_CHAR_ARRAY,
	AST_TYPE_HANDLE,
	AST_TYPE_NAMED, /* archetype or user-defined; .name points into CST */
	AST_TYPE_ARRAY, /* element array */
	AST_TYPE_SHAPED_ARRAY,
	AST_TYPE_TUPLE,
	AST_TYPE_ARCHETYPE, /* bare-category `archetype` parameter type */
} AstTypeTag;

typedef struct AstType AstType;
typedef struct AstTupleField AstTupleField;

struct AstTupleField {
	const char *name;
	AstType *type;
};

struct AstType {
	AstTypeTag tag;
	const char *name;      /* AST_TYPE_NAMED: archetype name (ptr into CST) */
	struct AstType *elem;  /* AST_TYPE_ARRAY / AST_TYPE_SHAPED_ARRAY */
	int rank;              /* AST_TYPE_SHAPED_ARRAY */
	AstTupleField *fields; /* AST_TYPE_TUPLE */
	int field_count;       /* AST_TYPE_TUPLE */
};

/* =========================
   Forward declarations
   ========================= */

typedef struct AstProgram AstProgram;
typedef struct AstDecl AstDecl;
typedef struct AstStmt AstStmt;
typedef struct AstExpr AstExpr;
typedef struct AstParam AstParam;
typedef struct AstField AstField;

/* =========================
   Declarations
   ========================= */

typedef enum {
	AST_DECL_WORLD,
	AST_DECL_ARCHETYPE,
	AST_DECL_PROC,
	AST_DECL_SYS,
	AST_DECL_FUNC,
	AST_DECL_FUNC_GROUP,
	AST_DECL_STATIC,
	AST_DECL_CONST,
} AstDeclKind;

typedef enum {
	AST_STATIC_ARCHETYPE,
	AST_STATIC_ARRAY,
} AstStaticKind;

typedef struct {
	char *name;
} AstWorldDecl;

typedef struct {
	char *name;
	AstField **fields;
	int field_count;
} AstArchetypeDecl;

struct AstField {
	FieldKind kind;
	char *name;
	AstType *type;
	SourceLoc loc;
};

struct AstParam {
	char *name;
	AstType *type;
	int is_out;
	SourceLoc loc;
};

typedef struct {
	char *name;
	AstParam **params;
	int param_count;
	int is_extern;
	AstStmt **stmts;
	int stmt_count;
	SourceLoc loc;
} AstProcDecl;

typedef struct {
	char *name;
	AstParam **params;
	int param_count;
	AstStmt **stmts;
	int stmt_count;
	SourceLoc loc;
} AstSysDecl;

typedef struct {
	char *name;
	char **member_names;
	int member_count;
	SourceLoc loc;
} AstFuncGroupDecl;

typedef struct {
	char *name;
	AstParam **params;
	int param_count;
	AstType *return_type;
	int is_extern;
	AstStmt **stmts;
	int stmt_count;
	SourceLoc loc;
} AstFuncDecl;

typedef struct {
	AstStaticKind kind;
	union {
		struct {
			char *archetype_name;
			char **field_names;
			AstExpr **field_values;
			int field_count;
			AstExpr *init_length;
		} archetype;
		struct {
			char *name;
			AstType *element_type;
			int size;
		} array;
	};
} AstStaticDecl;

typedef struct {
	char *name;
	AstExpr *value;
} AstConstDecl;

struct AstDecl {
	AstDeclKind kind;
	SourceLoc loc;
	union {
		AstWorldDecl *world;
		AstArchetypeDecl *archetype;
		AstProcDecl *proc;
		AstSysDecl *sys;
		AstFuncDecl *func;
		AstFuncGroupDecl *func_group;
		AstStaticDecl *static_decl;
		AstConstDecl *constant;
	} data;
};

struct AstProgram {
	AstDecl **decls;
	int decl_count;
	SourceLoc loc;
};

/* =========================
   Statements
   ========================= */

typedef enum {
	AST_STMT_LET,
	AST_STMT_ASSIGN,
	AST_STMT_FOR,
	AST_STMT_IF,
	AST_STMT_BREAK,
	AST_STMT_RUN,
	AST_STMT_EXPR,
	AST_STMT_FREE,
	AST_STMT_RETURN,
	AST_STMT_MULTI_BIND,
	AST_STMT_EACH_FIELD,
} AstStmtKind;

typedef struct {
	char **names; /* always names[], min 1 entry */
	int name_count;
	AstType *type; /* optional explicit type, only single-var */
	AstExpr *value;
} AstLetStmt;

typedef struct {
	AstExpr *target;
	AstExpr *value;
	Operator op;
} AstAssignStmt;

typedef struct {
	char *var_name;
	AstExpr *iterable;
	AstStmt *init;
	AstExpr *cond;
	AstStmt *incr;
	AstStmt **body;
	int body_count;
} AstForStmt;

typedef struct {
	AstExpr *cond;
	AstStmt **then_body;
	int then_count;
	AstStmt **else_body;
	int else_count;
} AstIfStmt;

typedef struct {
	char *system_name;
	char *world_name;
} AstRunStmt;

typedef struct {
	AstExpr *expr;
} AstExprStmt;

typedef struct {
	AstExpr *value;
} AstFreeStmt;

typedef struct {
	AstExpr *value;
} AstReturnStmt;

typedef struct {
	char *name;
	int is_new;
	AstType *type;
} AstBindingTarget;

typedef struct {
	AstBindingTarget *targets;
	int target_count;
	AstExpr *value;
	int from_shorthand;
} AstMultiBindStmt;

typedef struct {
	char *binding_name;
	AstType *filter_type; /* may be NULL */
	char *arch_param_name;
	AstStmt **body;
	int body_count;
} AstEachFieldStmt;

struct AstStmt {
	AstStmtKind kind;
	SourceLoc loc;
	union {
		AstLetStmt let_stmt;
		AstAssignStmt assign_stmt;
		AstForStmt for_stmt;
		AstIfStmt if_stmt;
		AstRunStmt run_stmt;
		AstExprStmt expr_stmt;
		AstFreeStmt free_stmt;
		AstReturnStmt return_stmt;
		AstMultiBindStmt multi_bind;
		AstEachFieldStmt each_field;
	} data;
};

/* =========================
   Expressions
   ========================= */

typedef enum {
	AST_EXPR_LITERAL,
	AST_EXPR_NAME,
	AST_EXPR_FIELD,
	AST_EXPR_INDEX,
	AST_EXPR_BINARY,
	AST_EXPR_UNARY,
	AST_EXPR_CALL,
	AST_EXPR_ALLOC,
	AST_EXPR_ARRAY_LITERAL,
	AST_EXPR_STRING,
} AstExprKind;

struct AstExpr {
	AstExprKind kind;
	SourceLoc loc;
	AstType resolved; /* always populated by lowering */
	union {
		struct {
			char *lexeme;
		} literal;
		struct {
			char *name;
		} name;
		struct {
			AstExpr *base;
			char *field_name;
		} field;
		struct {
			AstExpr *base;
			AstExpr **indices;
			int index_count;
		} index;
		struct {
			Operator op;
			AstExpr *left;
			AstExpr *right;
		} binary;
		struct {
			UnaryOperator op;
			AstExpr *operand;
		} unary;
		struct {
			AstExpr *callee;
			AstExpr **args;
			int arg_count;
		} call;
		struct {
			char *archetype_name;
			char **field_names;
			AstExpr **field_values;
			int field_count;
			AstExpr *init_length;
		} alloc;
		struct {
			AstExpr **elements;
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

AstProgram *ast_program_create(void);
void ast_program_free(AstProgram *prog);

AstDecl *ast_decl_create(AstDeclKind kind);
void ast_decl_free(AstDecl *decl);

AstStmt *ast_stmt_create(AstStmtKind kind);
void ast_stmt_free(AstStmt *stmt);

AstExpr *ast_expr_create(AstExprKind kind);
void ast_expr_free(AstExpr *expr);

AstType *ast_type_create(AstTypeTag tag);
void ast_type_free(AstType *type);

AstField *ast_field_create(FieldKind kind, char *name, AstType *type);
void ast_field_free(AstField *field);

AstParam *ast_param_create(char *name, AstType *type);
void ast_param_free(AstParam *param);

#endif /* AST_H */

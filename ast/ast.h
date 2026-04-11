#ifndef AST_H
#define AST_H

#include <stddef.h>

/* Forward declarations */
struct Program;
struct Decl;
struct PoolDecl;
struct ProcDecl;
struct Field;
struct Statement;
struct Expression;
struct Path;

typedef enum {
	DECL_POOL,
	DECL_PROC,
} DeclKind;

typedef struct Program {
	struct Decl **decls;
	int decl_count;
} Program;

typedef struct Decl {
	DeclKind kind;
	union {
		struct PoolDecl *pool;
		struct ProcDecl *proc;
	};
} Decl;

typedef struct PoolDecl {
	char *name;
	struct Field *fields;
	int field_count;
} PoolDecl;

typedef struct Field {
	char *name;
	char *type; /* Type name as string */
} Field;

typedef struct ProcDecl {
	char *name;
	struct Statement **statements;
	int statement_count;
} ProcDecl;

/* ===== STATEMENTS ===== */

typedef enum {
	STMT_LET_BINDING,
	STMT_ASSIGNMENT,
	STMT_FOR_LOOP,
	STMT_PROC_CALL,
} StatementType;

typedef struct {
	char *name;
	struct Expression *value;
} LetBinding;

typedef struct {
	struct Path *target;
	struct Expression *value;
} Assignment;

typedef struct {
	char *var_name;
	char *pool_name;
	struct Statement **statements;
	int statement_count;
} ForLoop;

typedef struct {
	struct Path *target;
	char *method_name;
	struct Expression **args;
	int arg_count;
} ProcCall;

typedef struct Statement {
	StatementType type;
	union {
		LetBinding let_binding;
		Assignment assignment;
		ForLoop for_loop;
		ProcCall proc_call;
	} data;
} Statement;

/* ===== EXPRESSIONS ===== */

typedef enum {
	EXPR_LITERAL,
	EXPR_IDENTIFIER,
	EXPR_PATH,
	EXPR_BINARY_OP,
	EXPR_PROC_CALL,
	EXPR_ALLOCATION,
	EXPR_DEALLOCATION,
} ExpressionType;

typedef enum {
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
} Operator;

typedef struct {
	Operator op;
	struct Expression *left;
	struct Expression *right;
} BinaryOp;

typedef struct {
	struct Path *target;
	char *method_name;
	struct Expression **args;
	int arg_count;
} ExprProcCall;

typedef struct {
	char *pool_name;
	/* Field initializers */
	char **field_names;
	struct Expression **field_values;
	int field_count;
} Allocation;

typedef struct {
	char *pool_name;
	struct Expression *handle;
} Deallocation;

typedef enum {
	LITERAL_NUMBER,
	LITERAL_BOOL,
} LiteralType;

typedef struct {
	LiteralType type;
	union {
		double number;
		int boolean;
	} value;
} Literal;

typedef struct Expression {
	ExpressionType type;
	union {
		Literal literal;
		char *identifier;
		struct Path *path;
		BinaryOp binary_op;
		ExprProcCall proc_call;
		Allocation allocation;
		Deallocation deallocation;
	} data;
} Expression;

/* ===== PATHS ===== */

typedef struct Path {
	char **components;
	int component_count;
} Path;

/* ===== UTILITY ===== */

/* Constructor functions (optional, for convenience) */
Program *program_create(void);
PoolDecl *pool_decl_create(char *name);
ProcDecl *proc_decl_create(char *name);
Field *field_create(char *name, char *type);
Statement *statement_create(StatementType type);
Expression *expression_create(ExpressionType type);
Path *path_create(void);

void program_free(Program *prog);
void pool_decl_free(PoolDecl *pool);
void proc_decl_free(ProcDecl *proc);
void field_free(Field *field);
void statement_free(Statement *stmt);
void expression_free(Expression *expr);
void path_free(Path *path);

#endif /* AST_H */

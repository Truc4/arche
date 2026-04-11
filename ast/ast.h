#ifndef AST_H
#define AST_H

#include <stddef.h>

/* =========================
   Forward declarations
   ========================= */

typedef struct Program Program;
typedef struct Decl Decl;
typedef struct ArchetypeDecl ArchetypeDecl;
typedef struct ProcDecl ProcDecl;
typedef struct FieldDecl FieldDecl;
typedef struct TypeRef TypeRef;
typedef struct Statement Statement;
typedef struct Expression Expression;

/* =========================
   Source location
   ========================= */

typedef struct {
	int line;
	int column;
} SourceLoc;

/* =========================
   Program / declarations
   ========================= */

typedef enum {
	DECL_ARCHETYPE,
	DECL_PROC,
} DeclKind;

struct Program {
	Decl **decls;
	int decl_count;
	SourceLoc loc;
};

struct Decl {
	DeclKind kind;
	SourceLoc loc;
	union {
		ArchetypeDecl *archetype;
		ProcDecl *proc;
	} data;
};

/* =========================
   Types
   ========================= */

typedef enum {
	TYPE_NAME,		   /* Int, Float, Bool, Vec3, Player, etc. */
	TYPE_ARRAY,		   /* nested / jagged array */
	TYPE_SHAPED_ARRAY, /* dense ranked array */
} TypeKind;

struct TypeRef {
	TypeKind kind;
	SourceLoc loc;
	union {
		char *name;

		struct {
			TypeRef *element_type;
		} array;

		struct {
			TypeRef *element_type;
			int rank;
		} shaped_array;
	} data;
};

/* =========================
   Archetypes
   ========================= */

typedef enum {
	FIELD_META,	  /* one value for whole archetype */
	FIELD_COLUMN, /* one value per element, aligned with size */
} FieldKind;

struct FieldDecl {
	FieldKind kind;
	char *name;
	TypeRef *type;
	SourceLoc loc;
};

struct ArchetypeDecl {
	char *name;
	FieldDecl **fields;
	int field_count;
	SourceLoc loc;
};

struct ProcDecl {
	char *name;
	Statement **statements;
	int statement_count;
	SourceLoc loc;
};

/* =========================
   Statements
   ========================= */

typedef enum {
	STMT_LET,
	STMT_ASSIGN,
	STMT_FOR,
	STMT_EXPR,
	STMT_FREE,
} StatementType;

typedef struct {
	char *name;
	TypeRef *type;	   /* optional, may be NULL */
	Expression *value; /* optional, may be NULL */
} LetStmt;

typedef struct {
	Expression *target; /* must be assignable: name, field, or index */
	Expression *value;
} AssignStmt;

typedef struct {
	char *var_name;
	Expression *iterable;
	Statement **body;
	int body_count;
} ForStmt;

typedef struct {
	Expression *expr;
} ExprStmt;

typedef struct {
	Expression *value;
} FreeStmt;

struct Statement {
	StatementType type;
	SourceLoc loc;
	union {
		LetStmt let_stmt;
		AssignStmt assign_stmt;
		ForStmt for_stmt;
		ExprStmt expr_stmt;
		FreeStmt free_stmt;
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
	EXPR_BINARY,
	EXPR_UNARY,
	EXPR_CALL,
	EXPR_ALLOC,
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

typedef enum {
	UNARY_NEG,
	UNARY_NOT,
} UnaryOperator;

typedef struct {
	char *lexeme;
} LiteralExpr;

typedef struct {
	char *name;
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
} AllocExpr;

struct Expression {
	ExpressionType type;
	SourceLoc loc;
	union {
		LiteralExpr literal;
		NameExpr name;
		FieldExpr field;
		IndexExpr index;
		BinaryExpr binary;
		UnaryExpr unary;
		CallExpr call;
		AllocExpr alloc;
	} data;
};

/* =========================
   Constructors
   ========================= */

Program *program_create(void);
Decl *decl_create(DeclKind kind);

ArchetypeDecl *archetype_decl_create(char *name);
ProcDecl *proc_decl_create(char *name);
FieldDecl *field_decl_create(FieldKind kind, char *name, TypeRef *type);

TypeRef *type_name_create(char *name);
TypeRef *type_array_create(TypeRef *element_type);
TypeRef *type_shaped_array_create(TypeRef *element_type, int rank);

Statement *statement_create(StatementType type);
Expression *expression_create(ExpressionType type);

/* =========================
   Destructors
   ========================= */

void program_free(Program *prog);
void decl_free(Decl *decl);

void archetype_decl_free(ArchetypeDecl *archetype);
void proc_decl_free(ProcDecl *proc);
void field_decl_free(FieldDecl *field);
void type_ref_free(TypeRef *type);

void statement_free(Statement *stmt);
void expression_free(Expression *expr);

#endif /* AST_H */

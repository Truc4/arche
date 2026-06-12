#include "lower.h"
#include "../semantic/sem_model.h"
#include "../semantic/semantic.h"
#include "../syntax/syntax_view.h"
#include <stdlib.h>
#include <string.h>

/* Semantic context for syntax-tree-driven lowering: resolves nominal type aliases (e.g.
 * `file` -> `opaque`) at lowering time. */
static SemanticContext *g_lower_sem = NULL;
void lower_set_sem(struct SemanticContext *ctx) {
	g_lower_sem = ctx;
}

/* Resolved types live in the semantic side model (keyed by syntax tree node id), set by
 * the driver before lowering. When unset (e.g. unit tests that lower directly),
 * we fall back to the type still carried on the expression. */
static const SemModel *g_lower_model = NULL;
void lower_set_model(const SemModel *m) {
	g_lower_model = m;
}

/* =========================
   Type mapping
   ========================= */

/* Recognize a fixed-width integer type name (byte, i8/u8 .. i64/u64, i128/u128).
 * Returns 1 and fills width/signed on match. */
int hir_parse_int_width(const char *s, int *width, int *is_signed) {
	if (!s)
		return 0;
	/* `byte` is a core alias for `u8` (core.arche) — erased to `u8` before here, so no special case. */
	if (s[0] != 'i' && s[0] != 'u')
		return 0;
	int sign = (s[0] == 'i') ? 1 : 0;
	const char *num = s + 1;
	int w;
	if (strcmp(num, "8") == 0)
		w = 8;
	else if (strcmp(num, "16") == 0)
		w = 16;
	else if (strcmp(num, "32") == 0)
		w = 32;
	else if (strcmp(num, "64") == 0)
		w = 64;
	else if (strcmp(num, "128") == 0)
		w = 128;
	else
		return 0;
	*width = w;
	*is_signed = sign;
	return 1;
}

static HirType map_type_str(const char *resolved_type) {
	HirType t = {0};
	if (!resolved_type) {
		t.tag = HIR_TYPE_UNKNOWN;
		return t;
	}
	int w, sg;
	if (strcmp(resolved_type, "int") == 0) {
		t.tag = HIR_TYPE_INT;
		t.int_width = 32;
		t.int_signed = 1;
	} else if (hir_parse_int_width(resolved_type, &w, &sg)) {
		t.tag = HIR_TYPE_INT;
		t.int_width = w;
		t.int_signed = sg;
	} else if (strcmp(resolved_type, "float") == 0 || strcmp(resolved_type, "double") == 0) {
		t.tag = HIR_TYPE_FLOAT;
	} else if (strcmp(resolved_type, "char") == 0) {
		t.tag = HIR_TYPE_CHAR;
	} else if (strcmp(resolved_type, "void") == 0) {
		t.tag = HIR_TYPE_VOID;
	} else if (strcmp(resolved_type, "char_array") == 0) {
		t.tag = HIR_TYPE_CHAR_ARRAY;
	} else if (strcmp(resolved_type, "handle") == 0) {
		t.tag = HIR_TYPE_HANDLE;
	} else if (strcmp(resolved_type, "opaque") == 0) {
		t.tag = HIR_TYPE_OPAQUE;
	} else {
		/* archetype or other named type — pointer into syntax tree, safe since syntax tree outlives AST */
		t.tag = HIR_TYPE_NAMED;
		t.name = resolved_type;
	}
	return t;
}

/* =========================
   Tuple desugaring (syntax tree -> AST)
   =========================
   Tuples exist only in the syntax tree. Lowering flattens every tuple field
   `pos: (x, y)` into scalar columns `pos_x`, `pos_y`, and rewrites system
   parameters and bodies accordingly (tuple_rewrite_*), so the AST (and every
   later pass) never sees a tuple. The syntax-tree-path tuple group registry
   (build_tgroups / tgroup_lookup, below) drives this. */

/* Rewrite `base.comp` (HIR_EXPR_FIELD over NAME base) into NAME `base_comp`. */
static void tuple_rewrite_expr(HirExpr *e, const char *base) {
	if (!e)
		return;
	switch (e->kind) {
	case HIR_EXPR_FIELD:
		tuple_rewrite_expr(e->data.field.base, base);
		if (e->data.field.base && e->data.field.base->kind == HIR_EXPR_NAME &&
		    strcmp(e->data.field.base->data.name.name, base) == 0) {
			const char *sub = e->data.field.field_name;
			char *combined = malloc(strlen(base) + strlen(sub) + 2);
			sprintf(combined, "%s_%s", base, sub);
			e->kind = HIR_EXPR_NAME;
			e->data.name.name = combined; /* old base node intentionally leaked */
		}
		break;
	case HIR_EXPR_INDEX:
		tuple_rewrite_expr(e->data.index.base, base);
		for (int i = 0; i < e->data.index.index_count; i++)
			tuple_rewrite_expr(e->data.index.indices[i], base);
		break;
	case HIR_EXPR_BINARY:
		tuple_rewrite_expr(e->data.binary.left, base);
		tuple_rewrite_expr(e->data.binary.right, base);
		break;
	case HIR_EXPR_UNARY:
		tuple_rewrite_expr(e->data.unary.operand, base);
		break;
	case HIR_EXPR_CALL:
		tuple_rewrite_expr(e->data.call.callee, base);
		for (int i = 0; i < e->data.call.arg_count; i++)
			tuple_rewrite_expr(e->data.call.args[i], base);
		break;
	default:
		break;
	}
}

static void tuple_rewrite_stmt(HirStmt *s, const char *base) {
	if (!s)
		return;
	switch (s->kind) {
	case HIR_STMT_BIND:
		tuple_rewrite_expr(s->data.bind_stmt.value, base);
		break;
	case HIR_STMT_ASSIGN:
		tuple_rewrite_expr(s->data.assign_stmt.target, base);
		tuple_rewrite_expr(s->data.assign_stmt.value, base);
		break;
	case HIR_STMT_FOR:
		tuple_rewrite_stmt(s->data.for_stmt.init, base);
		tuple_rewrite_expr(s->data.for_stmt.cond, base);
		tuple_rewrite_stmt(s->data.for_stmt.incr, base);
		for (int i = 0; i < s->data.for_stmt.body_count; i++)
			tuple_rewrite_stmt(s->data.for_stmt.body[i], base);
		break;
	case HIR_STMT_IF:
		tuple_rewrite_expr(s->data.if_stmt.cond, base);
		for (int i = 0; i < s->data.if_stmt.then_count; i++)
			tuple_rewrite_stmt(s->data.if_stmt.then_body[i], base);
		for (int i = 0; i < s->data.if_stmt.else_count; i++)
			tuple_rewrite_stmt(s->data.if_stmt.else_body[i], base);
		break;
	case HIR_STMT_EXPR:
		tuple_rewrite_expr(s->data.expr_stmt.expr, base);
		break;
	case HIR_STMT_RETURN:
		for (int i = 0; i < s->data.return_stmt.count; i++)
			tuple_rewrite_expr(s->data.return_stmt.values[i], base);
		break;
	case HIR_STMT_BLOCK: /* match / surface `{ }` desugar */
		for (int i = 0; i < s->data.block.count; i++)
			tuple_rewrite_stmt(s->data.block.stmts[i], base);
		break;
	default:
		break;
	}
}

/* =========================================================================
   syntax-tree-driven lowering — the lowering path. Reads the lossless syntax tree via
   syntax_view + the semantic side model. Reuses the helpers above (map_type_str, tuple
   registry, tuple_rewrite_*).
   ========================================================================= */

static HirExpr *lower_expr_cst(SyntaxView e);
static HirStmt *lower_stmt_cst(SyntaxView s);
static char *dupz(const char *s);

/* Lower an expression in a constant-required position (pool capacity / init length / field default /
 * const decl / global scalar init). CTFE first: if it folds to a compile-time integer, emit that
 * literal so codegen sees a constant; otherwise lower it normally. A `func` is a value, so
 * `Grid[area(3,4)]` becomes `Grid[12]` here. */
static HirExpr *lower_const_or_expr(SyntaxView e) {
	int folded;
	if (g_lower_sem && semantic_try_const_int(g_lower_sem, e, &folded)) {
		HirExpr *lit = hir_expr_create(HIR_EXPR_LITERAL);
		char buf[32];
		snprintf(buf, sizeof(buf), "%d", folded);
		lit->data.literal.lexeme = dupz(buf);
		return lit;
	}
	return lower_expr_cst(e);
}
static HirDecl *lower_archetype_from(SyntaxView f, char *name);
/* An anonymous `arche { … }` literal denotes the shape with those fields: mint a synthetic global
 * archetype for it and return its (deterministic) name, so codegen's canonical_archetype_decl
 * unifies it with any structurally-identical named shape. NULL if it can't be registered. */
static const char *synth_archetype_name(SyntaxView arch_expr);

/* malloc'd NUL-terminated copy of a view's source text. */
static char *sv_dup(SyntaxView v) {
	SynText t = sv_text(v);
	char *s = malloc(t.len + 1);
	if (t.ptr)
		memcpy(s, t.ptr, t.len);
	s[t.len] = '\0';
	return s;
}

/* Lexeme of a node's first TOKEN leaf — token-precise, so it excludes any trailing trivia
 * (a comment / blank line) that the node's span may include. A node's `length` runs to the next
 * token, so `sv_dup` on a literal whose value is the last thing before a comment would swallow the
 * comment (e.g. `x :: 0` + `// note` → lexeme "0 … note", read as float). Use the token instead. */
static char *sv_dup_first_token(SyntaxView v) {
	if (v.node) {
		for (int i = 0; i < v.node->child_count; i++) {
			if (v.node->children[i].tag == SE_TOKEN) {
				uint32_t off = v.node->children[i].as.token.offset;
				uint32_t len = v.node->children[i].as.token.length;
				char *s = malloc(len + 1);
				memcpy(s, v.src + off, len);
				s[len] = '\0';
				return s;
			}
		}
	}
	return sv_dup(v);
}

/* malloc'd NUL-terminated copy of a borrowed text slice. */
static char *dupz(const char *s); /* defined with the module helpers below */

static char *txt_dup(SynText t) {
	char *s = malloc(t.len + 1);
	if (t.ptr)
		memcpy(s, t.ptr, t.len);
	s[t.len] = '\0';
	return s;
}

/* The type name from an SN_TYPE_REF node. A qualified `mod.Name` (two IDENTs) folds to the
 * module's mangled type symbol `mod_Name` — same fold as a qualified value reference; a bare type
 * returns its single identifier. Caller owns the returned string. */
static char *type_ref_name(SyntaxView t) {
	SynText ids[2];
	int n = 0;
	for (int i = 0; i < t.node->child_count && n < 2; i++) {
		const SyntaxElem *e = &t.node->children[i];
		if (e->tag == SE_TOKEN && e->as.token.kind == TOK_IDENT) {
			ids[n].ptr = t.src + e->as.token.offset;
			ids[n].len = e->as.token.length;
			n++;
		}
	}
	if (n >= 2) {
		size_t L = ids[0].len + 1 + ids[1].len + 1;
		char *r = malloc(L);
		snprintf(r, L, "%.*s.%.*s", (int)ids[0].len, ids[0].ptr, (int)ids[1].len, ids[1].ptr);
		return r;
	}
	return txt_dup(sv_token(t, TOK_IDENT));
}

/* Lower a syntax tree type node (SN_TYPE_*) to an HirType, mirroring lower_type_ref. */
static HirType *lower_type_cst(SyntaxView t) {
	if (!sv_present(t))
		return NULL;
	HirType *at = hir_type_create(HIR_TYPE_UNKNOWN);
	switch (sv_kind(t)) {
	case SN_TYPE_REF: {
		char *raw = type_ref_name(t);
		const char *r = g_lower_sem ? semantic_resolve_type_alias(g_lower_sem, raw) : raw;
		char *name = malloc(strlen(r) + 1);
		strcpy(name, r);
		if (strcmp(name, "archetype") == 0)
			at->tag = HIR_TYPE_ARCHETYPE;
		else if (strcmp(name, "opaque") == 0)
			at->tag = HIR_TYPE_OPAQUE;
		else if (strcmp(name, "type") == 0)
			; /* meta-type erased; leave UNKNOWN defensively */
		else
			*at = map_type_str(name); /* borrows name via HIR_TYPE_NAMED below */
		/* map_type_str's HIR_TYPE_NAMED .name points at its argument; keep a stable copy.
		 * For an OPAQUE type, preserve the NOMINAL alias name (e.g. "file"/"socket"), NOT the
		 * resolved backing "opaque", so the RAII pass can match a local against its registered
		 * `@drop` destructor (the backing alone can't tell `file` from `socket`). */
		if (at->tag == HIR_TYPE_NAMED) {
			at->name = name;
			free(raw);
		} else if (at->tag == HIR_TYPE_OPAQUE && strcmp(raw, "opaque") != 0) {
			at->name = raw; /* the nominal alias name */
			free(name);
		} else {
			free(name);
			free(raw);
		}
		break;
	}
	case SN_TYPE_ARRAY: {
		at->tag = HIR_TYPE_ARRAY;
		HirType *elem = hir_type_create(HIR_TYPE_UNKNOWN);
		char *en = txt_dup(sv_token(t, TOK_IDENT));
		*elem = map_type_str(en);
		if (elem->tag == HIR_TYPE_NAMED)
			elem->name = en;
		else
			free(en);
		at->elem = elem;
		break;
	}
	case SN_TYPE_SHAPED_ARRAY: {
		/* `T[a][b]...` — innermost element is the named type; each `[n]` adds a rank.
		 * Reconstruct nested shaped arrays from the NUMBER tokens. */
		char *en = txt_dup(sv_token(t, TOK_IDENT));
		HirType *elem = hir_type_create(HIR_TYPE_UNKNOWN);
		*elem = map_type_str(en);
		if (elem->tag == HIR_TYPE_NAMED)
			elem->name = en;
		else
			free(en);
		/* collect ranks (NUMBER tokens) left-to-right */
		int ranks[16], nr = 0;
		for (int i = 0; i < t.node->child_count && nr < 16; i++)
			if (t.node->children[i].tag == SE_TOKEN && t.node->children[i].as.token.kind == TOK_NUMBER) {
				char buf[32];
				int l = (int)t.node->children[i].as.token.length;
				if (l > 31)
					l = 31;
				memcpy(buf, t.src + t.node->children[i].as.token.offset, l);
				buf[l] = '\0';
				ranks[nr++] = atoi(buf);
			}
		HirType *cur = elem;
		for (int i = nr - 1; i >= 0; i--) {
			HirType *sh = hir_type_create(HIR_TYPE_SHAPED_ARRAY);
			sh->elem = cur;
			sh->rank = ranks[i];
			cur = sh;
		}
		free(at);
		return cur;
	}
	case SN_TYPE_HANDLE: {
		at->tag = HIR_TYPE_HANDLE;
		/* handle<Name> / handle(Name): the archetype name is the second IDENT */
		SynText h = sv_token(t, TOK_IDENT); /* "handle" */
		(void)h;
		/* find the IDENT that isn't "handle" */
		for (int i = 0; i < t.node->child_count; i++)
			if (t.node->children[i].tag == SE_TOKEN && t.node->children[i].as.token.kind == TOK_IDENT) {
				SynText nm = {t.src + t.node->children[i].as.token.offset, t.node->children[i].as.token.length};
				if (!(nm.len == 6 && memcmp(nm.ptr, "handle", 6) == 0)) {
					at->name = txt_dup(nm);
					break;
				}
			}
		break;
	}
	case SN_TYPE_PROC:
	case SN_TYPE_FUNC: {
		int is_proc = (sv_kind(t) == SN_TYPE_PROC);
		at->tag = HIR_TYPE_FUNC;
		at->is_proc = is_proc;
		int np = sv_count(t, SN_PARAM);
		at->param_count = np;
		at->params = calloc(np ? np : 1, sizeof(HirType *));
		for (int i = 0; i < np; i++)
			at->params[i] = lower_type_cst(sv_type_at(sv_child_at(t, SN_PARAM, i), 0));
		if (is_proc) {
			int no = sv_count(t, SN_OUT_PARAM);
			at->result_count = no;
			at->results = calloc(no ? no : 1, sizeof(HirType *));
			for (int i = 0; i < no; i++)
				at->results[i] = lower_type_cst(sv_type_at(sv_child_at(t, SN_OUT_PARAM, i), 0));
		} else {
			at->result_count = 1;
			at->results = calloc(1, sizeof(HirType *));
			at->results[0] = lower_type_cst(sv_type_at(t, 0));
		}
		break;
	}
	default:
		break;
	}
	return at;
}

/* Resolved type of a syntax tree expression node, from the side model (keyed by node id). */
/* Decode an interned TypeId to a HirType (Phase 3) — the back-end twin of map_type_str. A tier-2
 * distinct subtype lowers as its backing; a nominal's spelling (width-int / char_array / handle /
 * opaque / archetype) is parsed exactly as map_type_str does, so the result matches the old
 * string-driven path. */
static HirType map_type_id(const TypeArena *a, TypeId t) {
	while (tyid_kind(a, t) == TYK_NOMINAL && tyid_backing(a, t) != TYID_UNKNOWN)
		t = tyid_backing(a, t);
	HirType ht = {0};
	switch (tyid_kind(a, t)) {
	case TYK_PRIM:
		switch (tyid_prim(a, t)) {
		case PRIM_INT:
			ht.tag = HIR_TYPE_INT;
			ht.int_width = 32;
			ht.int_signed = 1;
			break;
		case PRIM_FLOAT:
			ht.tag = HIR_TYPE_FLOAT;
			break;
		case PRIM_CHAR:
			ht.tag = HIR_TYPE_CHAR;
			break;
		case PRIM_VOID:
			ht.tag = HIR_TYPE_VOID;
			break;
		case PRIM_STR:
			ht.tag = HIR_TYPE_CHAR_ARRAY;
			break;
		case PRIM_BOOL:
			ht.tag = HIR_TYPE_NAMED; /* matches map_type_str("bool") -> NAMED */
			ht.name = "bool";
			break;
		default:
			ht.tag = HIR_TYPE_UNKNOWN;
			break;
		}
		return ht;
	case TYK_NOMINAL:
		/* width-int / char_array / handle / opaque / archetype name — same parse as map_type_str. */
		return map_type_str(tyid_nominal_name(a, t));
	default:
		ht.tag = HIR_TYPE_UNKNOWN;
		return ht;
	}
}

static HirType syntax_expr_type(SyntaxView e) {
	if (!g_lower_model || !g_lower_sem)
		return map_type_str(NULL);
	return map_type_id(sem_context_arena(g_lower_sem), sem_model_expr_type_id(g_lower_model, sv_id(e)));
}

/* Decode a string token's content (quotes + escapes) like parse_primary_expr does. */
static char *syntax_decode_string(SynText raw, int *out_len) {
	const char *s = raw.ptr;
	size_t len = raw.len;
	char *value = malloc(len + 1);
	int p = 0;
	for (size_t i = 1; i + 1 < len; i++) {
		if (s[i] == '\\' && i + 2 < len) {
			i++;
			switch (s[i]) {
			case 'n':
				value[p++] = '\n';
				break;
			case 't':
				value[p++] = '\t';
				break;
			case 'r':
				value[p++] = '\r';
				break;
			case '\\':
				value[p++] = '\\';
				break;
			case '"':
				value[p++] = '"';
				break;
			default:
				value[p++] = s[i];
				break;
			}
		} else {
			value[p++] = s[i];
		}
	}
	value[p] = '\0';
	*out_len = p;
	return value;
}

/* The "innermost value expression" child of a node (skips role-only name wrappers). */
static SyntaxView syntax_first_expr(SyntaxView v) {
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = v.node->children[i].as.node->kind;
			if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR) {
				SyntaxView r = {v.node->children[i].as.node, v.src};
				return r;
			}
		}
	SyntaxView none = {NULL, v.src};
	return none;
}

static Operator syntax_tok_to_op(TokenKind k) {
	switch (k) {
	case TOK_PLUS:
		return OP_ADD;
	case TOK_MINUS:
		return OP_SUB;
	case TOK_STAR:
		return OP_MUL;
	case TOK_SLASH:
		return OP_DIV;
	case TOK_PERCENT:
		return OP_MOD;
	case TOK_EQ_EQ:
		return OP_EQ;
	case TOK_BANG_EQ:
		return OP_NEQ;
	case TOK_LT:
		return OP_LT;
	case TOK_GT:
		return OP_GT;
	case TOK_LT_EQ:
		return OP_LTE;
	case TOK_GT_EQ:
		return OP_GTE;
	case TOK_AMP_AMP:
		return OP_AND;
	case TOK_PIPE_PIPE:
		return OP_OR;
	default:
		return OP_NONE;
	}
}

/* sv_type_at / sv_type_count now live in syntax_view.h (shared with the analyzer). */

/* nth child node that is an expression kind (skips role/name/type wrappers). */
static SyntaxView sv_node_at_expr(SyntaxView v, int idx) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = v.node->children[i].as.node->kind;
			if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR) {
				if (c == idx) {
					SyntaxView r = {v.node->children[i].as.node, v.src};
					return r;
				}
				c++;
			}
		}
	SyntaxView none = {NULL, v.src};
	return none;
}

static HirExpr *lower_expr_cst(SyntaxView e) {
	if (!sv_present(e))
		return NULL;
	HirExpr *ax = hir_expr_create(HIR_EXPR_LITERAL);
	ax->resolved = syntax_expr_type(e);

	switch (sv_kind(e)) {
	case SN_PAREN_EXPR:
		/* transparent: lower the inner expression */
		return lower_expr_cst(syntax_first_expr(e));
	case SN_LITERAL_EXPR:
		ax->kind = HIR_EXPR_LITERAL;
		ax->data.literal.lexeme = sv_dup_first_token(e);
		/* Literals are self-describing: if semantic left no resolved type (it doesn't
		 * key every literal, e.g. assignment RHS), infer it from the lexeme so codegen
		 * stores the right width. A recorded type wins (it may carry a coercion). */
		if (ax->resolved.tag == HIR_TYPE_UNKNOWN) {
			const char *lx = ax->data.literal.lexeme;
			if (lx && lx[0] == '\'') {
				ax->resolved.tag = HIR_TYPE_CHAR;
			} else if (lx && strchr(lx, '.')) {
				ax->resolved.tag = HIR_TYPE_FLOAT;
			} else if (lx && (lx[0] == '-' || (lx[0] >= '0' && lx[0] <= '9'))) {
				ax->resolved.tag = HIR_TYPE_INT;
				ax->resolved.int_width = 32;
				ax->resolved.int_signed = 1;
			}
		}
		break;
	case SN_STRING_EXPR: {
		ax->kind = HIR_EXPR_STRING;
		int n = 0;
		ax->data.string.value = syntax_decode_string(sv_text(e), &n);
		ax->data.string.length = n;
		break;
	}
	case SN_NAME_EXPR: {
		ax->kind = HIR_EXPR_NAME;
		/* table<Name> in value position resolves to the bare archetype name */
		if (sv_has_token(e, TOK_LT)) {
			/* second IDENT is the archetype name */
			char *nm = NULL;
			int seen = 0;
			for (int i = 0; i < e.node->child_count; i++)
				if (e.node->children[i].tag == SE_TOKEN && e.node->children[i].as.token.kind == TOK_IDENT) {
					SynText t = {e.src + e.node->children[i].as.token.offset, e.node->children[i].as.token.length};
					if (seen++) {
						nm = txt_dup(t);
						break;
					}
				}
			ax->data.name.name = nm ? nm : sv_dup(e);
		} else {
			ax->data.name.name = txt_dup(sv_token(e, TOK_IDENT));
		}
		break;
	}
	case SN_FIELD_EXPR: {
		/* Enum variant access `Enum.variant` → its int value (enums are compile-time, erased). */
		if (g_lower_sem && sv_count(e, SN_FIELD_NAME) == 1) {
			char *bn = txt_dup(sv_token(e, TOK_IDENT));
			if (bn && semantic_is_enum_type(g_lower_sem, bn)) {
				char *vn = sv_dup(sv_child_at(e, SN_FIELD_NAME, 0));
				long val = 0;
				if (vn && semantic_enum_variant_value(g_lower_sem, bn, vn, &val)) {
					char buf[32];
					snprintf(buf, sizeof(buf), "%ld", val);
					ax->kind = HIR_EXPR_LITERAL;
					ax->data.literal.lexeme = dupz(buf);
					ax->resolved.tag = HIR_TYPE_INT;
					ax->resolved.int_width = 32;
					ax->resolved.int_signed = 1;
					free(bn);
					free(vn);
					break;
				}
				free(vn);
			}
			free(bn);
		}
		/* `base.f1.f2…[idx]` is flat under one node: base IDENT, then (DOT FIELD_NAME)+,
		 * optionally a trailing `[indices]`. Rebuild the left-assoc AST. */
		HirExpr *base = hir_expr_create(HIR_EXPR_NAME);
		base->data.name.name = txt_dup(sv_token(e, TOK_IDENT));
		HirExpr *cur = base;
		int nfields = sv_count(e, SN_FIELD_NAME);
		for (int i = 0; i < nfields; i++) {
			HirExpr *f = hir_expr_create(HIR_EXPR_FIELD);
			f->data.field.base = cur;
			f->data.field.field_name = sv_dup(sv_child_at(e, SN_FIELD_NAME, i));
			cur = f;
		}
		/* trailing index over the final field */
		if (sv_has_token(e, TOK_LBRACKET)) {
			HirExpr *idx = hir_expr_create(HIR_EXPR_INDEX);
			/* the column's element type is this expr's resolved type; codegen reads it
			 * off the base field to size the store/GEP. */
			if (cur->kind == HIR_EXPR_FIELD)
				cur->resolved = syntax_expr_type(e);
			idx->data.index.base = cur;
			int ic = 0;
			for (int i = 0; i < e.node->child_count; i++)
				if (e.node->children[i].tag == SE_NODE) {
					SyntaxNodeKind k = e.node->children[i].as.node->kind;
					if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
						ic++;
				}
			idx->data.index.indices = calloc(ic ? ic : 1, sizeof(HirExpr *));
			idx->data.index.index_count = 0;
			for (int i = 0; i < e.node->child_count; i++)
				if (e.node->children[i].tag == SE_NODE) {
					SyntaxNodeKind k = e.node->children[i].as.node->kind;
					if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR) {
						SyntaxView iv = {e.node->children[i].as.node, e.src};
						idx->data.index.indices[idx->data.index.index_count++] = lower_expr_cst(iv);
					}
				}
			cur = idx;
		}
		free(ax);
		cur->resolved = syntax_expr_type(e);
		return cur;
	}
	case SN_INDEX_EXPR: {
		ax->kind = HIR_EXPR_INDEX;
		/* base may be a `name.f1.f2…` member chain folded into this node
		 * (e.g. `Particle.pos_x[0]`); rebuild the FIELD chain over the IDENT. */
		HirExpr *base = hir_expr_create(HIR_EXPR_NAME);
		base->data.name.name = txt_dup(sv_token(e, TOK_IDENT));
		int nfields = sv_count(e, SN_FIELD_NAME);
		for (int i = 0; i < nfields; i++) {
			HirExpr *f = hir_expr_create(HIR_EXPR_FIELD);
			f->data.field.base = base;
			f->data.field.field_name = sv_dup(sv_child_at(e, SN_FIELD_NAME, i));
			base = f;
		}
		/* The indexed column's element type equals this index expr's resolved type;
		 * codegen reads it off the base (FIELD) to pick the store/GEP width. */
		if (base->kind == HIR_EXPR_FIELD)
			base->resolved = ax->resolved;
		ax->data.index.base = base;
		int ic = 0;
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_NODE) {
				SyntaxNodeKind k = e.node->children[i].as.node->kind;
				if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
					ic++;
			}
		ax->data.index.indices = calloc(ic ? ic : 1, sizeof(HirExpr *));
		ax->data.index.index_count = 0;
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_NODE) {
				SyntaxNodeKind k = e.node->children[i].as.node->kind;
				if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR) {
					SyntaxView iv = {e.node->children[i].as.node, e.src};
					ax->data.index.indices[ax->data.index.index_count++] = lower_expr_cst(iv);
				}
			}
		/* `!name` failure policy: the SN_POLICY_REF child carries the policy ident. */
		{
			SyntaxView pol = sv_child(e, SN_POLICY_REF);
			if (sv_present(pol))
				ax->data.index.policy = txt_dup(sv_token(pol, TOK_IDENT));
		}
		/* Stamp the bounds prover's verdict (SemModel, keyed by node id) so codegen elides the policy
		 * macro on a proven-in-bounds index — the single elision authority, no codegen re-derivation. */
		if (g_lower_model && sem_model_policy_elided(g_lower_model, sv_id(e)))
			ax->data.index.policy_elided = 1;
		break;
	}
	case SN_SLICE_EXPR: {
		/* `base[lo:hi]` — base is IDENT + folded field chain (as for index); the expr child(ren)
		 * split on the `:` token: before → lo, after → hi (either may be omitted → NULL). */
		ax->kind = HIR_EXPR_SLICE;
		HirExpr *base = hir_expr_create(HIR_EXPR_NAME);
		base->data.name.name = txt_dup(sv_token(e, TOK_IDENT));
		int nfields = sv_count(e, SN_FIELD_NAME);
		for (int i = 0; i < nfields; i++) {
			HirExpr *f = hir_expr_create(HIR_EXPR_FIELD);
			f->data.field.base = base;
			f->data.field.field_name = sv_dup(sv_child_at(e, SN_FIELD_NAME, i));
			base = f;
		}
		ax->data.slice.base = base;
		ax->data.slice.lo = NULL;
		ax->data.slice.hi = NULL;
		int seen_colon = 0;
		for (int i = 0; i < e.node->child_count; i++) {
			SyntaxElem *ch = &e.node->children[i];
			if (ch->tag == SE_TOKEN && ch->as.token.kind == TOK_COLON) {
				seen_colon = 1;
				continue;
			}
			if (ch->tag == SE_NODE) {
				SyntaxNodeKind k = ch->as.node->kind;
				if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR) {
					SyntaxView iv = {ch->as.node, e.src};
					HirExpr *ex = lower_expr_cst(iv);
					if (!seen_colon)
						ax->data.slice.lo = ex;
					else
						ax->data.slice.hi = ex;
				}
			}
		}
		{
			SyntaxView pol = sv_child(e, SN_POLICY_REF);
			if (sv_present(pol))
				ax->data.slice.policy = txt_dup(sv_token(pol, TOK_IDENT));
		}
		if (g_lower_model && sem_model_policy_elided(g_lower_model, sv_id(e)))
			ax->data.slice.policy_elided = 1;
		break;
	}
	case SN_BINARY_EXPR: {
		ax->kind = HIR_EXPR_BINARY;
		/* operator token */
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_TOKEN) {
				Operator op = syntax_tok_to_op(e.node->children[i].as.token.kind);
				if (op != OP_NONE) {
					ax->data.binary.op = op;
					break;
				}
			}
		ax->data.binary.left = lower_expr_cst(sv_node_at_expr(e, 0));
		ax->data.binary.right = lower_expr_cst(sv_node_at_expr(e, 1));
		/* `a / b !policy`: the SN_POLICY_REF child carries the div-by-zero policy ident. */
		{
			SyntaxView pol = sv_child(e, SN_POLICY_REF);
			if (sv_present(pol))
				ax->data.binary.policy = txt_dup(sv_token(pol, TOK_IDENT));
		}
		break;
	}
	case SN_UNARY_EXPR: {
		ax->kind = HIR_EXPR_UNARY;
		SynText op = {NULL, 0};
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_TOKEN) {
				TokenKind tk = e.node->children[i].as.token.kind;
				if (tk == TOK_MINUS)
					ax->data.unary.op = UNARY_NEG;
				else if (tk == TOK_BANG)
					ax->data.unary.op = UNARY_NOT;
				else if (tk == TOK_MOVE)
					ax->data.unary.op = UNARY_MOVE;
				else if (tk == TOK_COPY)
					ax->data.unary.op = UNARY_COPY;
				else
					continue;
				break;
			}
		(void)op;
		ax->data.unary.operand = lower_expr_cst(syntax_first_expr(e));
		break;
	}
	case SN_CALL_EXPR: {
		ax->kind = HIR_EXPR_CALL;
		HirExpr *callee;
		int nfields = sv_count(e, SN_FIELD_NAME);
		if (nfields > 0) {
			/* Qualified callee `mod.name` (no SN_CALLEE_NAME): rebuild the field access from the
			 * base IDENT + SN_FIELD_NAME children. The qualify pass later folds `mod.name` →
			 * `mod_name` for imported modules. */
			HirExpr *base = hir_expr_create(HIR_EXPR_NAME);
			base->data.name.name = txt_dup(sv_token(e, TOK_IDENT));
			HirExpr *cur = base;
			for (int i = 0; i < nfields; i++) {
				HirExpr *f = hir_expr_create(HIR_EXPR_FIELD);
				f->data.field.base = cur;
				f->data.field.field_name = sv_dup(sv_child_at(e, SN_FIELD_NAME, i));
				cur = f;
			}
			callee = cur;
		} else {
			callee = hir_expr_create(HIR_EXPR_NAME);
			callee->data.name.name = sv_dup(sv_child(e, SN_CALLEE_NAME));
			/* Callable alias (`handler :: some_proc`): rewrite the callee to the real target so
			 * codegen emits a direct call. */
			if (g_lower_sem) {
				const char *tgt = semantic_resolve_callable_alias(g_lower_sem, callee->data.name.name);
				if (tgt) {
					free(callee->data.name.name);
					callee->data.name.name = malloc(strlen(tgt) + 1);
					strcpy(callee->data.name.name, tgt);
				}
			}
		}
		ax->data.call.callee = callee;
		int ac = 0;
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_NODE) {
				SyntaxNodeKind k = e.node->children[i].as.node->kind;
				if ((k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR) || k == SN_ARCH_EXPR)
					ac++;
			}
		ax->data.call.args = calloc(ac ? ac : 1, sizeof(HirExpr *));
		ax->data.call.arg_count = 0;
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_NODE) {
				SyntaxNodeKind k = e.node->children[i].as.node->kind;
				/* SN_ARCH_EXPR (an anonymous `arche {…}` literal) is an expression arg too, but sits
				 * outside the contiguous expr-kind range — accept it so `insert(arche{…}, …)` works. */
				if ((k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR) || k == SN_ARCH_EXPR) {
					SyntaxView av = {e.node->children[i].as.node, e.src};
					ax->data.call.args[ax->data.call.arg_count++] = lower_expr_cst(av);
				}
			}
		/* `insert(P,x) ?handler` / `… !panic`: the SN_POLICY_REF child carries the policy ident and
		 * the sigil (`?` ⇒ handler, `!` ⇒ panic). Semantic enforces sigil↔kind. */
		{
			SyntaxView pol = sv_child(e, SN_POLICY_REF);
			if (sv_present(pol)) {
				ax->data.call.policy = txt_dup(sv_token(pol, TOK_IDENT));
				ax->data.call.policy_is_handler = sv_token(pol, TOK_QUESTION).ptr != NULL;
			}
		}
		break;
	}
	case SN_ARCH_EXPR: {
		/* Anonymous shape literal `arche { … }` in expression position (a binding RHS is handled
		 * in lower_decl_cst). Lower to a NAME referencing a synthetic global archetype of the same
		 * shape, so it unifies with a structurally-identical named shape via canonical_archetype_decl. */
		const char *sn = synth_archetype_name(e);
		if (sn) {
			ax->kind = HIR_EXPR_NAME;
			ax->data.name.name = dupz(sn);
		} else {
			ax->kind = HIR_EXPR_LITERAL;
			ax->data.literal.lexeme = sv_dup(e);
		}
		break;
	}
	case SN_ARRAY_LIT_EXPR: {
		/* `{ e0, e1, … }` — a fixed-size array literal. Each child expression node is an element
		 * (nested `{…}` for inner dimensions lower recursively into nested array literals). */
		ax->kind = HIR_EXPR_ARRAY_LITERAL;
		int n = 0;
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_NODE)
				n++;
		ax->data.array_literal.elements = n ? calloc((size_t)n, sizeof(HirExpr *)) : NULL;
		ax->data.array_literal.element_count = 0;
		for (int i = 0; i < e.node->child_count; i++) {
			if (e.node->children[i].tag != SE_NODE)
				continue;
			SyntaxView ev = {e.node->children[i].as.node, e.src};
			ax->data.array_literal.elements[ax->data.array_literal.element_count++] = lower_expr_cst(ev);
		}
		break;
	}
	default:
		/* SN_ALLOC_EXPR and any unhandled: leave as a placeholder literal for now (these surface in
		 * verify-codegen if exercised). */
		ax->kind = HIR_EXPR_LITERAL;
		ax->data.literal.lexeme = sv_dup(e);
		break;
	}
	/* Implicit move: semantic flagged this node (a bare move-only name in an ownership-taking
	 * position) as an elided `move`. Materialize it as an explicit `move` HIR node so the transfer
	 * lives in the program, not in codegen's reading of the syntax — codegen then has ONE move path
	 * (consume the source, suppress its RAII drop). Semantic only flags bare names, so `ax` is never
	 * already a move/copy; guard anyway. */
	if (g_lower_model && sem_model_implicit_move(g_lower_model, sv_id(e)) && ax->kind != HIR_EXPR_UNARY) {
		HirExpr *mv = hir_expr_create(HIR_EXPR_UNARY);
		mv->data.unary.op = UNARY_MOVE;
		mv->data.unary.operand = ax;
		mv->resolved = ax->resolved; /* `move` is transparent — same type as its operand */
		return mv;
	}
	return ax;
}

/* Lower the statement-kind child nodes of `parent` into an HirStmt array. */
static HirStmt **syntax_lower_body(SyntaxView parent, int *out_count) {
	int n = 0;
	for (int i = 0; i < parent.node->child_count; i++)
		if (parent.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = parent.node->children[i].as.node->kind;
			if (k >= SN_BIND_STMT && k <= SN_MATCH_STMT)
				n++;
		}
	*out_count = n;
	if (n == 0)
		return NULL;
	HirStmt **out = calloc(n, sizeof(HirStmt *));
	int j = 0;
	for (int i = 0; i < parent.node->child_count; i++)
		if (parent.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = parent.node->children[i].as.node->kind;
			if (k >= SN_BIND_STMT && k <= SN_MATCH_STMT) {
				SyntaxView sv = {parent.node->children[i].as.node, parent.src};
				out[j++] = lower_stmt_cst(sv);
			}
		}
	return out;
}

static HirStmt *lower_stmt_cst(SyntaxView s);

/* Lower the direct statement children of `parent` whose child index is in [lo, hi). Used to
 * split an if's flat then/else child list on the `else` token. */
static HirStmt **syntax_lower_body_split(SyntaxView parent, int lo, int hi, int *out_count) {
	if (lo < 0)
		lo = 0;
	if (hi > parent.node->child_count)
		hi = parent.node->child_count;
	int n = 0;
	for (int i = lo; i < hi; i++)
		if (parent.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = parent.node->children[i].as.node->kind;
			if (k >= SN_BIND_STMT && k <= SN_MATCH_STMT)
				n++;
		}
	*out_count = n;
	if (n == 0)
		return NULL;
	HirStmt **out = calloc(n, sizeof(HirStmt *));
	int j = 0;
	for (int i = lo; i < hi; i++)
		if (parent.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = parent.node->children[i].as.node->kind;
			if (k >= SN_BIND_STMT && k <= SN_MATCH_STMT) {
				SyntaxView sv = {parent.node->children[i].as.node, parent.src};
				out[j++] = lower_stmt_cst(sv);
			}
		}
	return out;
}

static Operator syntax_assign_op(TokenKind k) {
	switch (k) {
	case TOK_PLUS_EQ:
		return OP_ADD;
	case TOK_MINUS_EQ:
		return OP_SUB;
	case TOK_STAR_EQ:
		return OP_MUL;
	case TOK_SLASH_EQ:
		return OP_DIV;
	case TOK_PERCENT_EQ:
		return OP_MOD;
	default:
		return OP_NONE; /* plain `=` */
	}
}

static HirStmt *lower_stmt_cst(SyntaxView s) {
	HirStmt *as = hir_stmt_create(HIR_STMT_EXPR);

	switch (sv_kind(s)) {
	case SN_BIND_STMT: {
		if (g_lower_model && sem_model_bind_alias(g_lower_model, sv_id(s))) {
			as->kind = HIR_STMT_EXPR;
			HirExpr *zero = hir_expr_create(HIR_EXPR_LITERAL);
			zero->data.literal.lexeme = malloc(2);
			strcpy(zero->data.literal.lexeme, "0");
			as->data.expr_stmt.expr = zero;
			break;
		}
		as->kind = HIR_STMT_BIND;
		SyntaxView target = sv_node_at_expr(s, 0);
		as->data.bind_stmt.name_count = 1;
		as->data.bind_stmt.names = calloc(1, sizeof(char *));
		as->data.bind_stmt.names[0] = sv_dup(target);
		as->data.bind_stmt.type = lower_type_cst(sv_type_at(s, 0));
		as->data.bind_stmt.value = lower_expr_cst(sv_node_at_expr(s, 1));
		break;
	}
	case SN_ASSIGN_STMT: {
		as->kind = HIR_STMT_ASSIGN;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_TOKEN) {
				TokenKind tk = s.node->children[i].as.token.kind;
				if (tk == TOK_EQ || tk == TOK_PLUS_EQ || tk == TOK_MINUS_EQ || tk == TOK_STAR_EQ ||
				    tk == TOK_SLASH_EQ || tk == TOK_PERCENT_EQ) {
					as->data.assign_stmt.op = syntax_assign_op(tk);
					break;
				}
			}
		as->data.assign_stmt.target = lower_expr_cst(sv_node_at_expr(s, 0));
		as->data.assign_stmt.value = lower_expr_cst(sv_node_at_expr(s, 1));
		break;
	}
	case SN_EXPR_STMT:
		as->kind = HIR_STMT_EXPR;
		as->data.expr_stmt.expr = lower_expr_cst(sv_node_at_expr(s, 0));
		break;
	case SN_BREAK_STMT:
		as->kind = HIR_STMT_BREAK;
		break;
	case SN_CONTINUE_STMT:
		as->kind = HIR_STMT_CONTINUE;
		break;
	case SN_RETURN_STMT: {
		as->kind = HIR_STMT_RETURN;
		int c = 0;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_NODE) {
				SyntaxNodeKind k = s.node->children[i].as.node->kind;
				if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
					c++;
			}
		as->data.return_stmt.count = c;
		as->data.return_stmt.values = calloc(c ? c : 1, sizeof(HirExpr *));
		for (int i = 0; i < c; i++)
			as->data.return_stmt.values[i] = lower_expr_cst(sv_node_at_expr(s, i));
		break;
	}
	case SN_RUN_STMT: {
		as->kind = HIR_STMT_RUN;
		/* `run sys` / `run device.system`: `run` is a keyword, so the only IDENTs are the
		 * (possibly qualified) system-name segments — join them with `.` to match the imported
		 * system's canonical identity. (`run … in world` is not emitted by the parser today.) */
		char namebuf[256];
		int nl = 0;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_TOKEN && s.node->children[i].as.token.kind == TOK_IDENT) {
				if (nl > 0 && nl < (int)sizeof(namebuf) - 1)
					namebuf[nl++] = '.';
				int seg = (int)s.node->children[i].as.token.length;
				for (int k = 0; k < seg && nl < (int)sizeof(namebuf) - 1; k++)
					namebuf[nl++] = s.src[s.node->children[i].as.token.offset + k];
			}
		namebuf[nl] = '\0';
		as->data.run_stmt.system_name = dupz(namebuf);
		as->data.run_stmt.world_name = NULL;
		break;
	}
	case SN_IF_STMT: {
		as->kind = HIR_STMT_IF;
		as->data.if_stmt.cond = lower_expr_cst(sv_node_at_expr(s, 0));
		/* The parser emits then- and else-statements as a FLAT child list separated by the
		 * `else` token. Split on it: children before → then-body, children after → else-body. */
		int else_tok = -1;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_TOKEN && s.node->children[i].as.token.kind == TOK_ELSE) {
				else_tok = i;
				break;
			}
		as->data.if_stmt.then_body =
		    syntax_lower_body_split(s, 0, else_tok >= 0 ? else_tok : s.node->child_count, &as->data.if_stmt.then_count);
		if (else_tok >= 0)
			as->data.if_stmt.else_body =
			    syntax_lower_body_split(s, else_tok, s.node->child_count, &as->data.if_stmt.else_count);
		break;
	}
	case SN_FOR_STMT: {
		as->kind = HIR_STMT_FOR;
		if (sv_has_token(s, TOK_LPAREN)) {
			/* C-style: `for ( init ; cond ; incr ) { body }`. The two header `;` split
			 * the clauses into segments 0/1/2 (any may be empty); init/incr are statement
			 * nodes, cond a bare expr node. Body statements follow the `{`. */
			int seen_brace = 0, seg = 0, nbody = 0;
			for (int i = 0; i < s.node->child_count; i++) {
				SyntaxElem *ch = &s.node->children[i];
				if (ch->tag == SE_TOKEN) {
					if (ch->as.token.kind == TOK_LBRACE)
						seen_brace = 1;
					else if (ch->as.token.kind == TOK_SEMI && !seen_brace && seg < 2)
						seg++;
					continue;
				}
				SyntaxNodeKind k = ch->as.node->kind;
				SyntaxView cv = {ch->as.node, s.src};
				if (seen_brace) {
					if (k >= SN_BIND_STMT && k <= SN_MATCH_STMT)
						nbody++;
					continue;
				}
				if (seg == 0 && k >= SN_BIND_STMT && k <= SN_MATCH_STMT)
					as->data.for_stmt.init = lower_stmt_cst(cv);
				else if (seg == 1 && k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
					as->data.for_stmt.cond = lower_expr_cst(cv);
				else if (seg == 2 && k >= SN_BIND_STMT && k <= SN_MATCH_STMT)
					as->data.for_stmt.incr = lower_stmt_cst(cv);
			}
			as->data.for_stmt.body = calloc(nbody ? nbody : 1, sizeof(HirStmt *));
			as->data.for_stmt.body_count = 0;
			seen_brace = 0;
			for (int i = 0; i < s.node->child_count; i++) {
				SyntaxElem *ch = &s.node->children[i];
				if (ch->tag == SE_TOKEN) {
					if (ch->as.token.kind == TOK_LBRACE)
						seen_brace = 1;
					continue;
				}
				if (!seen_brace)
					continue;
				SyntaxNodeKind k = ch->as.node->kind;
				if (k >= SN_BIND_STMT && k <= SN_MATCH_STMT) {
					SyntaxView cv = {ch->as.node, s.src};
					as->data.for_stmt.body[as->data.for_stmt.body_count++] = lower_stmt_cst(cv);
				}
			}
			break;
		}
		/* infinite form `for { body }` (the range-based `for IDENT in …` is rejected at parse). */
		as->data.for_stmt.body = syntax_lower_body(s, &as->data.for_stmt.body_count);
		break;
	}
	case SN_EACH_FIELD_STMT: {
		as->kind = HIR_STMT_EACH_FIELD;
		int ni = 0;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_TOKEN && s.node->children[i].as.token.kind == TOK_IDENT) {
				SynText t = {s.src + s.node->children[i].as.token.offset, s.node->children[i].as.token.length};
				if (ni == 0)
					as->data.each_field.binding_name = txt_dup(t);
				else if (ni == 1)
					as->data.each_field.arch_param_name = txt_dup(t);
				ni++;
			}
		as->data.each_field.filter_type = lower_type_cst(sv_type_at(s, 0));
		as->data.each_field.body = syntax_lower_body(s, &as->data.each_field.body_count);
		break;
	}
	case SN_MULTI_BIND_STMT: {
		as->kind = HIR_STMT_MULTI_BIND;
		/* The binding operator is the first `=` token; targets precede it, the value
		 * follows. Shorthand `a, b := e` has no leading `(` (all targets new, untyped);
		 * the paren form `(a: T, b) = e` carries per-target types + `is_new`. */
		int eq_idx = -1, lparen_idx = -1;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_TOKEN) {
				TokenKind tk = s.node->children[i].as.token.kind;
				if (tk == TOK_LPAREN && lparen_idx < 0)
					lparen_idx = i;
				if (tk == TOK_EQ) {
					eq_idx = i;
					break;
				}
			}
		int paren = (lparen_idx >= 0 && lparen_idx < eq_idx);
		as->data.multi_bind.from_shorthand = paren ? 0 : 1;
		as->data.multi_bind.targets = calloc(s.node->child_count, sizeof(HirBindingTarget));
		as->data.multi_bind.target_count = 0;
		const char *pend = NULL;
		int pend_len = 0, pend_active = 0, pend_new = 0;
		HirType *pend_type = NULL;
#define MB_FLUSH()                                                                                                     \
	do {                                                                                                               \
		if (pend_active) {                                                                                             \
			int ti = as->data.multi_bind.target_count++;                                                               \
			as->data.multi_bind.targets[ti].name = txt_dup((SynText){pend, pend_len});                                 \
			as->data.multi_bind.targets[ti].is_new = paren ? pend_new : 1;                                             \
			as->data.multi_bind.targets[ti].type = pend_type;                                                          \
			pend_active = 0;                                                                                           \
			pend_new = 0;                                                                                              \
			pend_type = NULL;                                                                                          \
		}                                                                                                              \
	} while (0)
		for (int i = 0; i < eq_idx; i++) {
			SyntaxElem *ch = &s.node->children[i];
			if (ch->tag == SE_NODE) {
				SyntaxNodeKind k = ch->as.node->kind;
				if (k == SN_NAME_EXPR) { /* the first shorthand target is a NAME_EXPR */
					MB_FLUSH();
					SynText t = sv_token((SyntaxView){ch->as.node, s.src}, TOK_IDENT);
					pend = t.ptr;
					pend_len = (int)t.len;
					pend_active = 1;
				} else if (k >= SN_TYPE_REF && k <= SN_TYPE_FUNC && pend_active) {
					pend_type = lower_type_cst((SyntaxView){ch->as.node, s.src});
				}
				continue;
			}
			TokenKind tk = ch->as.token.kind;
			if (tk == TOK_IDENT) {
				MB_FLUSH();
				pend = s.src + ch->as.token.offset;
				pend_len = (int)ch->as.token.length;
				pend_active = 1;
			} else if (tk == TOK_COLON && pend_active) {
				pend_new = 1; /* `name:` → newly declared (paren form) */
			} else if (tk == TOK_COMMA) {
				MB_FLUSH();
			}
		}
		MB_FLUSH();
#undef MB_FLUSH
		/* value: first expr node after the `=` */
		for (int i = eq_idx + 1; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_NODE) {
				SyntaxNodeKind k = s.node->children[i].as.node->kind;
				if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR) {
					as->data.multi_bind.value = lower_expr_cst((SyntaxView){s.node->children[i].as.node, s.src});
					break;
				}
			}
		break;
	}
	case SN_PROC_CALL_STMT: {
		/* `foo(in)(out)` — lowered as a multi-bind: value = the call `foo(in)`, targets = the
		 * out-args. Codegen passes the targets' addresses as the proc's out-pointers. */
		as->kind = HIR_STMT_MULTI_BIND;
		as->data.multi_bind.from_shorthand = 0;
		int nout = 0;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_NODE && s.node->children[i].as.node->kind == SN_OUT_ARG)
				nout++;
		as->data.multi_bind.targets = calloc(nout ? nout : 1, sizeof(HirBindingTarget));
		as->data.multi_bind.target_count = 0;
		as->data.multi_bind.value = NULL;
		for (int i = 0; i < s.node->child_count; i++) {
			if (s.node->children[i].tag != SE_NODE)
				continue;
			SyntaxNode *cn = s.node->children[i].as.node;
			SyntaxView cnv = (SyntaxView){cn, s.src};
			if (cn->kind == SN_CALL_EXPR) {
				as->data.multi_bind.value = lower_expr_cst(cnv);
			} else if (cn->kind == SN_OUT_ARG) {
				int ti = as->data.multi_bind.target_count++;
				as->data.multi_bind.targets[ti].name = txt_dup(sv_token(cnv, TOK_IDENT));
				as->data.multi_bind.targets[ti].is_new = sv_has_token(cnv, TOK_COLON);
				as->data.multi_bind.targets[ti].type =
				    sv_type_count(cnv) > 0 ? lower_type_cst(sv_type_at(cnv, 0)) : NULL;
			}
		}
		break;
	}
	case SN_BLOCK: {
		/* a surface `{ … }` block statement */
		as->kind = HIR_STMT_BLOCK;
		as->data.block.stmts = syntax_lower_body(s, &as->data.block.count);
		break;
	}
	case SN_MATCH_STMT: {
		/* Desugar `match scrut { p0 : b0, …, _ : bdef }` into a block:
		 *   __match := scrut;
		 *   if (__match == p0) { b0 } else if (__match == p1) { b1 } … else { bdef }
		 * Int/char-literal patterns and `_` are handled here; string/enum patterns are
		 * lowered in their own steps (str_eq chain / enum tag). */
		SyntaxView scrut = sv_node_at_expr(s, 0);
		char tmp[40];
		snprintf(tmp, sizeof(tmp), "__match_%u", (unsigned)sv_id(s));

		int narm = sv_count(s, SN_MATCH_ARM);

		/* If any arm pattern is a string literal, the scrutinee is a string: type the
		 * temp as char[] so it isn't inferred as i32 (which would mis-store the i8*). */
		int has_string_arm = 0;
		for (int i = 0; i < narm && !has_string_arm; i++) {
			SyntaxView arm = sv_child_at(s, SN_MATCH_ARM, i);
			for (int c = 0; c < arm.node->child_count; c++)
				if (arm.node->children[c].tag == SE_TOKEN && arm.node->children[c].as.token.kind == TOK_STRING) {
					has_string_arm = 1;
					break;
				}
		}

		HirStmt *bind = hir_stmt_create(HIR_STMT_BIND);
		bind->data.bind_stmt.names = calloc(1, sizeof(char *));
		bind->data.bind_stmt.names[0] = dupz(tmp);
		bind->data.bind_stmt.name_count = 1;
		bind->data.bind_stmt.type = NULL;
		if (has_string_arm) {
			HirType *ct = hir_type_create(HIR_TYPE_ARRAY);
			ct->elem = hir_type_create(HIR_TYPE_CHAR);
			bind->data.bind_stmt.type = ct;
		}
		bind->data.bind_stmt.value = lower_expr_cst(scrut);
		/* wildcard `_` arm becomes the final else */
		HirStmt **chain = NULL;
		int chain_count = 0;
		for (int i = 0; i < narm; i++) {
			SyntaxView arm = sv_child_at(s, SN_MATCH_ARM, i);
			SynText pt = {NULL, 0};
			TokenKind pk = TOK_EOF;
			for (int c = 0; c < arm.node->child_count; c++)
				if (arm.node->children[c].tag == SE_TOKEN) {
					TokenKind k = arm.node->children[c].as.token.kind;
					if (k == TOK_COLON)
						break; /* the pattern ends at the arm `:` */
					if (k == TOK_NUMBER || k == TOK_STRING || k == TOK_CHAR_LIT || k == TOK_IDENT) {
						pk = k;
						pt = (SynText){arm.src + arm.node->children[c].as.token.offset,
						               arm.node->children[c].as.token.length};
						/* keep scanning: in a qualified `Enum.case` the LAST ident before `:` is the case */
					}
				}
			if (pk == TOK_IDENT && pt.len == 1 && pt.ptr[0] == '_') {
				chain = syntax_lower_body(arm, &chain_count); /* wildcard → default else */
				break;
			}
		}
		/* build the if-chain from last arm to first (non-wildcard arms only) */
		for (int i = narm - 1; i >= 0; i--) {
			SyntaxView arm = sv_child_at(s, SN_MATCH_ARM, i);
			SynText pt = {NULL, 0};
			TokenKind pk = TOK_EOF;
			for (int c = 0; c < arm.node->child_count; c++)
				if (arm.node->children[c].tag == SE_TOKEN) {
					TokenKind k = arm.node->children[c].as.token.kind;
					if (k == TOK_COLON)
						break; /* the pattern ends at the arm `:` */
					if (k == TOK_NUMBER || k == TOK_STRING || k == TOK_CHAR_LIT || k == TOK_IDENT) {
						pk = k;
						pt = (SynText){arm.src + arm.node->children[c].as.token.offset,
						               arm.node->children[c].as.token.length};
						/* keep scanning: in a qualified `Enum.case` the LAST ident before `:` is the case */
					}
				}
			if (pk == TOK_IDENT && pt.len == 1 && pt.ptr[0] == '_')
				continue; /* the wildcard, already captured */
			/* Comparison lexeme: int/char literal used directly; a bare IDENT is an enum variant,
			 * resolved to its int value. (String patterns are handled in a later step.) */
			char enumbuf[32];
			const char *litlex = NULL;
			int is_string = 0;
			if (pk == TOK_NUMBER || pk == TOK_CHAR_LIT) {
				litlex = NULL; /* use pt verbatim below */
			} else if (pk == TOK_STRING) {
				is_string = 1; /* string pattern → strcmp(scrut, "lit") == 0 */
			} else if (pk == TOK_IDENT && g_lower_sem) {
				char *vn = txt_dup(pt);
				long v = 0;
				int found = vn && semantic_find_enum_variant(g_lower_sem, vn, &v);
				free(vn);
				if (!found)
					continue; /* unknown bare pattern */
				snprintf(enumbuf, sizeof(enumbuf), "%ld", v);
				litlex = enumbuf;
			} else {
				continue; /* unsupported pattern form */
			}

			HirStmt *iff = hir_stmt_create(HIR_STMT_IF);
			HirExpr *cond;
			if (is_string) {
				/* `streq(__match, "lit") != 0` — streq (pure arche, in core.arche) returns
				 * 1 when the strings are equal. */
				HirExpr *call = hir_expr_create(HIR_EXPR_CALL);
				HirExpr *callee = hir_expr_create(HIR_EXPR_NAME);
				callee->data.name.name = dupz("streq");
				call->data.call.callee = callee;
				call->data.call.args = calloc(2, sizeof(HirExpr *));
				/* Inline the scrutinee (a string lvalue) per arm rather than a typed temp —
				 * the char[] temp would be mis-inferred as i32. */
				HirExpr *sarg = lower_expr_cst(scrut);
				HirExpr *parg = hir_expr_create(HIR_EXPR_STRING);
				int slen = 0;
				parg->data.string.value = syntax_decode_string(pt, &slen);
				parg->data.string.length = slen;
				call->data.call.args[0] = sarg;
				call->data.call.args[1] = parg;
				call->data.call.arg_count = 2;
				/* streq returns bool (i8). Tag the synthetic call's resolved type so codegen
				 * coerces its i8 result to the comparison width (zext to i32) instead of treating
				 * it as the i32 default — otherwise the `!= 0` emits `icmp ne i32 <i8>` (bad IR). */
				call->resolved.tag = HIR_TYPE_NAMED;
				call->resolved.name = "bool";
				HirExpr *zero = hir_expr_create(HIR_EXPR_LITERAL);
				zero->data.literal.lexeme = dupz("0");
				cond = hir_expr_create(HIR_EXPR_BINARY);
				cond->data.binary.op = OP_NEQ;
				cond->data.binary.left = call;
				cond->data.binary.right = zero;
			} else {
				HirExpr *nm = hir_expr_create(HIR_EXPR_NAME);
				nm->data.name.name = dupz(tmp);
				HirExpr *lit = hir_expr_create(HIR_EXPR_LITERAL);
				lit->data.literal.lexeme = litlex ? dupz(litlex) : txt_dup(pt);
				cond = hir_expr_create(HIR_EXPR_BINARY);
				cond->data.binary.op = OP_EQ;
				cond->data.binary.left = nm;
				cond->data.binary.right = lit;
			}
			iff->data.if_stmt.cond = cond;
			iff->data.if_stmt.then_body = syntax_lower_body(arm, &iff->data.if_stmt.then_count);
			iff->data.if_stmt.else_body = chain;
			iff->data.if_stmt.else_count = chain_count;
			chain = calloc(1, sizeof(HirStmt *));
			chain[0] = iff;
			chain_count = 1;
		}

		as->kind = HIR_STMT_BLOCK;
		as->data.block.stmts = calloc(2, sizeof(HirStmt *));
		int nstmt = 0;
		/* String matches inline the scrutinee per arm, so the temp bind is unused. */
		if (has_string_arm)
			hir_stmt_free(bind);
		else
			as->data.block.stmts[nstmt++] = bind;
		if (chain_count > 0)
			as->data.block.stmts[nstmt++] = chain[0];
		as->data.block.count = nstmt;
		free(chain);
		break;
	}
	default:
		/* anything still unhandled: placeholder no-op. */
		as->kind = HIR_STMT_EXPR;
		as->data.expr_stmt.expr = NULL;
		break;
	}
	return as;
}

static HirParam *lower_param_cst(SyntaxView p) {
	HirParam *ap = hir_param_create(NULL, NULL);
	ap->name = sv_dup(sv_child(p, SN_PARAM_NAME));
	ap->type = lower_type_cst(sv_type_at(p, 0)); /* NULL for sys params */
	ap->is_own = sv_has_token(p, TOK_OWN);
	return ap;
}

/* ---- tuple-group registry (syntax tree equivalent of main.c expand_archetype_tuple_groups) ----
 * A top-level `pos (x, y) :: T` declares a tuple group: a bare archetype field `pos`
 * expands to flat columns `pos_x`, `pos_y` (each of type T). */
typedef struct {
	char *name;
	char **suffix;
	int nsuf;
	HirType member;
} CstTupleGroup;
static CstTupleGroup g_tgroups[64];
static int g_tgroup_count = 0;

/* Register a tuple group `name (s0, s1, …) :: T` from a contiguous child range
 * [start,end) of `parent`: `name` is the IDENT before `(`, the suffixes are the
 * IDENTs inside `()`, and the member type is the first type node after `)`. */
static void register_tgroup(const SyntaxNode *parent, const char *src, int start, int end, const char *forced_name) {
	if (g_tgroup_count >= 64)
		return;
	CstTupleGroup *g = &g_tgroups[g_tgroup_count];
	g->name = forced_name ? dupz(forced_name) : NULL;
	g->suffix = calloc(end - start + 1, sizeof(char *));
	g->nsuf = 0;
	g->member = (HirType){.tag = HIR_TYPE_UNKNOWN};
	int in_paren = 0;
	for (int k = start; k < end; k++) {
		SyntaxElem *ch = &parent->children[k];
		if (ch->tag == SE_TOKEN) {
			if (ch->as.token.kind == TOK_LPAREN)
				in_paren = 1;
			else if (ch->as.token.kind == TOK_RPAREN)
				in_paren = 0;
			else if (ch->as.token.kind == TOK_IDENT) {
				SynText t = {src + ch->as.token.offset, ch->as.token.length};
				if (!in_paren && !g->name)
					g->name = txt_dup(t);
				else if (in_paren)
					g->suffix[g->nsuf++] = txt_dup(t);
			}
		} else {
			SyntaxNodeKind kk = ch->as.node->kind;
			if (kk >= SN_TYPE_REF && kk <= SN_TYPE_FUNC && g->member.tag == HIR_TYPE_UNKNOWN) {
				HirType *mt = lower_type_cst((SyntaxView){ch->as.node, src});
				if (mt)
					g->member = *mt;
			}
		}
	}
	if (g->name && g->nsuf > 0)
		g_tgroup_count++;
}

/* Scan an archetype-body node (legacy SN_ARCHETYPE_DECL or unified SN_ARCH_EXPR) for inline
 * tuple fields `pos (x, y) :: T` and register each as a tuple group. */
static void register_arch_tgroups(const SyntaxNode *arch, const char *src) {
	for (int k = 0; k < arch->child_count; k++) {
		if (arch->children[k].tag != SE_NODE || arch->children[k].as.node->kind != SN_FIELD_NAME)
			continue;
		/* a `(` immediately following this FIELD_NAME marks an inline tuple */
		int j = k + 1;
		if (j < arch->child_count && arch->children[j].tag == SE_TOKEN &&
		    arch->children[j].as.token.kind == TOK_LPAREN) {
			int e = j;
			while (e < arch->child_count &&
			       !(arch->children[e].tag == SE_NODE && arch->children[e].as.node->kind == SN_FIELD_NAME && e != k))
				e++;
			char *nm = sv_dup((SyntaxView){arch->children[k].as.node, src});
			register_tgroup(arch, src, j, e, nm);
			free(nm);
		}
	}
}

/* Find an SN_ARCH_EXPR among a node's direct children (the RHS of `Name :: arche { … }`). */
static const SyntaxNode *arch_expr_child(const SyntaxNode *d) {
	for (int i = 0; i < d->child_count; i++)
		if (d->children[i].tag == SE_NODE && d->children[i].as.node->kind == SN_ARCH_EXPR)
			return d->children[i].as.node;
	return NULL;
}

/* True if the decl is decorated (its first token is `@`). A decorator with args — `@allow(x)`,
 * `@implements(dev.foo)` — has a `(` that must NOT be mistaken for a tuple-group paren. */
static int decl_is_decorated(const SyntaxNode *d) {
	for (int i = 0; i < d->child_count; i++)
		if (d->children[i].tag == SE_TOKEN)
			return d->children[i].as.token.kind == TOK_AT;
	return 0;
}

static void build_tgroups(const SyntaxNode *root, const char *src) {
	g_tgroup_count = 0;
	for (int i = 0; i < root->child_count; i++) {
		if (root->children[i].tag != SE_NODE)
			continue;
		const SyntaxNode *d = root->children[i].as.node;
		const SyntaxNode *ae;
		/* unified archetype form `Name :: arche { … }` — scan its body for inline tuple fields */
		if (d->kind == SN_CONST_DECL && (ae = arch_expr_child(d)) != NULL)
			register_arch_tgroups(ae, src);
		/* top-level tuple group: `pos (x, y) :: T` (a const-decl with a direct `(`, not decorated). */
		else if (d->kind == SN_CONST_DECL && !decl_is_decorated(d) && sv_has_token((SyntaxView){d, src}, TOK_LPAREN))
			register_tgroup(d, src, 0, d->child_count, NULL);
		/* legacy inline archetype tuple field: `pos (x, y) :: T` inside `arche { … }` */
		else if (d->kind == SN_ARCHETYPE_DECL)
			register_arch_tgroups(d, src);
	}
}

static CstTupleGroup *tgroup_lookup(const char *name) {
	if (!name)
		return NULL;
	for (int i = 0; i < g_tgroup_count; i++)
		if (strcmp(g_tgroups[i].name, name) == 0)
			return &g_tgroups[i];
	return NULL;
}

/* Collapse a nested tuple-field access `arch.pos.x` (FIELD over FIELD over NAME,
 * where `pos` is a tuple group with component `x`) into the flattened column
 * `arch.pos_x`, matching the flattened archetype columns. Recurses over the tree. */
static void tuple_collapse_expr(HirExpr *e) {
	if (!e)
		return;
	switch (e->kind) {
	case HIR_EXPR_FIELD: {
		tuple_collapse_expr(e->data.field.base);
		HirExpr *b = e->data.field.base;
		if (b && b->kind == HIR_EXPR_FIELD && b->data.field.base && b->data.field.base->kind == HIR_EXPR_NAME) {
			CstTupleGroup *g = tgroup_lookup(b->data.field.field_name);
			const char *comp = e->data.field.field_name;
			if (g) {
				for (int s = 0; s < g->nsuf; s++)
					if (strcmp(g->suffix[s], comp) == 0) {
						char *combined = malloc(strlen(b->data.field.field_name) + 1 + strlen(comp) + 1);
						sprintf(combined, "%s_%s", b->data.field.field_name, comp);
						free(e->data.field.field_name);
						e->data.field.field_name = combined;
						e->data.field.base = b->data.field.base; /* drop the middle FIELD (leaked) */
						break;
					}
			}
		}
		break;
	}
	case HIR_EXPR_INDEX:
		tuple_collapse_expr(e->data.index.base);
		for (int i = 0; i < e->data.index.index_count; i++)
			tuple_collapse_expr(e->data.index.indices[i]);
		break;
	case HIR_EXPR_BINARY:
		tuple_collapse_expr(e->data.binary.left);
		tuple_collapse_expr(e->data.binary.right);
		break;
	case HIR_EXPR_UNARY:
		tuple_collapse_expr(e->data.unary.operand);
		break;
	case HIR_EXPR_CALL:
		tuple_collapse_expr(e->data.call.callee);
		for (int i = 0; i < e->data.call.arg_count; i++)
			tuple_collapse_expr(e->data.call.args[i]);
		break;
	case HIR_EXPR_ALLOC:
		for (int i = 0; i < e->data.alloc.field_count; i++)
			tuple_collapse_expr(e->data.alloc.field_values[i]);
		tuple_collapse_expr(e->data.alloc.init_length);
		break;
	case HIR_EXPR_ARRAY_LITERAL:
		for (int i = 0; i < e->data.array_literal.element_count; i++)
			tuple_collapse_expr(e->data.array_literal.elements[i]);
		break;
	default:
		break;
	}
}
static void tuple_collapse_stmt(HirStmt *s) {
	if (!s)
		return;
	switch (s->kind) {
	case HIR_STMT_BIND:
		tuple_collapse_expr(s->data.bind_stmt.value);
		break;
	case HIR_STMT_ASSIGN:
		tuple_collapse_expr(s->data.assign_stmt.target);
		tuple_collapse_expr(s->data.assign_stmt.value);
		break;
	case HIR_STMT_FOR:
		tuple_collapse_stmt(s->data.for_stmt.init);
		tuple_collapse_expr(s->data.for_stmt.cond);
		tuple_collapse_stmt(s->data.for_stmt.incr);
		tuple_collapse_expr(s->data.for_stmt.iterable);
		for (int i = 0; i < s->data.for_stmt.body_count; i++)
			tuple_collapse_stmt(s->data.for_stmt.body[i]);
		break;
	case HIR_STMT_IF:
		tuple_collapse_expr(s->data.if_stmt.cond);
		for (int i = 0; i < s->data.if_stmt.then_count; i++)
			tuple_collapse_stmt(s->data.if_stmt.then_body[i]);
		for (int i = 0; i < s->data.if_stmt.else_count; i++)
			tuple_collapse_stmt(s->data.if_stmt.else_body[i]);
		break;
	case HIR_STMT_EXPR:
		tuple_collapse_expr(s->data.expr_stmt.expr);
		break;
	case HIR_STMT_RETURN:
		for (int i = 0; i < s->data.return_stmt.count; i++)
			tuple_collapse_expr(s->data.return_stmt.values[i]);
		break;
	case HIR_STMT_MULTI_BIND:
		tuple_collapse_expr(s->data.multi_bind.value);
		break;
	case HIR_STMT_EACH_FIELD:
		for (int i = 0; i < s->data.each_field.body_count; i++)
			tuple_collapse_stmt(s->data.each_field.body[i]);
		break;
	case HIR_STMT_BLOCK: /* match / surface `{ }` desugar */
		for (int i = 0; i < s->data.block.count; i++)
			tuple_collapse_stmt(s->data.block.stmts[i]);
		break;
	default:
		break;
	}
}
static void tuple_collapse_decl(HirDecl *d) {
	switch (d->kind) {
	case HIR_DECL_PROC:
		for (int i = 0; i < d->data.proc->stmt_count; i++)
			tuple_collapse_stmt(d->data.proc->stmts[i]);
		break;
	case HIR_DECL_SYS:
		for (int i = 0; i < d->data.sys->stmt_count; i++)
			tuple_collapse_stmt(d->data.sys->stmts[i]);
		break;
	case HIR_DECL_FUNC:
		for (int i = 0; i < d->data.func->stmt_count; i++)
			tuple_collapse_stmt(d->data.func->stmts[i]);
		break;
	default:
		break;
	}
}

/* True if this decl node carries a `@drop` decorator (a direct `@ drop` token pair). */
static int syntax_decl_has_drop_decorator(SyntaxView d) {
	if (!sv_present(d))
		return 0;
	int n = d.node->child_count;
	for (int i = 0; i + 1 < n; i++) {
		const SyntaxElem *e1 = &d.node->children[i];
		if (e1->tag != SE_TOKEN || e1->as.token.kind != TOK_AT)
			continue;
		const SyntaxElem *e2 = &d.node->children[i + 1];
		if (e2->tag != SE_TOKEN || e2->as.token.kind != TOK_IDENT)
			continue;
		if (e2->as.token.length == 4 && memcmp(d.src + e2->as.token.offset, "drop", 4) == 0)
			return 1;
	}
	return 0;
}
static int syntax_decl_has_intrinsic_decorator(SyntaxView d) {
	if (!sv_present(d))
		return 0;
	int n = d.node->child_count;
	for (int i = 0; i + 1 < n; i++) {
		const SyntaxElem *e1 = &d.node->children[i];
		if (e1->tag != SE_TOKEN || e1->as.token.kind != TOK_AT)
			continue;
		const SyntaxElem *e2 = &d.node->children[i + 1];
		if (e2->tag != SE_TOKEN || e2->as.token.kind != TOK_IDENT)
			continue;
		if (e2->as.token.length == 9 && memcmp(d.src + e2->as.token.offset, "intrinsic", 9) == 0)
			return 1;
	}
	return 0;
}

/* The op category a `policy` decl serves, from `@policy(<category>)`: 1=bounds, 2=pool, 3=divide, 0=none.
 * `policy` lexes as TOK_POLICY (a keyword), so the sequence is `@ policy ( <cat> )`. */
static int syntax_decl_policy_category(SyntaxView d) {
	if (!sv_present(d))
		return 0;
	int n = d.node->child_count;
	for (int i = 0; i + 4 < n; i++) {
		const SyntaxElem *at = &d.node->children[i];
		const SyntaxElem *kw = &d.node->children[i + 1];
		const SyntaxElem *lp = &d.node->children[i + 2];
		const SyntaxElem *cat = &d.node->children[i + 3];
		const SyntaxElem *rp = &d.node->children[i + 4];
		if (at->tag != SE_TOKEN || at->as.token.kind != TOK_AT || kw->tag != SE_TOKEN ||
		    kw->as.token.kind != TOK_POLICY)
			continue;
		if (lp->tag != SE_TOKEN || lp->as.token.kind != TOK_LPAREN || cat->tag != SE_TOKEN ||
		    cat->as.token.kind != TOK_IDENT || rp->tag != SE_TOKEN || rp->as.token.kind != TOK_RPAREN)
			continue;
		const char *p = d.src + cat->as.token.offset;
		size_t len = cat->as.token.length;
		if (len == 6 && memcmp(p, "bounds", 6) == 0)
			return 1;
		if (len == 4 && memcmp(p, "pool", 4) == 0)
			return 2;
		if (len == 6 && memcmp(p, "divide", 6) == 0)
			return 3;
		return 0;
	}
	return 0;
}

/* Map a category ident (`bounds`/`pool`/`divide`) to its int code (1/2/3), or 0 if unknown. */
static int default_category_code(const char *p, size_t len) {
	if (len == 6 && memcmp(p, "bounds", 6) == 0)
		return 1;
	if (len == 4 && memcmp(p, "pool", 4) == 0)
		return 2;
	if (len == 6 && memcmp(p, "divide", 6) == 0)
		return 3;
	return 0;
}

/* Lower a `@default(<kind>, <category>, <policy>)` directive node into a HIR_DECL_DEFAULT. The node's
 * tokens are `@ default ( <kind-kw> , <category> , <policy> )`; read the three argument tokens by
 * position. `category`/`policy` are left raw here (validated in semantic). */
static HirDecl *lower_default_directive(SyntaxView d) {
	HirDecl *dd = hir_decl_create(HIR_DECL_DEFAULT);
	HirDefaultDecl *df = calloc(1, sizeof(HirDefaultDecl));
	df->effect_kind = -1;
	df->category = 0;
	df->policy = NULL;
	int n = d.node ? d.node->child_count : 0;
	int seen = 0; /* argument position: 0=kind, 1=category, 2=policy */
	for (int i = 0; i < n && seen < 3; i++) {
		if (d.node->children[i].tag != SE_TOKEN)
			continue;
		TokenKind tk = d.node->children[i].as.token.kind;
		uint32_t off = d.node->children[i].as.token.offset;
		uint32_t tlen = d.node->children[i].as.token.length;
		if (tk == TOK_AT || tk == TOK_LPAREN || tk == TOK_RPAREN || tk == TOK_COMMA)
			continue;
		/* The leading `default` ident is consumed before the first real arg: skip it once. */
		if (seen == 0 && tk == TOK_IDENT && tlen == 7 && memcmp(d.src + off, "default", 7) == 0)
			continue;
		if (seen == 0) {
			df->effect_kind = (tk == TOK_FUNC) ? 1 : 0; /* proc default */
		} else if (seen == 1) {
			df->category = default_category_code(d.src + off, tlen);
		} else {
			df->policy = malloc(tlen + 1);
			memcpy(df->policy, d.src + off, tlen);
			df->policy[tlen] = '\0';
		}
		seen++;
	}
	dd->data.default_decl = df;
	return dd;
}

/* ===== Unified-grammar RHS value forms (P2) =====
 * Build the HIR decl from an RHS value-form node `f` with the name from the binding LHS.
 * Mirrors the legacy keyword-led cases below (dead once old syntax is removed). `name` owned. */

static HirDecl *lower_proc_from(SyntaxView f, char *name) {
	HirDecl *ad = hir_decl_create(HIR_DECL_PROC);
	HirProcDecl *ap = calloc(1, sizeof(HirProcDecl));
	ap->name = name;
	/* Foreign (FFI-bodied): a proc value-form with no `{` body block (parser emits a bodiless
	 * proc value-form only inside a `#foreign` region). Mirrors semantic.c build_proc_from. */
	ap->is_extern = !sv_has_token(f, TOK_LBRACE);
	int np = sv_count(f, SN_PARAM);
	ap->params = calloc(np ? np : 1, sizeof(HirParam *));
	for (int i = 0; i < np; i++)
		ap->params[i] = lower_param_cst(sv_child_at(f, SN_PARAM, i));
	ap->param_count = np;
	int nout = sv_count(f, SN_OUT_PARAM);
	ap->out_params = calloc(nout ? nout : 1, sizeof(HirParam *));
	for (int i = 0; i < nout; i++)
		ap->out_params[i] = lower_param_cst(sv_child_at(f, SN_OUT_PARAM, i));
	ap->out_param_count = nout;
	ap->stmts = syntax_lower_body(f, &ap->stmt_count);
	ad->data.proc = ap;
	return ad;
}

static HirDecl *lower_func_from(SyntaxView f, char *name) {
	HirDecl *ad = hir_decl_create(HIR_DECL_FUNC);
	HirFuncDecl *af = calloc(1, sizeof(HirFuncDecl));
	af->name = name;
	af->is_extern = 0; /* funcs are never foreign — FFI bodies are procs */
	int np = sv_count(f, SN_PARAM);
	af->params = calloc(np ? np : 1, sizeof(HirParam *));
	for (int i = 0; i < np; i++)
		af->params[i] = lower_param_cst(sv_child_at(f, SN_PARAM, i));
	af->param_count = np;
	int nt = sv_type_count(f);
	af->return_types = calloc(nt ? nt : 1, sizeof(HirType *));
	af->return_type_count = 0;
	for (int i = 0; i < nt; i++)
		af->return_types[af->return_type_count++] = lower_type_cst(sv_type_at(f, i));
	af->stmts = syntax_lower_body(f, &af->stmt_count);
	ad->data.func = af;
	return ad;
}

static HirDecl *lower_sys_from(SyntaxView f, char *name) {
	HirDecl *ad = hir_decl_create(HIR_DECL_SYS);
	HirSysDecl *as = calloc(1, sizeof(HirSysDecl));
	as->name = name;
	as->stmts = syntax_lower_body(f, &as->stmt_count);
	int np = sv_count(f, SN_PARAM);
	int pcount = 0;
	for (int i = 0; i < np; i++) {
		char *pn = sv_dup(sv_child(sv_child_at(f, SN_PARAM, i), SN_PARAM_NAME));
		CstTupleGroup *g = tgroup_lookup(pn);
		pcount += g ? g->nsuf : 1;
		free(pn);
	}
	as->params = calloc(pcount ? pcount : 1, sizeof(HirParam *));
	as->param_count = 0;
	for (int i = 0; i < np; i++) {
		SyntaxView p = sv_child_at(f, SN_PARAM, i);
		char *pn = sv_dup(sv_child(p, SN_PARAM_NAME));
		CstTupleGroup *g = tgroup_lookup(pn);
		if (!g) {
			as->params[as->param_count++] = lower_param_cst(p);
			free(pn);
			continue;
		}
		for (int sx = 0; sx < as->stmt_count; sx++)
			tuple_rewrite_stmt(as->stmts[sx], pn);
		int is_own = sv_has_token(p, TOK_OWN);
		for (int j = 0; j < g->nsuf; j++) {
			HirParam *ap = hir_param_create(NULL, NULL);
			ap->name = malloc(strlen(pn) + 1 + strlen(g->suffix[j]) + 1);
			sprintf(ap->name, "%s_%s", pn, g->suffix[j]);
			HirType *mt = hir_type_create(HIR_TYPE_UNKNOWN);
			*mt = g->member;
			ap->type = mt;
			ap->is_own = is_own;
			as->params[as->param_count++] = ap;
		}
		free(pn);
	}
	ad->data.sys = as;
	return ad;
}

static HirDecl *lower_archetype_from(SyntaxView f, char *name) {
	HirDecl *ad = hir_decl_create(HIR_DECL_ARCHETYPE);
	HirArchetypeDecl *aa = calloc(1, sizeof(HirArchetypeDecl));
	aa->name = name;
	int nf = sv_count(f, SN_FIELD_NAME);
	int cap = nf > 0 ? nf : 1;
	aa->fields = calloc(cap, sizeof(HirField *));
	aa->field_count = 0;
	for (int i = 0; i < f.node->child_count; i++) {
		if (f.node->children[i].tag != SE_NODE || f.node->children[i].as.node->kind != SN_FIELD_NAME)
			continue;
		SyntaxView fn = {f.node->children[i].as.node, f.src};
		SyntaxView ty = {NULL, f.src};
		for (int k = i + 1; k < f.node->child_count; k++)
			if (f.node->children[k].tag == SE_NODE) {
				SyntaxNodeKind kk = f.node->children[k].as.node->kind;
				if (kk == SN_FIELD_NAME)
					break;
				if (kk >= SN_TYPE_REF && kk <= SN_TYPE_FUNC) {
					ty.node = f.node->children[k].as.node;
					break;
				}
			}
		char *raw = sv_dup(fn);
		if (aa->field_count >= cap) {
			cap *= 2;
			aa->fields = realloc(aa->fields, cap * sizeof(HirField *));
		}
		CstTupleGroup *g = tgroup_lookup(raw);
		if (g) {
			for (int sx = 0; sx < g->nsuf; sx++) {
				if (aa->field_count >= cap) {
					cap *= 2;
					aa->fields = realloc(aa->fields, cap * sizeof(HirField *));
				}
				HirField *cf = hir_field_create(FIELD_COLUMN, NULL, NULL);
				cf->name = malloc(strlen(raw) + 1 + strlen(g->suffix[sx]) + 1);
				sprintf(cf->name, "%s_%s", raw, g->suffix[sx]);
				HirType *mt = hir_type_create(HIR_TYPE_UNKNOWN);
				*mt = g->member;
				cf->type = mt;
				aa->fields[aa->field_count++] = cf;
			}
			free(raw);
			continue;
		}
		HirField *af = hir_field_create(FIELD_COLUMN, NULL, NULL);
		af->name = sv_dup(fn);
		if (sv_present(ty)) {
			af->type = lower_type_cst(ty);
		} else {
			const char *r = g_lower_sem ? semantic_resolve_type_alias(g_lower_sem, raw) : raw;
			HirType *t = hir_type_create(HIR_TYPE_UNKNOWN);
			*t = map_type_str(r);
			if (t->tag == HIR_TYPE_NAMED) {
				char *nm = malloc(strlen(r) + 1);
				strcpy(nm, r);
				t->name = nm;
			}
			af->type = t;
		}
		free(raw);
		aa->fields[aa->field_count++] = af;
	}
	ad->data.archetype = aa;
	return ad;
}

/* Synthetic archetypes minted from anonymous `arche { … }` literals, flushed into the program's
 * decl list by lower_to_hir (reset per compile). Deterministic naming dedupes identical literals;
 * canonical_archetype_decl then folds each onto a structurally-identical named shape. */
static HirDecl *g_synth_arch[64];
static int g_synth_arch_count = 0;

static const char *synth_archetype_name(SyntaxView arch_expr) {
	/* Deterministic name from the sorted field-name set, so identical literals reuse one decl. */
	char *names[64];
	int n = 0;
	for (int i = 0; i < arch_expr.node->child_count && n < 64; i++)
		if (arch_expr.node->children[i].tag == SE_NODE && arch_expr.node->children[i].as.node->kind == SN_FIELD_NAME)
			names[n++] = sv_dup((SyntaxView){arch_expr.node->children[i].as.node, arch_expr.src});
	for (int i = 1; i < n; i++) { /* insertion sort */
		char *key = names[i];
		int j = i - 1;
		while (j >= 0 && strcmp(names[j], key) > 0) {
			names[j + 1] = names[j];
			j--;
		}
		names[j + 1] = key;
	}
	char buf[256];
	int bl = snprintf(buf, sizeof(buf), "__shape");
	for (int i = 0; i < n && bl < (int)sizeof(buf); i++)
		bl += snprintf(buf + bl, sizeof(buf) - (size_t)bl, "_%s", names[i]);
	for (int i = 0; i < n; i++)
		free(names[i]);
	for (int i = 0; i < g_synth_arch_count; i++)
		if (strcmp(g_synth_arch[i]->data.archetype->name, buf) == 0)
			return g_synth_arch[i]->data.archetype->name; /* dedupe identical literals */
	if (g_synth_arch_count >= 64)
		return NULL;
	HirDecl *d = lower_archetype_from(arch_expr, dupz(buf));
	g_synth_arch[g_synth_arch_count++] = d;
	return d->data.archetype->name;
}

static HirDecl *lower_func_group_from(SyntaxView f, char *name) {
	HirDecl *ad = hir_decl_create(HIR_DECL_FUNC_GROUP);
	HirFuncGroupDecl *fg = calloc(1, sizeof(HirFuncGroupDecl));
	fg->name = name;
	int nmem = 0;
	for (int i = 0; i < f.node->child_count; i++)
		if (f.node->children[i].tag == SE_TOKEN && f.node->children[i].as.token.kind == TOK_IDENT)
			nmem++;
	fg->member_names = calloc(nmem ? nmem : 1, sizeof(char *));
	fg->member_count = 0;
	for (int i = 0; i < f.node->child_count; i++)
		if (f.node->children[i].tag == SE_TOKEN && f.node->children[i].as.token.kind == TOK_IDENT) {
			SynText t = {f.src + f.node->children[i].as.token.offset, f.node->children[i].as.token.length};
			fg->member_names[fg->member_count++] = txt_dup(t);
		}
	ad->data.func_group = fg;
	return ad;
}

/* Binding LHS name: the IDENT immediately before the first top-level `:` (skips `@decorator`). */
static SynText lower_binding_name(SyntaxView d) {
	SynText last = {NULL, 0};
	for (int i = 0; i < d.node->child_count; i++) {
		SyntaxElem *e = &d.node->children[i];
		if (e->tag != SE_TOKEN)
			continue;
		if (e->as.token.kind == TOK_COLON)
			break;
		if (e->as.token.kind == TOK_IDENT)
			last = (SynText){d.src + e->as.token.offset, e->as.token.length};
	}
	return last;
}

static SyntaxView lower_rhs_form(SyntaxView d) {
	for (int i = 0; i < d.node->child_count; i++) {
		if (d.node->children[i].tag != SE_NODE)
			continue;
		SyntaxNodeKind k = d.node->children[i].as.node->kind;
		if (k == SN_PROC_EXPR || k == SN_FUNC_EXPR || k == SN_POLICY_EXPR || k == SN_SYS_EXPR || k == SN_ARCH_EXPR ||
		    k == SN_GROUP_EXPR || k == SN_ENUM_EXPR || k == SN_TYPE_PROC || k == SN_TYPE_FUNC) {
			SyntaxView v = {d.node->children[i].as.node, d.src};
			return v;
		}
	}
	SyntaxView none = {NULL, d.src};
	return none;
}

static HirDecl *lower_decl_cst(SyntaxView d) {
	switch (sv_kind(d)) {
	case SN_USE_DECL:
		return NULL;
	case SN_DEFAULT_DECL:
		return lower_default_directive(d);
	case SN_WORLD_DECL: {
		HirDecl *ad = hir_decl_create(HIR_DECL_WORLD);
		ad->data.world = calloc(1, sizeof(HirWorldDecl));
		ad->data.world->name = txt_dup(sv_token(d, TOK_IDENT));
		return ad;
	}
	case SN_ARCHETYPE_DECL: {
		HirDecl *ad = hir_decl_create(HIR_DECL_ARCHETYPE);
		HirArchetypeDecl *aa = calloc(1, sizeof(HirArchetypeDecl));
		aa->name = sv_dup(sv_child(d, SN_TYPE_DEF_NAME));
		int nf = sv_count(d, SN_FIELD_NAME);
		int cap = nf > 0 ? nf : 1;
		aa->fields = calloc(cap, sizeof(HirField *));
		aa->field_count = 0;
		for (int i = 0; i < d.node->child_count; i++) {
			if (d.node->children[i].tag != SE_NODE || d.node->children[i].as.node->kind != SN_FIELD_NAME)
				continue;
			SyntaxView fn = {d.node->children[i].as.node, d.src};
			/* type = a type node before the next field; else a bare field (`arche { a, b }`)
			 * whose component type is the field name itself (resolved through aliases). */
			SyntaxView ty = {NULL, d.src};
			for (int k = i + 1; k < d.node->child_count; k++)
				if (d.node->children[k].tag == SE_NODE) {
					SyntaxNodeKind kk = d.node->children[k].as.node->kind;
					if (kk == SN_FIELD_NAME)
						break;
					if (kk >= SN_TYPE_REF && kk <= SN_TYPE_FUNC) {
						ty.node = d.node->children[k].as.node;
						break;
					}
				}
			char *raw = sv_dup(fn);
			/* bare field matching a top-level group, or an inline `pos (x,y) :: T` field
			 * (both registered in the tuple-group table) → one tuple-typed column. */
			if (aa->field_count >= cap) {
				cap *= 2;
				aa->fields = realloc(aa->fields, cap * sizeof(HirField *));
			}
			CstTupleGroup *g = tgroup_lookup(raw);
			if (g) {
				/* tuple group: flatten to scalar columns `pos_x`, `pos_y` (codegen has no tuple type).
				 * `pos.x`/`Body.pos.x` accesses are collapsed to the combined column name during statement lowering. */
				for (int sx = 0; sx < g->nsuf; sx++) {
					if (aa->field_count >= cap) {
						cap *= 2;
						aa->fields = realloc(aa->fields, cap * sizeof(HirField *));
					}
					HirField *cf = hir_field_create(FIELD_COLUMN, NULL, NULL);
					cf->name = malloc(strlen(raw) + 1 + strlen(g->suffix[sx]) + 1);
					sprintf(cf->name, "%s_%s", raw, g->suffix[sx]);
					HirType *mt = hir_type_create(HIR_TYPE_UNKNOWN);
					*mt = g->member;
					cf->type = mt;
					aa->fields[aa->field_count++] = cf;
				}
				free(raw);
				continue;
			}
			HirField *af = hir_field_create(FIELD_COLUMN, NULL, NULL);
			af->name = sv_dup(fn);
			if (sv_present(ty)) {
				af->type = lower_type_cst(ty);
			} else {
				const char *r = g_lower_sem ? semantic_resolve_type_alias(g_lower_sem, raw) : raw;
				HirType *t = hir_type_create(HIR_TYPE_UNKNOWN);
				*t = map_type_str(r);
				if (t->tag == HIR_TYPE_NAMED) {
					char *nm = malloc(strlen(r) + 1);
					strcpy(nm, r);
					t->name = nm;
				}
				af->type = t;
			}
			free(raw);
			aa->fields[aa->field_count++] = af;
		}
		ad->data.archetype = aa;
		return ad;
	}
	case SN_PROC_DECL: {
		HirDecl *ad = hir_decl_create(HIR_DECL_PROC);
		HirProcDecl *ap = calloc(1, sizeof(HirProcDecl));
		ap->name = sv_dup(sv_child(d, SN_FUNC_DEF_NAME));
		ap->is_extern = !sv_has_token(d, TOK_LBRACE);
		int np = sv_count(d, SN_PARAM);
		ap->params = calloc(np ? np : 1, sizeof(HirParam *));
		for (int i = 0; i < np; i++)
			ap->params[i] = lower_param_cst(sv_child_at(d, SN_PARAM, i));
		ap->param_count = np;
		/* out-params: the `(out)` list, written in place (0 = no outputs) */
		int nout = sv_count(d, SN_OUT_PARAM);
		ap->out_params = calloc(nout ? nout : 1, sizeof(HirParam *));
		for (int i = 0; i < nout; i++)
			ap->out_params[i] = lower_param_cst(sv_child_at(d, SN_OUT_PARAM, i));
		ap->out_param_count = nout;
		ap->stmts = syntax_lower_body(d, &ap->stmt_count);
		ad->data.proc = ap;
		return ad;
	}
	case SN_SYS_DECL: {
		HirDecl *ad = hir_decl_create(HIR_DECL_SYS);
		HirSysDecl *as = calloc(1, sizeof(HirSysDecl));
		as->name = sv_dup(sv_child(d, SN_FUNC_DEF_NAME));
		/* Lower the body first; a tuple-group param then expands into one scalar param
		 * per component (`pos` → `pos_x`, `pos_y`) and its `pos.x` body accesses are
		 * rewritten to the flattened names (flattened sys lowering). */
		as->stmts = syntax_lower_body(d, &as->stmt_count);
		int np = sv_count(d, SN_PARAM);
		int pcount = 0;
		for (int i = 0; i < np; i++) {
			char *pn = sv_dup(sv_child(sv_child_at(d, SN_PARAM, i), SN_PARAM_NAME));
			CstTupleGroup *g = tgroup_lookup(pn);
			pcount += g ? g->nsuf : 1;
			free(pn);
		}
		as->params = calloc(pcount ? pcount : 1, sizeof(HirParam *));
		as->param_count = 0;
		for (int i = 0; i < np; i++) {
			SyntaxView p = sv_child_at(d, SN_PARAM, i);
			char *pn = sv_dup(sv_child(p, SN_PARAM_NAME));
			CstTupleGroup *g = tgroup_lookup(pn);
			if (!g) {
				as->params[as->param_count++] = lower_param_cst(p);
				free(pn);
				continue;
			}
			for (int sx = 0; sx < as->stmt_count; sx++)
				tuple_rewrite_stmt(as->stmts[sx], pn);
			int is_own = sv_has_token(p, TOK_OWN);
			for (int j = 0; j < g->nsuf; j++) {
				HirParam *ap = hir_param_create(NULL, NULL);
				ap->name = malloc(strlen(pn) + 1 + strlen(g->suffix[j]) + 1);
				sprintf(ap->name, "%s_%s", pn, g->suffix[j]);
				HirType *mt = hir_type_create(HIR_TYPE_UNKNOWN);
				*mt = g->member;
				ap->type = mt;
				ap->is_own = is_own;
				as->params[as->param_count++] = ap;
			}
			free(pn);
		}
		ad->data.sys = as;
		return ad;
	}
	case SN_FUNC_DECL: {
		HirDecl *ad = hir_decl_create(HIR_DECL_FUNC);
		HirFuncDecl *af = calloc(1, sizeof(HirFuncDecl));
		af->name = sv_dup(sv_child(d, SN_FUNC_DEF_NAME));
		af->is_extern = 0; /* funcs are never foreign */
		int np = sv_count(d, SN_PARAM);
		af->params = calloc(np ? np : 1, sizeof(HirParam *));
		for (int i = 0; i < np; i++)
			af->params[i] = lower_param_cst(sv_child_at(d, SN_PARAM, i));
		af->param_count = np;
		/* return types: TYPE_REF nodes that are NOT inside params (params are wrapped in SN_PARAM) */
		int nt = sv_type_count(d);
		af->return_types = calloc(nt ? nt : 1, sizeof(HirType *));
		af->return_type_count = 0;
		for (int i = 0; i < nt; i++)
			af->return_types[af->return_type_count++] = lower_type_cst(sv_type_at(d, i));
		af->stmts = syntax_lower_body(d, &af->stmt_count);
		ad->data.func = af;
		return ad;
	}
	case SN_CONST_DECL: {
		/* Unified grammar: a binding `name :: <value form>` declares that kind, named by the LHS. */
		SyntaxView rhs = lower_rhs_form(d);
		if (sv_present(rhs)) {
			SyntaxNodeKind rk = sv_kind(rhs);
			if (rk == SN_PROC_EXPR || rk == SN_FUNC_EXPR || rk == SN_POLICY_EXPR || rk == SN_SYS_EXPR ||
			    rk == SN_ARCH_EXPR || rk == SN_GROUP_EXPR) {
				char *nm = txt_dup(lower_binding_name(d));
				switch (rk) {
				case SN_PROC_EXPR: {
					HirDecl *pd = lower_proc_from(rhs, nm);
					/* Propagate the `@drop` decorator (a direct `@ drop` token pair on the
					 * decl node) so the RAII pass can register this proc as a destructor. */
					if (pd && pd->kind == HIR_DECL_PROC && pd->data.proc && syntax_decl_has_drop_decorator(d))
						pd->data.proc->is_drop = 1;
					/* `@intrinsic`: calls to this decl lower to a built-in instruction (codegen
					 * checks the resolved decl's flag, not the symbol name — see codegen syscall). */
					if (pd && pd->kind == HIR_DECL_PROC && pd->data.proc && syntax_decl_has_intrinsic_decorator(d))
						pd->data.proc->is_intrinsic = 1;
					return pd;
				}
				case SN_FUNC_EXPR: {
					HirDecl *fd = lower_func_from(rhs, nm);
					return fd;
				}
				case SN_POLICY_EXPR: {
					/* A policy lowers to a func body, but it's a MACRO: codegen inlines its statements
					 * at each fallible op site (operands bound as mutable locals), so it is never emitted
					 * as its own LLVM function. Marked so codegen can find it and skip its emission. */
					HirDecl *pf = lower_func_from(rhs, nm);
					if (pf && pf->kind == HIR_DECL_FUNC && pf->data.func) {
						pf->data.func->is_policy = 1;
						pf->data.func->policy_category = syntax_decl_policy_category(d);
					}
					return pf;
				}
				case SN_SYS_EXPR:
					return lower_sys_from(rhs, nm);
				case SN_ARCH_EXPR:
					return lower_archetype_from(rhs, nm);
				case SN_GROUP_EXPR:
					return lower_func_group_from(rhs, nm);
				default:
					break;
				}
			}
			/* enum: compile-time only (variants resolve to int literals); emit no decl. */
			if (rk == SN_ENUM_EXPR)
				return NULL;
		}
		/* Callable alias `handler :: some_proc`: compile-time only (calls are rewritten to the
		 * target); emit no decl. */
		if (g_lower_sem) {
			char *bn = txt_dup(lower_binding_name(d));
			const char *tgt = bn ? semantic_resolve_callable_alias(g_lower_sem, bn) : NULL;
			free(bn);
			if (tgt)
				return NULL;
		}
		/* Tuple-group type definition `pos (x, y) :: T` mints nominal component types
		 * (pos_x, pos_y); it's compile-time only and erased before codegen. */
		if (sv_has_token(d, TOK_LPAREN))
			return NULL;
		HirDecl *ad = hir_decl_create(HIR_DECL_CONST);
		HirConstDecl *ac = calloc(1, sizeof(HirConstDecl));
		ac->name = txt_dup(sv_token(d, TOK_IDENT));
		ac->value = lower_const_or_expr(sv_node_at_expr(d, 0));
		ad->data.constant = ac;
		return ad;
	}
	case SN_STATIC_DECL: {
		HirDecl *ad = hir_decl_create(HIR_DECL_STATIC);
		HirStaticDecl *sd = calloc(1, sizeof(HirStaticDecl));
		if (sv_has_token(d, TOK_LBRACKET)) {
			/* Pool allocation `Name[C](N){V}` — archetype name is the (possibly qualified) head:
			 * the `.`-joined IDENT tokens before `[`. A bare `Particle[N]` yields "Particle"; a
			 * qualified `lib.Particle[N]` yields "lib.Particle" (the imported shape's canonical name). */
			sd->kind = HIR_STATIC_ARCHETYPE;
			/* Prefix pool `[C]Name(N){V}`: the archetype name is the (possibly `.`-qualified) IDENT
			 * run that sits at top level AFTER the capacity `[…]` and before any `(`/`{`. It is
			 * collected in the phase walk below (the PH_NONE IDENT case), so a bare `[N]Particle`
			 * yields "Particle" and `[N]lib.Particle` yields "lib.Particle". */
			char namebuf[256];
			int nl = 0;
			/* `[8]Foo ?handler`: the pool's default insert overflow handler. */
			{
				SyntaxView pol = sv_child(d, SN_POLICY_REF);
				if (sv_present(pol))
					sd->archetype.overflow_policy = txt_dup(sv_token(pol, TOK_IDENT));
			}
			/* `[capacity] (init_length) { field: value, ... }`. Capacity (the `[…]` expr) →
			 * field_values[0] (field_names[0]=NULL); the optional `(…)` expr → init_length, the
			 * known row count (drives bounds-check elision); each `field: value` in `{}` appends
			 * a named initializer. Expr nodes appear in all three bracket kinds, so track which
			 * one we're inside. */
			int cap_alloc = d.node->child_count + 1;
			sd->archetype.field_names = calloc(cap_alloc, sizeof(char *));
			sd->archetype.field_values = calloc(cap_alloc, sizeof(HirExpr *));
			sd->archetype.field_count = 0;
			enum { PH_NONE, PH_CAP, PH_LEN, PH_FIELDS } phase = PH_NONE;
			const char *pend = NULL;
			int pend_len = 0;
			for (int i = 0; i < d.node->child_count; i++) {
				SyntaxElem *ch = &d.node->children[i];
				if (ch->tag == SE_TOKEN) {
					switch (ch->as.token.kind) {
					case TOK_LBRACKET:
						phase = PH_CAP;
						break;
					case TOK_LPAREN:
						phase = PH_LEN;
						break;
					case TOK_LBRACE:
						phase = PH_FIELDS;
						break;
					case TOK_RBRACKET:
					case TOK_RPAREN:
					case TOK_RBRACE:
						phase = PH_NONE;
						break;
					case TOK_IDENT:
						if (phase == PH_FIELDS) {
							pend = d.src + ch->as.token.offset;
							pend_len = (int)ch->as.token.length;
						} else if (phase == PH_NONE) {
							/* archetype name segment — top level, after the capacity `[]` */
							if (nl > 0 && nl < (int)sizeof(namebuf) - 1)
								namebuf[nl++] = '.';
							int seg = (int)ch->as.token.length;
							for (int kk = 0; kk < seg && nl < (int)sizeof(namebuf) - 1; kk++)
								namebuf[nl++] = d.src[ch->as.token.offset + kk];
						}
						break;
					default:
						break;
					}
					continue;
				}
				SyntaxNodeKind k = ch->as.node->kind;
				if (k < SN_LITERAL_EXPR || k > SN_PAREN_EXPR)
					continue;
				SyntaxView ev = {ch->as.node, d.src};
				if (phase == PH_CAP) {
					sd->archetype.field_values[0] = lower_const_or_expr(ev);
					sd->archetype.field_count = 1;
				} else if (phase == PH_LEN) {
					sd->archetype.init_length = lower_const_or_expr(ev);
				} else if (phase == PH_FIELDS && pend) {
					int fc = sd->archetype.field_count;
					sd->archetype.field_names[fc] = txt_dup((SynText){pend, pend_len});
					sd->archetype.field_values[fc] = lower_const_or_expr(ev);
					sd->archetype.field_count++;
					pend = NULL;
				}
			}
			namebuf[nl] = '\0';
			sd->archetype.archetype_name = dupz(namebuf);
		} else {
			/* Storage form of the unified binding: `name : T` / `name : T = v` / `name := v`. A
			 * sized-array T (it has an element type) is a buffer; any other (or absent) T is a
			 * scalar. The `= v` value is the initializer; its absence is the implicit `= 0`. */
			char *nm = txt_dup(sv_token(d, TOK_IDENT));
			SyntaxView arr_ty = sv_type_at(d, 0);
			SyntaxView initv = sv_node_at_expr(d, 0);
			HirType *full = sv_present(arr_ty) ? lower_type_cst(arr_ty) : NULL;
			if (full && full->elem) {
				/* buffer: codegen wants element + size, not the array wrapper */
				sd->kind = HIR_STATIC_ARRAY;
				sd->array.name = nm;
				sd->array.element_type = full->elem;
				for (int i = 0; i < arr_ty.node->child_count; i++)
					if (arr_ty.node->children[i].tag == SE_TOKEN &&
					    arr_ty.node->children[i].as.token.kind == TOK_NUMBER) {
						char buf[32];
						int l = (int)arr_ty.node->children[i].as.token.length;
						if (l > 31)
							l = 31;
						memcpy(buf, arr_ty.src + arr_ty.node->children[i].as.token.offset, l);
						buf[l] = '\0';
						sd->array.size = atoi(buf);
						break;
					}
				sd->array.init = sv_present(initv) ? lower_expr_cst(initv) : NULL;
			} else {
				/* scalar; inferred form `name := v` carries no type node — infer int/float from the
				 * literal initializer's lexeme. A constant func/expr initializer folds to a literal. */
				HirExpr *ie = sv_present(initv) ? lower_const_or_expr(initv) : NULL;
				HirType *sty = full;
				if (!sty) {
					sty = hir_type_create(HIR_TYPE_UNKNOWN);
					const char *tn = (ie && ie->kind == HIR_EXPR_LITERAL && ie->data.literal.lexeme &&
					                  strpbrk(ie->data.literal.lexeme, ".eE"))
					                     ? "float"
					                     : "int";
					*sty = map_type_str(tn);
				}
				sd->kind = HIR_STATIC_SCALAR;
				sd->scalar.name = nm;
				sd->scalar.type = sty;
				sd->scalar.init = ie;
			}
		}
		ad->data.static_decl = sd;
		return ad;
	}
	case SN_FUNC_GROUP_DECL: {
		/* `func g = { a, b };` — name in FUNC_DEF_NAME, members are the bare IDENTs. */
		HirDecl *ad = hir_decl_create(HIR_DECL_FUNC_GROUP);
		HirFuncGroupDecl *fg = calloc(1, sizeof(HirFuncGroupDecl));
		fg->name = sv_dup(sv_child(d, SN_FUNC_DEF_NAME));
		int nmem = 0;
		for (int i = 0; i < d.node->child_count; i++)
			if (d.node->children[i].tag == SE_TOKEN && d.node->children[i].as.token.kind == TOK_IDENT)
				nmem++;
		fg->member_names = calloc(nmem ? nmem : 1, sizeof(char *));
		fg->member_count = 0;
		for (int i = 0; i < d.node->child_count; i++)
			if (d.node->children[i].tag == SE_TOKEN && d.node->children[i].as.token.kind == TOK_IDENT) {
				SynText t = {d.src + d.node->children[i].as.token.offset, d.node->children[i].as.token.length};
				fg->member_names[fg->member_count++] = txt_dup(t);
			}
		ad->data.func_group = fg;
		return ad;
	}
	default:
		/* tuple flattening + field-init blocks not yet ported. */
		return NULL;
	}
}

/* ---- module inlining: syntax-tree-path equivalent of main.c's resolve_uses ----
 * `use foo;` inlines foo.arche's decls at the use site, auto-prefixing every
 * name foo declares (and foo-internal references to them) with `foo_`. main.c
 * registers each used module's syntax tree here before calling lower_to_hir. */
typedef struct {
	char *name;
	const SyntaxNode *root;
	const char *src;
	char *filename; /* source path; a `*.ds.arche` file is a device datasheet (decls stay global) */
} LowerModule;
static LowerModule g_modules[64];
static int g_module_count = 0;

static char *dupz(const char *s) {
	char *r = malloc(strlen(s) + 1);
	strcpy(r, s);
	return r;
}

/* A device datasheet file: its decls are shared global vocabulary, registered UNPREFIXED so a
 * driver references them by bare name and a device's systems bind to the driver's shape. */
static int is_datasheet_file(const char *fn) {
	if (!fn)
		return 0;
	size_t L = strlen(fn);
	return L >= 9 && strcmp(fn + L - 9, ".ds.arche") == 0;
}

void lower_add_module(const char *name, const SyntaxNode *root, const char *src, const char *filename) {
	if (g_module_count >= 64 || !name || !root)
		return;
	g_modules[g_module_count].name = dupz(name);
	g_modules[g_module_count].root = root;
	g_modules[g_module_count].src = src;
	g_modules[g_module_count].filename = filename ? dupz(filename) : NULL;
	g_module_count++;
}

void lower_reset_modules(void) {
	for (int i = 0; i < g_module_count; i++) {
		free(g_modules[i].name);
		free(g_modules[i].filename);
	}
	g_module_count = 0;
}

/* `@implements` bindings: rename a device requirement `old` → the driver's name `new_` everywhere.
 * Applied as a post-inlining pass that reuses the rename traversal (via prefixed_dup, set-independent
 * so it fires even though datasheet decls aren't in any module rename set). Empty during inlining. */
typedef struct {
	char *old;
	char *new_;
} ImplBind;
static ImplBind g_impl[64];
static int g_impl_count = 0;

/* Apply an active `@implements` binding to an owned name slot (e.g. a sys param or archetype field
 * name). The normal rename traversal skips these — but column binding is by name, so a device
 * requirement used as a column must be substituted to the driver's name here. */
static void subst_name(char **slot) {
	if (!slot || !*slot)
		return;
	for (int i = 0; i < g_impl_count; i++)
		if (strcmp(*slot, g_impl[i].old) == 0) {
			free(*slot);
			*slot = dupz(g_impl[i].new_);
			return;
		}
}

/* If `name` is in set, return a freshly-allocated qualified identity `prefix.name`; else NULL.
 * (Dotted to match the semantic resolver; `.` is a legal LLVM global-identifier char.) An active
 * `@implements` binding `old → new_` matches first and substitutes regardless of `set`. */
static char *prefixed_dup(const char *name, const char *prefix, char **set, int count) {
	if (!name)
		return NULL;
	for (int i = 0; i < g_impl_count; i++)
		if (strcmp(name, g_impl[i].old) == 0)
			return dupz(g_impl[i].new_);
	for (int i = 0; i < count; i++)
		if (strcmp(name, set[i]) == 0) {
			char *r = malloc(strlen(prefix) + 1 + strlen(name) + 1);
			sprintf(r, "%s.%s", prefix, name);
			return r;
		}
	return NULL;
}
static void rn_owned(char **slot, const char *prefix, char **set, int count) {
	char *p = prefixed_dup(*slot, prefix, set, count);
	if (p) {
		free(*slot);
		*slot = p;
	}
}
/* HirType.name points into the syntax tree (not owned) — swap the pointer, never free it. */
static void rn_const(const char **slot, const char *prefix, char **set, int count) {
	char *p = prefixed_dup(*slot, prefix, set, count);
	if (p)
		*slot = p;
}

static void hir_rn_type(HirType *t, const char *prefix, char **set, int count) {
	if (!t)
		return;
	if (t->tag == HIR_TYPE_NAMED) {
		rn_const(&t->name, prefix, set, count);
		/* A module-local nominal type (e.g. `socket` inside `net`) was lowered as NAMED before this
		 * rename ran, so its opaque-ness was missed — the alias is registered only under the prefixed
		 * key (`net_socket`). Now that the name is prefixed, re-resolve it: if it aliases `opaque`,
		 * retag so codegen erases it to an i64 cell instead of emitting a `%struct.<name>`. Keep
		 * `t->name` (the nominal alias) so the RAII `@drop` pass can still match it. */
		if (g_lower_sem && t->name) {
			const char *b = semantic_resolve_type_alias(g_lower_sem, t->name);
			if (b && strcmp(b, "opaque") == 0)
				t->tag = HIR_TYPE_OPAQUE;
		}
	}
	hir_rn_type(t->elem, prefix, set, count);
	for (int i = 0; i < t->field_count; i++)
		hir_rn_type(t->fields[i].type, prefix, set, count);
}
static void hir_rn_expr(HirExpr *e, const char *prefix, char **set, int count) {
	if (!e)
		return;
	switch (e->kind) {
	case HIR_EXPR_NAME:
		rn_owned(&e->data.name.name, prefix, set, count);
		break;
	case HIR_EXPR_FIELD:
		hir_rn_expr(e->data.field.base, prefix, set, count);
		break;
	case HIR_EXPR_INDEX:
		hir_rn_expr(e->data.index.base, prefix, set, count);
		for (int i = 0; i < e->data.index.index_count; i++)
			hir_rn_expr(e->data.index.indices[i], prefix, set, count);
		break;
	case HIR_EXPR_BINARY:
		hir_rn_expr(e->data.binary.left, prefix, set, count);
		hir_rn_expr(e->data.binary.right, prefix, set, count);
		break;
	case HIR_EXPR_UNARY:
		hir_rn_expr(e->data.unary.operand, prefix, set, count);
		break;
	case HIR_EXPR_CALL:
		hir_rn_expr(e->data.call.callee, prefix, set, count);
		for (int i = 0; i < e->data.call.arg_count; i++)
			hir_rn_expr(e->data.call.args[i], prefix, set, count);
		break;
	case HIR_EXPR_ALLOC:
		rn_owned(&e->data.alloc.archetype_name, prefix, set, count);
		for (int i = 0; i < e->data.alloc.field_count; i++)
			hir_rn_expr(e->data.alloc.field_values[i], prefix, set, count);
		hir_rn_expr(e->data.alloc.init_length, prefix, set, count);
		break;
	case HIR_EXPR_ARRAY_LITERAL:
		for (int i = 0; i < e->data.array_literal.element_count; i++)
			hir_rn_expr(e->data.array_literal.elements[i], prefix, set, count);
		break;
	default:
		break;
	}
}
static void hir_rn_stmt(HirStmt *s, const char *prefix, char **set, int count) {
	if (!s)
		return;
	switch (s->kind) {
	case HIR_STMT_BIND:
		hir_rn_type(s->data.bind_stmt.type, prefix, set, count);
		hir_rn_expr(s->data.bind_stmt.value, prefix, set, count);
		break;
	case HIR_STMT_ASSIGN:
		hir_rn_expr(s->data.assign_stmt.target, prefix, set, count);
		hir_rn_expr(s->data.assign_stmt.value, prefix, set, count);
		break;
	case HIR_STMT_FOR:
		hir_rn_expr(s->data.for_stmt.iterable, prefix, set, count);
		hir_rn_stmt(s->data.for_stmt.init, prefix, set, count);
		hir_rn_expr(s->data.for_stmt.cond, prefix, set, count);
		hir_rn_stmt(s->data.for_stmt.incr, prefix, set, count);
		for (int i = 0; i < s->data.for_stmt.body_count; i++)
			hir_rn_stmt(s->data.for_stmt.body[i], prefix, set, count);
		break;
	case HIR_STMT_IF:
		hir_rn_expr(s->data.if_stmt.cond, prefix, set, count);
		for (int i = 0; i < s->data.if_stmt.then_count; i++)
			hir_rn_stmt(s->data.if_stmt.then_body[i], prefix, set, count);
		for (int i = 0; i < s->data.if_stmt.else_count; i++)
			hir_rn_stmt(s->data.if_stmt.else_body[i], prefix, set, count);
		break;
	case HIR_STMT_EXPR:
		hir_rn_expr(s->data.expr_stmt.expr, prefix, set, count);
		break;
	case HIR_STMT_RETURN:
		for (int i = 0; i < s->data.return_stmt.count; i++)
			hir_rn_expr(s->data.return_stmt.values[i], prefix, set, count);
		break;
	case HIR_STMT_MULTI_BIND:
		for (int i = 0; i < s->data.multi_bind.target_count; i++)
			hir_rn_type(s->data.multi_bind.targets[i].type, prefix, set, count);
		hir_rn_expr(s->data.multi_bind.value, prefix, set, count);
		break;
	case HIR_STMT_EACH_FIELD:
		hir_rn_type(s->data.each_field.filter_type, prefix, set, count);
		for (int i = 0; i < s->data.each_field.body_count; i++)
			hir_rn_stmt(s->data.each_field.body[i], prefix, set, count);
		break;
	case HIR_STMT_BLOCK: /* `match` / surface `{ }` desugar — descend so intra-module names get renamed */
		for (int i = 0; i < s->data.block.count; i++)
			hir_rn_stmt(s->data.block.stmts[i], prefix, set, count);
		break;
	default: /* BREAK, RUN — no embedded names (run is world/system, not module-local) */
		break;
	}
}
static void hir_rn_decl(HirDecl *d, const char *prefix, char **set, int count) {
	switch (d->kind) {
	case HIR_DECL_ARCHETYPE:
		rn_owned(&d->data.archetype->name, prefix, set, count);
		for (int i = 0; i < d->data.archetype->field_count; i++)
			hir_rn_type(d->data.archetype->fields[i]->type, prefix, set, count);
		break;
	case HIR_DECL_PROC:
		rn_owned(&d->data.proc->name, prefix, set, count);
		for (int i = 0; i < d->data.proc->param_count; i++)
			hir_rn_type(d->data.proc->params[i]->type, prefix, set, count);
		for (int i = 0; i < d->data.proc->out_param_count; i++)
			hir_rn_type(d->data.proc->out_params[i]->type, prefix, set, count);
		for (int i = 0; i < d->data.proc->stmt_count; i++)
			hir_rn_stmt(d->data.proc->stmts[i], prefix, set, count);
		break;
	case HIR_DECL_SYS:
		rn_owned(&d->data.sys->name, prefix, set, count);
		for (int i = 0; i < d->data.sys->param_count; i++)
			hir_rn_type(d->data.sys->params[i]->type, prefix, set, count);
		for (int i = 0; i < d->data.sys->stmt_count; i++)
			hir_rn_stmt(d->data.sys->stmts[i], prefix, set, count);
		break;
	case HIR_DECL_FUNC:
		rn_owned(&d->data.func->name, prefix, set, count);
		for (int i = 0; i < d->data.func->return_type_count; i++)
			hir_rn_type(d->data.func->return_types[i], prefix, set, count);
		for (int i = 0; i < d->data.func->param_count; i++)
			hir_rn_type(d->data.func->params[i]->type, prefix, set, count);
		for (int i = 0; i < d->data.func->stmt_count; i++)
			hir_rn_stmt(d->data.func->stmts[i], prefix, set, count);
		break;
	case HIR_DECL_FUNC_GROUP:
		rn_owned(&d->data.func_group->name, prefix, set, count);
		for (int i = 0; i < d->data.func_group->member_count; i++)
			rn_owned(&d->data.func_group->member_names[i], prefix, set, count);
		break;
	case HIR_DECL_STATIC:
		if (d->data.static_decl->kind == HIR_STATIC_ARRAY) {
			rn_owned(&d->data.static_decl->array.name, prefix, set, count);
			hir_rn_type(d->data.static_decl->array.element_type, prefix, set, count);
		} else if (d->data.static_decl->kind == HIR_STATIC_SCALAR) {
			rn_owned(&d->data.static_decl->scalar.name, prefix, set, count);
			hir_rn_type(d->data.static_decl->scalar.type, prefix, set, count);
			hir_rn_expr(d->data.static_decl->scalar.init, prefix, set, count);
		} else {
			rn_owned(&d->data.static_decl->archetype.archetype_name, prefix, set, count);
			for (int i = 0; i < d->data.static_decl->archetype.field_count; i++)
				hir_rn_expr(d->data.static_decl->archetype.field_values[i], prefix, set, count);
			hir_rn_expr(d->data.static_decl->archetype.init_length, prefix, set, count);
		}
		break;
	case HIR_DECL_CONST:
		rn_owned(&d->data.constant->name, prefix, set, count);
		hir_rn_expr(d->data.constant->value, prefix, set, count);
		break;
	case HIR_DECL_WORLD:
		rn_owned(&d->data.world->name, prefix, set, count);
		break;
	case HIR_DECL_DEFAULT:
		/* A program default directive references a policy by name globally; no module-local rename. */
		break;
	}
}

/* The top-level name an HirDecl defines (NULL if none / world). */
static const char *hir_decl_name(HirDecl *d) {
	switch (d->kind) {
	case HIR_DECL_ARCHETYPE:
		return d->data.archetype->name;
	case HIR_DECL_PROC:
		return d->data.proc->name;
	case HIR_DECL_SYS:
		return d->data.sys->name;
	case HIR_DECL_FUNC:
		return d->data.func->name;
	case HIR_DECL_FUNC_GROUP:
		return d->data.func_group->name;
	case HIR_DECL_STATIC:
		if (d->data.static_decl->kind == HIR_STATIC_ARRAY)
			return d->data.static_decl->array.name;
		if (d->data.static_decl->kind == HIR_STATIC_SCALAR)
			return d->data.static_decl->scalar.name;
		return d->data.static_decl->archetype.archetype_name;
	case HIR_DECL_CONST:
		return d->data.constant->name;
	case HIR_DECL_WORLD:
		return d->data.world->name;
	case HIR_DECL_DEFAULT:
		return NULL; /* a directive, not a named decl */
	}
	return NULL;
}

/* ---- Qualified module access: rewrite `mod.name` → the mangled `mod_name` symbol ----
 * `#import io` inlines io's decls renamed to `io_<name>`. A use site writes `io.open`, which
 * parses as a field access NAME(io).open. This pass turns that into NAME(io_open) when `io` is an
 * inlined module and `open` is one of its exports — so qualified access resolves to the real
 * symbol. (The bare `io_open` form keeps working; this just adds the `io.open` spelling.) Field
 * access on a value (`h.mass`) or archetype column (`Particle.pos`) is left untouched, since the
 * base name isn't a module. */
typedef struct {
	char **prefix;   /* module names (the `io` in `io.open`) */
	char ***exports; /* each module's exported bare names */
	int *export_n;   /* count per module */
	int n;           /* number of inlined modules */
} QualCtx;

static int qual_lookup(const QualCtx *q, const char *base, const char *field, char *out, size_t out_sz) {
	for (int m = 0; m < q->n; m++) {
		if (strcmp(base, q->prefix[m]) != 0)
			continue;
		for (int s = 0; s < q->export_n[m]; s++) {
			/* Each export entry is "<visible>=<target-symbol>": match the visible (qualified)
			 * name, resolve to its target. A pure-Arche export targets the prefixed
			 * `<mod>_<name>`; a foreign export targets its real declared link name — so
			 * `fmt.printf` → libc `printf`, `net.listen` → runtime `net_listen`. */
			const char *e = q->exports[m][s];
			const char *eq = strchr(e, '=');
			if (!eq)
				continue;
			size_t vlen = (size_t)(eq - e);
			if (strlen(field) == vlen && strncmp(field, e, vlen) == 0) {
				snprintf(out, out_sz, "%s", eq + 1);
				return 1;
			}
		}
	}
	return 0;
}

static void hir_q_expr(HirExpr *e, const QualCtx *q) {
	if (!e)
		return;
	if (e->kind == HIR_EXPR_FIELD && e->data.field.base && e->data.field.base->kind == HIR_EXPR_NAME &&
	    e->data.field.field_name) {
		char mangled[256];
		if (qual_lookup(q, e->data.field.base->data.name.name, e->data.field.field_name, mangled, sizeof(mangled))) {
			/* Transmute FIELD → NAME(mod_field). Old base/field_name are left to the arena
			 * (small, bounded; the union write below clobbers the field pointers anyway). */
			e->kind = HIR_EXPR_NAME;
			e->data.name.name = dupz(mangled);
			return;
		}
	}
	switch (e->kind) {
	case HIR_EXPR_FIELD:
		hir_q_expr(e->data.field.base, q);
		break;
	case HIR_EXPR_INDEX:
		hir_q_expr(e->data.index.base, q);
		for (int i = 0; i < e->data.index.index_count; i++)
			hir_q_expr(e->data.index.indices[i], q);
		break;
	case HIR_EXPR_BINARY:
		hir_q_expr(e->data.binary.left, q);
		hir_q_expr(e->data.binary.right, q);
		break;
	case HIR_EXPR_UNARY:
		hir_q_expr(e->data.unary.operand, q);
		break;
	case HIR_EXPR_CALL:
		hir_q_expr(e->data.call.callee, q);
		for (int i = 0; i < e->data.call.arg_count; i++)
			hir_q_expr(e->data.call.args[i], q);
		break;
	case HIR_EXPR_ALLOC:
		for (int i = 0; i < e->data.alloc.field_count; i++)
			hir_q_expr(e->data.alloc.field_values[i], q);
		hir_q_expr(e->data.alloc.init_length, q);
		break;
	case HIR_EXPR_ARRAY_LITERAL:
		for (int i = 0; i < e->data.array_literal.element_count; i++)
			hir_q_expr(e->data.array_literal.elements[i], q);
		break;
	default:
		break;
	}
}

static void hir_q_stmt(HirStmt *s, const QualCtx *q) {
	if (!s)
		return;
	switch (s->kind) {
	case HIR_STMT_BIND:
		hir_q_expr(s->data.bind_stmt.value, q);
		break;
	case HIR_STMT_ASSIGN:
		hir_q_expr(s->data.assign_stmt.target, q);
		hir_q_expr(s->data.assign_stmt.value, q);
		break;
	case HIR_STMT_FOR:
		hir_q_expr(s->data.for_stmt.iterable, q);
		hir_q_stmt(s->data.for_stmt.init, q);
		hir_q_expr(s->data.for_stmt.cond, q);
		hir_q_stmt(s->data.for_stmt.incr, q);
		for (int i = 0; i < s->data.for_stmt.body_count; i++)
			hir_q_stmt(s->data.for_stmt.body[i], q);
		break;
	case HIR_STMT_IF:
		hir_q_expr(s->data.if_stmt.cond, q);
		for (int i = 0; i < s->data.if_stmt.then_count; i++)
			hir_q_stmt(s->data.if_stmt.then_body[i], q);
		for (int i = 0; i < s->data.if_stmt.else_count; i++)
			hir_q_stmt(s->data.if_stmt.else_body[i], q);
		break;
	case HIR_STMT_EXPR:
		hir_q_expr(s->data.expr_stmt.expr, q);
		break;
	case HIR_STMT_RETURN:
		for (int i = 0; i < s->data.return_stmt.count; i++)
			hir_q_expr(s->data.return_stmt.values[i], q);
		break;
	case HIR_STMT_MULTI_BIND:
		hir_q_expr(s->data.multi_bind.value, q);
		break;
	case HIR_STMT_EACH_FIELD:
		for (int i = 0; i < s->data.each_field.body_count; i++)
			hir_q_stmt(s->data.each_field.body[i], q);
		break;
	case HIR_STMT_BLOCK:
		/* `match` desugars to a BLOCK { __match := scrut; if-chain }. Without descending into it, a
		 * qualified call (`fmt.print(...)`) in a match arm keeps its FIELD callee unresolved → codegen
		 * sees a NULL func_name. */
		for (int i = 0; i < s->data.block.count; i++)
			hir_q_stmt(s->data.block.stmts[i], q);
		break;
	default:
		break;
	}
}

static void hir_q_decl(HirDecl *d, const QualCtx *q) {
	if (!d)
		return;
	switch (d->kind) {
	case HIR_DECL_PROC:
		for (int i = 0; i < d->data.proc->stmt_count; i++)
			hir_q_stmt(d->data.proc->stmts[i], q);
		break;
	case HIR_DECL_SYS:
		for (int i = 0; i < d->data.sys->stmt_count; i++)
			hir_q_stmt(d->data.sys->stmts[i], q);
		break;
	case HIR_DECL_FUNC:
		for (int i = 0; i < d->data.func->stmt_count; i++)
			hir_q_stmt(d->data.func->stmts[i], q);
		break;
	case HIR_DECL_STATIC:
		if (d->data.static_decl->kind == HIR_STATIC_ARCHETYPE) {
			for (int i = 0; i < d->data.static_decl->archetype.field_count; i++)
				hir_q_expr(d->data.static_decl->archetype.field_values[i], q);
			hir_q_expr(d->data.static_decl->archetype.init_length, q);
		} else if (d->data.static_decl->kind == HIR_STATIC_SCALAR) {
			hir_q_expr(d->data.static_decl->scalar.init, q);
		}
		break;
	case HIR_DECL_CONST:
		hir_q_expr(d->data.constant->value, q);
		break;
	default:
		break;
	}
}

/* Lower one module decl from `node`, append to ast, and record its name in `full` (intra-module
 * resolution) and — when `exported` — `expset` (externally visible via qualified `mod.name`).
 * Non-externs are prefixed to `<mod>_<name>` and recorded under their source name. Externs keep
 * their unprefixed decl (the C ABI symbol) and are NOT added to `full`; they go into `expset` under
 * the prefix-stripped visible name, so `mod.<visible>` reconstructs `<mod>_<visible>` == the C
 * symbol (e.g. `net.listen` → `net_listen`). Shared by the module loop and recursion into
 * `#foreign { }` / `#module { }` block regions. */
static void hir_add_module_decl(const SyntaxNode *node, const char *msrc, const char *mod_name, HirProgram *ast,
                                char ***full, int *fulln, int *fullcap, char ***expset, int *expn, int *expcap,
                                int exported, int is_datasheet, int module_is_device, int file_local, char ***fileset,
                                int *filesetn, int *filesetcap, int unit) {
	HirDecl *md = lower_decl_cst((SyntaxView){node, msrc});
	if (!md)
		return;
	md->unit = unit; /* per-unit codegen grouping (mirror of DeclSummary.unit) */
	/* A pool decl in a datasheet (`.ds.arche`) is a storage REQUIREMENT (min rows), not an allocation. */
	if (is_datasheet && md->kind == HIR_DECL_STATIC && md->data.static_decl &&
	    md->data.static_decl->kind == HIR_STATIC_ARCHETYPE)
		md->data.static_decl->is_requirement = 1;
	/* A shape may be DEFINED in more than one place — a device impl that uses it AND the driver, say —
	 * and every definition is the same canonical shape. Duplicates are NOT stripped here: codegen folds
	 * them onto one canonical decl (canonical_archetype_decl), so one shape still emits one struct + one
	 * set of helpers. (A datasheet never reaches this with an archetype — defining a shape in a
	 * `.ds.arche` is a semantic error.) */
	ast->decls[ast->decl_count++] = md;
	int is_ext = (md->kind == HIR_DECL_PROC && md->data.proc->is_extern) ||
	             (md->kind == HIR_DECL_FUNC && md->data.func->is_extern);
	const char *nm = hir_decl_name(md);
	if (!nm)
		return;
	/* A decl is registered FLAT (unprefixed, bare export) when foreign (C ABI symbol), a datasheet decl
	 * (shared global vocabulary), an EXPORTED ARCHETYPE (a public shape is global vocabulary — its name
	 * is bare, never `<device>.Name`; a `#module` shape stays unit-private), OR from a PLAIN module (no
	 * `.ds.arche`) — a plain/path module merges flat into the importer (Jai `#load`), so `helper()` not
	 * `mod.helper()`. Only a DEVICE's pure-Arche behavior decls are renamed to `<device>.<name>`. */
	int flat = is_ext || is_datasheet || !module_is_device || (md->kind == HIR_DECL_ARCHETYPE && exported);
	/* A `#file` decl is file-local: excluded from the cross-file `full` set + never exported; collected
	 * into the per-file `fileset` and renamed to a file-unique identity by hir_inline_module (mirror of
	 * semantic). This also prevents two sibling files' same-named `#file` decls colliding at codegen. */
	if (file_local) {
		if (*filesetn == *filesetcap) {
			*filesetcap = *filesetcap ? *filesetcap * 2 : 8;
			*fileset = realloc(*fileset, (size_t)*filesetcap * sizeof(char *));
		}
		(*fileset)[(*filesetn)++] = dupz(nm);
		return;
	}
	if (!flat) {
		if (*fulln == *fullcap) {
			*fullcap = *fullcap ? *fullcap * 2 : 8;
			*full = realloc(*full, (size_t)*fullcap * sizeof(char *));
		}
		(*full)[(*fulln)++] = dupz(nm);
	}
	if (exported) {
		if (*expn == *expcap) {
			*expcap = *expcap ? *expcap * 2 : 8;
			*expset = realloc(*expset, (size_t)*expcap * sizeof(char *));
		}
		/* "<member>=<identity>": flat → the bare name; a device's pure-Arche decl → `<device>.<name>`. */
		char entry[512];
		if (flat)
			snprintf(entry, sizeof(entry), "%s=%s", nm, nm);
		else
			snprintf(entry, sizeof(entry), "%s=%s.%s", nm, mod_name, nm);
		(*expset)[(*expn)++] = dupz(entry);
	}
}

static int hir_is_collectible_decl(SyntaxNodeKind k) {
	return k >= SN_WORLD_DECL && k <= SN_USE_DECL && k != SN_USE_DECL;
}

/* The module name an `#import` element resolves to: an IDENT is the name verbatim (device by name);
 * a STRING is a path (module by path) whose name is the basename minus a trailing `.arche`. Returns a
 * malloc'd name (caller frees), or NULL for any other token. Mirror of compile.c's path handling. */
static char *import_token_module_name(const char *src, const SyntaxElem *tok) {
	if (tok->tag != SE_TOKEN)
		return NULL;
	TokenKind k = tok->as.token.kind;
	if (k != TOK_IDENT && k != TOK_STRING)
		return NULL;
	size_t off = tok->as.token.offset, len = tok->as.token.length;
	if (k == TOK_STRING && len >= 2) { /* strip the quotes */
		off += 1;
		len -= 2;
	}
	char *s = txt_dup((SynText){src + off, len});
	if (k == TOK_STRING) {
		char *slash = strrchr(s, '/');
		char *base = slash ? slash + 1 : s; /* basename */
		size_t bl = strlen(base);
		if (bl > 6 && strcmp(base + bl - 6, ".arche") == 0)
			base[bl - 6] = '\0';
		if (base != s)
			memmove(s, base, strlen(base) + 1);
	}
	return s;
}

/* Inline module `mod_name` into `ast` (prefixed so intra-module refs resolve), record its exports
 * for the qualify pass, then RECURSIVELY inline its own `#import`s — mirror of sem_inline_module so
 * a module may use qualified access to a transitive import (`csv` → `parse.atof`). Dedup = cycle-safe. */
static void hir_inline_module(const char *mod_name, HirProgram *ast, char **mod_prefix, char ***mod_exports,
                              int *mod_export_n, int *inlined) {
	for (int a = 0; a < *inlined; a++)
		if (strcmp(mod_prefix[a], mod_name) == 0)
			return;
	int uid = *inlined + 1; /* this module's unit id (it is registered at mod_prefix[*inlined] below) */
	int first = ast->decl_count;
	int found = 0;
	char **full = NULL;
	int fulln = 0, fullcap = 0;
	char **expset = NULL;
	int expn = 0, expcap = 0;
	/* A module is a DEVICE if ANY of its files is a `.ds.arche` datasheet. A device's pure-Arche impl
	 * decls are namespaced (`device.name`); a plain module's decls merge flat (mirror of semantic). */
	int module_is_device = 0;
	for (int m = 0; m < g_module_count; m++)
		if (strcmp(g_modules[m].name, mod_name) == 0 && is_datasheet_file(g_modules[m].filename))
			module_is_device = 1;
	for (int m = 0; m < g_module_count; m++) {
		if (strcmp(g_modules[m].name, mod_name) != 0)
			continue;
		found = 1;
		const SyntaxNode *mr = g_modules[m].root;
		const char *msrc = g_modules[m].src;
		int ds = is_datasheet_file(g_modules[m].filename); /* `.ds.arche` → decls stay global */
		int exported = 1;
		int file_local = 0;               /* sticky once a `#file` banner is seen */
		int file_first = ast->decl_count; /* this file's decl range, for the #file rename */
		char **fileset = NULL;            /* this file's `#file` decl names */
		int filesetn = 0, filesetcap = 0;
		for (int j = 0; j < mr->child_count; j++) {
			if (mr->children[j].tag != SE_NODE)
				continue;
			const SyntaxNode *cn = mr->children[j].as.node;
			SyntaxNodeKind mk = cn->kind;
			if (mk == SN_REGION) {
				SyntaxView rv = {cn, msrc};
				int is_block = sv_has_token(rv, TOK_LBRACE);
				int is_foreign = sv_has_token(rv, TOK_HASH_FOREIGN);
				int is_file = sv_has_token(rv, TOK_HASH_FILE);
				if (is_block) {
					int child_exp = is_foreign ? exported : 0;
					int child_fl = is_file ? 1 : file_local;
					for (int c = 0; c < cn->child_count; c++) {
						if (cn->children[c].tag != SE_NODE)
							continue;
						if (!hir_is_collectible_decl(cn->children[c].as.node->kind))
							continue;
						hir_add_module_decl(cn->children[c].as.node, msrc, mod_name, ast, &full, &fulln, &fullcap,
						                    &expset, &expn, &expcap, child_exp, ds, module_is_device, child_fl,
						                    &fileset, &filesetn, &filesetcap, uid);
					}
				} else if (!is_foreign) {
					exported = 0;
					if (is_file)
						file_local = 1; /* `#file` banner → rest of this file is file-local */
				}
				continue;
			}
			if (!hir_is_collectible_decl(mk))
				continue;
			hir_add_module_decl(cn, msrc, mod_name, ast, &full, &fulln, &fullcap, &expset, &expn, &expcap, exported, ds,
			                    module_is_device, file_local, &fileset, &filesetn, &filesetcap, uid);
		}
		/* Rename this file's `#file` decls (+ intra-file refs) to a file-unique identity `<mod>.__f<m>`
		 * (mirror of semantic): file-local visibility, and no codegen collision between two sibling
		 * files' same-named `#file` decls. */
		if (filesetn > 0) {
			char fprefix[300];
			snprintf(fprefix, sizeof(fprefix), "%s.__f%d", mod_name, m);
			for (int d = file_first; d < ast->decl_count; d++)
				hir_rn_decl(ast->decls[d], fprefix, fileset, filesetn);
			for (int x = 0; x < filesetn; x++)
				free(fileset[x]);
		}
		free(fileset);
	}
	if (!found) {
		free(full);
		free(expset);
		return;
	}
	/* Scope resolution (mirror of sem_inline_module): rename this module's pure-Arche decls + their
	 * intra-module references to the qualified identity `<mod>.<name>`; foreign decls keep their
	 * C-symbol name. */
	for (int d = first; d < ast->decl_count; d++)
		hir_rn_decl(ast->decls[d], mod_name, full, fulln);
	for (int x = 0; x < fulln; x++)
		free(full[x]);
	free(full);
	if (*inlined < 64) {
		mod_prefix[*inlined] = dupz(mod_name);
		mod_exports[*inlined] = expset;
		mod_export_n[*inlined] = expn;
		(*inlined)++;
	} else {
		for (int x = 0; x < expn; x++)
			free(expset[x]);
		free(expset);
	}
	for (int m = 0; m < g_module_count; m++) {
		if (strcmp(g_modules[m].name, mod_name) != 0)
			continue;
		const SyntaxNode *mr = g_modules[m].root;
		const char *msrc = g_modules[m].src;
		for (int j = 0; j < mr->child_count; j++) {
			if (mr->children[j].tag != SE_NODE || mr->children[j].as.node->kind != SN_USE_DECL)
				continue;
			const SyntaxNode *un = mr->children[j].as.node;
			for (int t = 0; t < un->child_count; t++) {
				if (un->children[t].tag != SE_TOKEN)
					continue;
				char *sub = import_token_module_name(msrc, &un->children[t]);
				if (!sub)
					continue;
				hir_inline_module(sub, ast, mod_prefix, mod_exports, mod_export_n, inlined);
				free(sub);
			}
		}
	}
}

/* Collect `@implements(<device>.<req>, …)` bindings from one driver decl into g_impl: each
 * requirement's tail name (`foo` in `physics.foo`) maps to this decl's own name (`bar`). The device's
 * uses of `foo` are then substituted to `bar`, so its systems bind to the driver's shape. */
static void collect_impl_binds(const SyntaxNode *decl, const char *src) {
	SynText bn = lower_binding_name((SyntaxView){decl, src});
	if (!bn.ptr)
		return;
	char *local = txt_dup(bn);
	int n = decl->child_count;
	for (int i = 0; i + 1 < n; i++) {
		SyntaxElem *a = &decl->children[i], *b = &decl->children[i + 1];
		if (a->tag != SE_TOKEN || a->as.token.kind != TOK_AT)
			continue;
		if (b->tag != SE_TOKEN || b->as.token.kind != TOK_IDENT)
			continue;
		if (b->as.token.length != 10 || strncmp(src + b->as.token.offset, "implements", 10) != 0)
			continue;
		int j = i + 2;
		if (j >= n || decl->children[j].tag != SE_TOKEN || decl->children[j].as.token.kind != TOK_LPAREN)
			continue;
		const char *tail = NULL;
		int tail_len = 0;
		for (j++; j < n; j++) {
			SyntaxElem *t = &decl->children[j];
			if (t->tag != SE_TOKEN)
				continue;
			TokenKind k = t->as.token.kind;
			if (k == TOK_IDENT) {
				tail = src + t->as.token.offset; /* keep the LAST segment of a qualified `dev.foo` */
				tail_len = (int)t->as.token.length;
			} else if (k == TOK_COMMA || k == TOK_RPAREN) {
				if (tail && g_impl_count < 64) {
					char *old = malloc((size_t)tail_len + 1);
					memcpy(old, tail, (size_t)tail_len);
					old[tail_len] = '\0';
					g_impl[g_impl_count].old = old;
					g_impl[g_impl_count].new_ = dupz(local);
					g_impl_count++;
				}
				tail = NULL;
				if (k == TOK_RPAREN)
					break;
			}
		}
	}
	free(local);
}

/* The lowering entry: syntax tree + semantic side model -> HIR. */
HirProgram *lower_to_hir(const SyntaxNode *root, const char *src) {
	if (!root)
		return NULL;
	HirProgram *ast = hir_program_create();
	SyntaxView r = sv_root(root, src);
	build_tgroups(root, src); /* tuple-group consts → archetype-field expansion table */
	g_synth_arch_count = 0;   /* synthetic archetypes minted from anonymous `arche {…}` literals */
	/* Deep count: decls nested inside `#foreign { }` / `#module { }` block regions are collected
	 * too (see the region recursion below), so the shallow top-level child count would undersize
	 * the array and overflow it. sv_node_count_deep is a safe (over-)estimate. */
	int cap = sv_node_count_deep(r);
	for (int m = 0; m < g_module_count; m++)
		cap += sv_node_count_deep(sv_root(g_modules[m].root, g_modules[m].src));
	ast->decls = calloc(cap ? cap : 1, sizeof(HirDecl *));
	ast->decl_count = 0;

	/* Per inlined module: its prefix + the bare names it exported (for the
	 * cross-module bare-reference pass below). */
	char *mod_prefix[64];
	char **mod_exports[64];
	int mod_export_n[64];
	int inlined = 0;

	for (int i = 0; i < root->child_count; i++) {
		if (root->children[i].tag != SE_NODE)
			continue;
		SyntaxNodeKind k = root->children[i].as.node->kind;
		/* Region marker: banner contributes no decls here (siblings collected normally); a
		 * `{ ... }` block's child decls are collected inline (no main-file export band). */
		if (k == SN_REGION) {
			const SyntaxNode *rn = root->children[i].as.node;
			if (sv_has_token((SyntaxView){rn, src}, TOK_LBRACE)) {
				for (int c = 0; c < rn->child_count; c++) {
					if (rn->children[c].tag != SE_NODE)
						continue;
					if (!hir_is_collectible_decl(rn->children[c].as.node->kind))
						continue;
					HirDecl *ad = lower_decl_cst((SyntaxView){rn->children[c].as.node, src});
					if (ad)
						ast->decls[ast->decl_count++] = ad;
				}
			}
			continue;
		}
		if (k < SN_WORLD_DECL || k > SN_USE_DECL)
			continue;
		SyntaxView dv = {root->children[i].as.node, src};

		if (k == SN_USE_DECL) {
			/* One element per import: IDENT = device by name, STRING = module by path. Inline each:
			 * auto-prefix every name it declares (and internal refs) with `<module>_`. A module is a
			 * folder, so it may register as several files under one name — inline all. */
			const SyntaxNode *un = dv.node;
			for (int t = 0; t < un->child_count; t++) {
				char *mod_name = import_token_module_name(src, &un->children[t]);
				if (!mod_name)
					continue;
				hir_inline_module(mod_name, ast, mod_prefix, mod_exports, mod_export_n, &inlined);
				free(mod_name);
			} /* end per-import loop */
			continue;
		}

		HirDecl *ad = lower_decl_cst(dv);
		if (ad)
			ast->decls[ast->decl_count++] = ad;
	}

	/* Scope resolution: bind every `mod.member` reference to its member's qualified identity (mirror
	 * of sem_qualify_decl). Lookup is by literal member name in the module's export set. */
	if (inlined > 0) {
		QualCtx q = {mod_prefix, mod_exports, mod_export_n, inlined};
		for (int d = 0; d < ast->decl_count; d++)
			hir_q_decl(ast->decls[d], &q);
	}

	/* `@implements` bindings (driver decls). Collect `<req-tail> → <decl-name>`, drop the device's
	 * datasheet requirement decl (the driver's decl is the real definition), then substitute the
	 * requirement name → the driver's name everywhere so the device's systems bind to the driver's
	 * shape (the substitution fires via g_impl inside the reused rename traversal). */
	g_impl_count = 0;
	for (int i = 0; i < root->child_count; i++)
		if (root->children[i].tag == SE_NODE && root->children[i].as.node->kind >= SN_WORLD_DECL &&
		    root->children[i].as.node->kind <= SN_USE_DECL)
			collect_impl_binds(root->children[i].as.node, src);
	if (g_impl_count > 0) {
		int w = 0;
		for (int d = 0; d < ast->decl_count; d++) {
			const char *nm = hir_decl_name(ast->decls[d]);
			int drop = 0;
			if (nm)
				for (int b = 0; b < g_impl_count; b++)
					if (strcmp(nm, g_impl[b].old) == 0) {
						drop = 1;
						break;
					}
			if (!drop)
				ast->decls[w++] = ast->decls[d]; /* dropped decl left to the arena */
		}
		ast->decl_count = w;
		for (int d = 0; d < ast->decl_count; d++) {
			HirDecl *dd = ast->decls[d];
			hir_rn_decl(dd, "", NULL, 0); /* count=0 → only g_impl substitutions fire (names + body refs) */
			/* Column-binding names that the traversal skips: sys params, archetype fields. */
			if (dd->kind == HIR_DECL_SYS)
				for (int p = 0; p < dd->data.sys->param_count; p++)
					subst_name(&dd->data.sys->params[p]->name);
			else if (dd->kind == HIR_DECL_ARCHETYPE)
				for (int f = 0; f < dd->data.archetype->field_count; f++)
					subst_name(&dd->data.archetype->fields[f]->name);
		}
		for (int b = 0; b < g_impl_count; b++) {
			free(g_impl[b].old);
			free(g_impl[b].new_);
		}
		g_impl_count = 0;
	}

	/* Module exports are reachable ONLY via qualified access (`mod.name`, handled by the qualify
	 * pass above) — a bare `name` does NOT resolve to a module export. (`core` is prepended, not
	 * inlined, so its bare names are unaffected; externs keep their C-ABI bare name as top-level
	 * symbols.) */
	for (int m = 0; m < inlined; m++) {
		for (int s = 0; s < mod_export_n[m]; s++)
			free(mod_exports[m][s]);
		free(mod_exports[m]);
		free(mod_prefix[m]);
	}
	/* Register synthetic archetypes minted from anonymous `arche {…}` literals during expression
	 * lowering. They carry no module refs (skip the qualify pass); a structurally-identical named
	 * shape, declared earlier, stays canonical so canonical_archetype_decl folds the literal onto it. */
	if (g_synth_arch_count > 0) {
		ast->decls = realloc(ast->decls, (size_t)(ast->decl_count + g_synth_arch_count) * sizeof(HirDecl *));
		for (int i = 0; i < g_synth_arch_count; i++)
			ast->decls[ast->decl_count++] = g_synth_arch[i];
	}
	/* A shape may be DEFINED in several places — a device impl that uses it AND the driver, say — and
	 * every definition is the same canonical shape. Collapse duplicate same-named archetype decls to ONE
	 * here: codegen folds the struct via canonical_archetype_decl, but a per-system call site enumerates
	 * matching archetype DECLS, so two `Pt` decls would emit `f(%struct.Pt*, %struct.Pt*)` (a duplicate
	 * parameter). Keep the first decl for each name; free the rest. */
	{
		int w = 0;
		for (int r = 0; r < ast->decl_count; r++) {
			HirDecl *d = ast->decls[r];
			int dup = 0;
			if (d->kind == HIR_DECL_ARCHETYPE && d->data.archetype && d->data.archetype->name) {
				for (int e = 0; e < w; e++)
					if (ast->decls[e]->kind == HIR_DECL_ARCHETYPE && ast->decls[e]->data.archetype &&
					    ast->decls[e]->data.archetype->name &&
					    strcmp(ast->decls[e]->data.archetype->name, d->data.archetype->name) == 0) {
						dup = 1;
						break;
					}
			}
			if (dup)
				hir_decl_free(d);
			else
				ast->decls[w++] = d;
		}
		ast->decl_count = w;
	}
	/* Collapse nested tuple-field accesses (`arch.pos.x` → `arch.pos_x`) to match the
	 * flattened archetype columns; no-op when no tuple groups are declared. */
	if (g_tgroup_count > 0)
		for (int i = 0; i < ast->decl_count; i++)
			tuple_collapse_decl(ast->decls[i]);
	return ast;
}

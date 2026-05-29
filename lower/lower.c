#include "lower.h"
#include "../cst/cst_view.h"
#include "../semantic/sem_model.h"
#include "../semantic/semantic.h"
#include <stdlib.h>
#include <string.h>

/* Semantic context for CST-driven lowering: resolves nominal type aliases (e.g.
 * `file` -> `opaque`), which the AstProgram path got via in-place erasure. */
static SemanticContext *g_lower_sem = NULL;
void lower_set_sem(struct SemanticContext *ctx) {
	g_lower_sem = ctx;
}

/* Resolved types live in the semantic side model (keyed by CST node id), set by
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
	if (strcmp(s, "byte") == 0) {
		*width = 8;
		*is_signed = 0;
		return 1;
	}
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
		/* archetype or other named type — pointer into CST, safe since CST outlives AST */
		t.tag = HIR_TYPE_NAMED;
		t.name = resolved_type;
	}
	return t;
}

/* =========================
   Tuple desugaring (CST -> AST)
   =========================
   Tuples exist only in the CST. Lowering flattens every tuple field
   `pos: (x, y)` into scalar columns `pos_x`, `pos_y`, and rewrites system
   parameters and bodies accordingly (tuple_rewrite_*), so the AST (and every
   later pass) never sees a tuple. The CST-path tuple group registry
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
	default:
		break;
	}
}

/* =========================================================================
   CST-driven lowering (alternative to the AstProgram-based path above). Reads the
   lossless CST via cst_view + the semantic side model. Gated by ARCHE_LOWER_CST
   until validated IR-identical; the AstProgram path remains the default. Reuses the
   AST-level helpers above (map_type_str, tuple registry, tuple_rewrite_*).
   ========================================================================= */

static HirExpr *lower_expr_cst(CstView e);
static HirStmt *lower_stmt_cst(CstView s);

/* malloc'd NUL-terminated copy of a view's source text. */
static char *cv_dup(CstView v) {
	CvText t = cv_text(v);
	char *s = malloc(t.len + 1);
	if (t.ptr)
		memcpy(s, t.ptr, t.len);
	s[t.len] = '\0';
	return s;
}

/* malloc'd NUL-terminated copy of a borrowed text slice. */
static char *dupz(const char *s); /* defined with the module helpers below */

static char *txt_dup(CvText t) {
	char *s = malloc(t.len + 1);
	if (t.ptr)
		memcpy(s, t.ptr, t.len);
	s[t.len] = '\0';
	return s;
}

/* Lower a CST type node (SN_TYPE_*) to an HirType, mirroring lower_type_ref. */
static HirType *lower_type_cst(CstView t) {
	if (!cv_present(t))
		return NULL;
	HirType *at = hir_type_create(HIR_TYPE_UNKNOWN);
	switch (cv_kind(t)) {
	case SN_TYPE_REF: {
		char *raw = txt_dup(cv_token(t, TOK_IDENT));
		const char *r = g_lower_sem ? semantic_resolve_type_alias(g_lower_sem, raw) : raw;
		char *name = malloc(strlen(r) + 1);
		strcpy(name, r);
		free(raw);
		if (strcmp(name, "archetype") == 0)
			at->tag = HIR_TYPE_ARCHETYPE;
		else if (strcmp(name, "opaque") == 0)
			at->tag = HIR_TYPE_OPAQUE;
		else if (strcmp(name, "type") == 0)
			; /* meta-type erased; leave UNKNOWN defensively */
		else
			*at = map_type_str(name); /* borrows name via HIR_TYPE_NAMED below */
		/* map_type_str's HIR_TYPE_NAMED .name points at its argument; keep a stable copy. */
		if (at->tag == HIR_TYPE_NAMED)
			at->name = name;
		else
			free(name);
		break;
	}
	case SN_TYPE_ARRAY: {
		at->tag = HIR_TYPE_ARRAY;
		HirType *elem = hir_type_create(HIR_TYPE_UNKNOWN);
		char *en = txt_dup(cv_token(t, TOK_IDENT));
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
		char *en = txt_dup(cv_token(t, TOK_IDENT));
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
		CvText h = cv_token(t, TOK_IDENT); /* "handle" */
		(void)h;
		/* find the IDENT that isn't "handle" */
		for (int i = 0; i < t.node->child_count; i++)
			if (t.node->children[i].tag == SE_TOKEN && t.node->children[i].as.token.kind == TOK_IDENT) {
				CvText nm = {t.src + t.node->children[i].as.token.offset, t.node->children[i].as.token.length};
				if (!(nm.len == 6 && memcmp(nm.ptr, "handle", 6) == 0)) {
					at->name = txt_dup(nm);
					break;
				}
			}
		break;
	}
	default:
		break;
	}
	return at;
}

/* Resolved type of a CST expression node, from the side model (keyed by node id). */
static HirType cst_expr_type(CstView e) {
	return map_type_str(g_lower_model ? sem_model_expr_type(g_lower_model, cv_id(e)) : NULL);
}

/* Decode a string token's content (quotes + escapes) like parse_primary_expr does. */
static char *cst_decode_string(CvText raw, int *out_len) {
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
static CstView cst_first_expr(CstView v) {
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = v.node->children[i].as.node->kind;
			if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR) {
				CstView r = {v.node->children[i].as.node, v.src};
				return r;
			}
		}
	CstView none = {NULL, v.src};
	return none;
}

static Operator cst_tok_to_op(TokenKind k) {
	switch (k) {
	case TOK_PLUS:
		return OP_ADD;
	case TOK_MINUS:
		return OP_SUB;
	case TOK_STAR:
		return OP_MUL;
	case TOK_SLASH:
		return OP_DIV;
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
	default:
		return OP_NONE;
	}
}

/* cv_type_at / cv_type_count now live in cst_view.h (shared with the analyzer). */

/* nth child node that is an expression kind (skips role/name/type wrappers). */
static CstView cv_node_at_expr(CstView v, int idx) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = v.node->children[i].as.node->kind;
			if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR) {
				if (c == idx) {
					CstView r = {v.node->children[i].as.node, v.src};
					return r;
				}
				c++;
			}
		}
	CstView none = {NULL, v.src};
	return none;
}

static HirExpr *lower_expr_cst(CstView e) {
	if (!cv_present(e))
		return NULL;
	HirExpr *ax = hir_expr_create(HIR_EXPR_LITERAL);
	ax->resolved = cst_expr_type(e);

	switch (cv_kind(e)) {
	case SN_PAREN_EXPR:
		/* transparent: lower the inner expression */
		return lower_expr_cst(cst_first_expr(e));
	case SN_LITERAL_EXPR:
		ax->kind = HIR_EXPR_LITERAL;
		ax->data.literal.lexeme = cv_dup(e);
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
		ax->data.string.value = cst_decode_string(cv_text(e), &n);
		ax->data.string.length = n;
		break;
	}
	case SN_NAME_EXPR: {
		ax->kind = HIR_EXPR_NAME;
		/* table<Name> in value position resolves to the bare archetype name */
		if (cv_has_token(e, TOK_LT)) {
			/* second IDENT is the archetype name */
			char *nm = NULL;
			int seen = 0;
			for (int i = 0; i < e.node->child_count; i++)
				if (e.node->children[i].tag == SE_TOKEN && e.node->children[i].as.token.kind == TOK_IDENT) {
					CvText t = {e.src + e.node->children[i].as.token.offset, e.node->children[i].as.token.length};
					if (seen++) {
						nm = txt_dup(t);
						break;
					}
				}
			ax->data.name.name = nm ? nm : cv_dup(e);
		} else {
			ax->data.name.name = txt_dup(cv_token(e, TOK_IDENT));
		}
		break;
	}
	case SN_FIELD_EXPR: {
		/* `base.f1.f2…[idx]` is flat under one node: base IDENT, then (DOT FIELD_NAME)+,
		 * optionally a trailing `[indices]`. Rebuild the left-assoc AST. */
		HirExpr *base = hir_expr_create(HIR_EXPR_NAME);
		base->data.name.name = txt_dup(cv_token(e, TOK_IDENT));
		HirExpr *cur = base;
		int nfields = cv_count(e, SN_FIELD_NAME);
		for (int i = 0; i < nfields; i++) {
			HirExpr *f = hir_expr_create(HIR_EXPR_FIELD);
			f->data.field.base = cur;
			f->data.field.field_name = cv_dup(cv_child_at(e, SN_FIELD_NAME, i));
			cur = f;
		}
		/* trailing index over the final field */
		if (cv_has_token(e, TOK_LBRACKET)) {
			HirExpr *idx = hir_expr_create(HIR_EXPR_INDEX);
			/* the column's element type is this expr's resolved type; codegen reads it
			 * off the base field to size the store/GEP. */
			if (cur->kind == HIR_EXPR_FIELD)
				cur->resolved = cst_expr_type(e);
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
						CstView iv = {e.node->children[i].as.node, e.src};
						idx->data.index.indices[idx->data.index.index_count++] = lower_expr_cst(iv);
					}
				}
			cur = idx;
		}
		free(ax);
		cur->resolved = cst_expr_type(e);
		return cur;
	}
	case SN_INDEX_EXPR: {
		ax->kind = HIR_EXPR_INDEX;
		/* base may be a `name.f1.f2…` member chain folded into this node
		 * (e.g. `Particle.pos_x[0]`); rebuild the FIELD chain over the IDENT. */
		HirExpr *base = hir_expr_create(HIR_EXPR_NAME);
		base->data.name.name = txt_dup(cv_token(e, TOK_IDENT));
		int nfields = cv_count(e, SN_FIELD_NAME);
		for (int i = 0; i < nfields; i++) {
			HirExpr *f = hir_expr_create(HIR_EXPR_FIELD);
			f->data.field.base = base;
			f->data.field.field_name = cv_dup(cv_child_at(e, SN_FIELD_NAME, i));
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
					CstView iv = {e.node->children[i].as.node, e.src};
					ax->data.index.indices[ax->data.index.index_count++] = lower_expr_cst(iv);
				}
			}
		break;
	}
	case SN_BINARY_EXPR: {
		ax->kind = HIR_EXPR_BINARY;
		/* operator token */
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_TOKEN) {
				Operator op = cst_tok_to_op(e.node->children[i].as.token.kind);
				if (op != OP_NONE) {
					ax->data.binary.op = op;
					break;
				}
			}
		ax->data.binary.left = lower_expr_cst(cv_node_at_expr(e, 0));
		ax->data.binary.right = lower_expr_cst(cv_node_at_expr(e, 1));
		break;
	}
	case SN_UNARY_EXPR: {
		ax->kind = HIR_EXPR_UNARY;
		CvText op = {NULL, 0};
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
		ax->data.unary.operand = lower_expr_cst(cst_first_expr(e));
		break;
	}
	case SN_CALL_EXPR: {
		ax->kind = HIR_EXPR_CALL;
		HirExpr *callee = hir_expr_create(HIR_EXPR_NAME);
		callee->data.name.name = cv_dup(cv_child(e, SN_CALLEE_NAME));
		ax->data.call.callee = callee;
		int ac = 0;
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_NODE) {
				SyntaxNodeKind k = e.node->children[i].as.node->kind;
				if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
					ac++;
			}
		ax->data.call.args = calloc(ac ? ac : 1, sizeof(HirExpr *));
		ax->data.call.arg_count = 0;
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_NODE) {
				SyntaxNodeKind k = e.node->children[i].as.node->kind;
				if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR) {
					CstView av = {e.node->children[i].as.node, e.src};
					ax->data.call.args[ax->data.call.arg_count++] = lower_expr_cst(av);
				}
			}
		break;
	}
	default:
		/* SN_ALLOC_EXPR / SN_ARRAY_LIT_EXPR and any unhandled: leave as a placeholder
		 * literal for now (these surface in verify-codegen if exercised). */
		ax->kind = HIR_EXPR_LITERAL;
		ax->data.literal.lexeme = cv_dup(e);
		break;
	}
	return ax;
}

/* Lower the statement-kind child nodes of `parent` into an HirStmt array. */
static HirStmt **cst_lower_body(CstView parent, int *out_count) {
	int n = 0;
	for (int i = 0; i < parent.node->child_count; i++)
		if (parent.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = parent.node->children[i].as.node->kind;
			if (k >= SN_BIND_STMT && k <= SN_EACH_FIELD_STMT)
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
			if (k >= SN_BIND_STMT && k <= SN_EACH_FIELD_STMT) {
				CstView sv = {parent.node->children[i].as.node, parent.src};
				out[j++] = lower_stmt_cst(sv);
			}
		}
	return out;
}

static Operator cst_assign_op(TokenKind k) {
	switch (k) {
	case TOK_PLUS_EQ:
		return OP_ADD;
	case TOK_MINUS_EQ:
		return OP_SUB;
	case TOK_STAR_EQ:
		return OP_MUL;
	case TOK_SLASH_EQ:
		return OP_DIV;
	default:
		return OP_NONE; /* plain `=` */
	}
}

static HirStmt *lower_stmt_cst(CstView s) {
	HirStmt *as = hir_stmt_create(HIR_STMT_EXPR);

	switch (cv_kind(s)) {
	case SN_BIND_STMT: {
		if (g_lower_model && sem_model_bind_alias(g_lower_model, cv_id(s))) {
			as->kind = HIR_STMT_EXPR;
			HirExpr *zero = hir_expr_create(HIR_EXPR_LITERAL);
			zero->data.literal.lexeme = malloc(2);
			strcpy(zero->data.literal.lexeme, "0");
			as->data.expr_stmt.expr = zero;
			break;
		}
		as->kind = HIR_STMT_BIND;
		CstView target = cv_node_at_expr(s, 0);
		as->data.bind_stmt.name_count = 1;
		as->data.bind_stmt.names = calloc(1, sizeof(char *));
		as->data.bind_stmt.names[0] = cv_dup(target);
		as->data.bind_stmt.type = lower_type_cst(cv_type_at(s, 0));
		as->data.bind_stmt.value = lower_expr_cst(cv_node_at_expr(s, 1));
		break;
	}
	case SN_ASSIGN_STMT: {
		as->kind = HIR_STMT_ASSIGN;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_TOKEN) {
				TokenKind tk = s.node->children[i].as.token.kind;
				if (tk == TOK_EQ || tk == TOK_PLUS_EQ || tk == TOK_MINUS_EQ || tk == TOK_STAR_EQ ||
				    tk == TOK_SLASH_EQ) {
					as->data.assign_stmt.op = cst_assign_op(tk);
					break;
				}
			}
		as->data.assign_stmt.target = lower_expr_cst(cv_node_at_expr(s, 0));
		as->data.assign_stmt.value = lower_expr_cst(cv_node_at_expr(s, 1));
		break;
	}
	case SN_EXPR_STMT:
		as->kind = HIR_STMT_EXPR;
		as->data.expr_stmt.expr = lower_expr_cst(cv_node_at_expr(s, 0));
		break;
	case SN_BREAK_STMT:
		as->kind = HIR_STMT_BREAK;
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
			as->data.return_stmt.values[i] = lower_expr_cst(cv_node_at_expr(s, i));
		break;
	}
	case SN_RUN_STMT: {
		as->kind = HIR_STMT_RUN;
		/* `run sys` / `run sys in world`: `run` is an identifier (not a keyword), so the
		 * IDENTs are [run, sys, world?] (the `in` keyword is TOK_IN). System = IDENT[1]. */
		char *names[3] = {NULL, NULL, NULL};
		int ni = 0;
		for (int i = 0; i < s.node->child_count && ni < 3; i++)
			if (s.node->children[i].tag == SE_TOKEN && s.node->children[i].as.token.kind == TOK_IDENT) {
				CvText t = {s.src + s.node->children[i].as.token.offset, s.node->children[i].as.token.length};
				names[ni++] = txt_dup(t);
			}
		as->data.run_stmt.system_name = names[1] ? names[1] : txt_dup((CvText){"", 0});
		as->data.run_stmt.world_name = names[2];
		free(names[0]);
		break;
	}
	case SN_IF_STMT: {
		as->kind = HIR_STMT_IF;
		as->data.if_stmt.cond = lower_expr_cst(cv_node_at_expr(s, 0));
		/* then-body = stmt children directly under the IF; else-body under ELSE_CLAUSE. */
		CstView elsec = cv_child(s, SN_ELSE_CLAUSE);
		as->data.if_stmt.then_body = cst_lower_body(s, &as->data.if_stmt.then_count);
		if (cv_present(elsec))
			as->data.if_stmt.else_body = cst_lower_body(elsec, &as->data.if_stmt.else_count);
		break;
	}
	case SN_FOR_STMT: {
		as->kind = HIR_STMT_FOR;
		if (cv_has_token(s, TOK_LPAREN)) {
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
				CstView cv = {ch->as.node, s.src};
				if (seen_brace) {
					if (k >= SN_BIND_STMT && k <= SN_EACH_FIELD_STMT)
						nbody++;
					continue;
				}
				if (seg == 0 && k >= SN_BIND_STMT && k <= SN_EACH_FIELD_STMT)
					as->data.for_stmt.init = lower_stmt_cst(cv);
				else if (seg == 1 && k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
					as->data.for_stmt.cond = lower_expr_cst(cv);
				else if (seg == 2 && k >= SN_BIND_STMT && k <= SN_EACH_FIELD_STMT)
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
				if (k >= SN_BIND_STMT && k <= SN_EACH_FIELD_STMT) {
					CstView cv = {ch->as.node, s.src};
					as->data.for_stmt.body[as->data.for_stmt.body_count++] = lower_stmt_cst(cv);
				}
			}
			break;
		}
		/* range form `for IDENT in IDENT { body }`, or infinite `for { body }`. */
		int ni = 0;
		char *vname = NULL, *iname = NULL;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_TOKEN && s.node->children[i].as.token.kind == TOK_IDENT) {
				CvText t = {s.src + s.node->children[i].as.token.offset, s.node->children[i].as.token.length};
				if (ni == 0)
					vname = txt_dup(t);
				else if (ni == 1)
					iname = txt_dup(t);
				ni++;
			}
		as->data.for_stmt.var_name = vname;
		if (iname) {
			HirExpr *it = hir_expr_create(HIR_EXPR_NAME);
			it->data.name.name = iname;
			as->data.for_stmt.iterable = it;
		}
		as->data.for_stmt.body = cst_lower_body(s, &as->data.for_stmt.body_count);
		break;
	}
	case SN_EACH_FIELD_STMT: {
		as->kind = HIR_STMT_EACH_FIELD;
		int ni = 0;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_TOKEN && s.node->children[i].as.token.kind == TOK_IDENT) {
				CvText t = {s.src + s.node->children[i].as.token.offset, s.node->children[i].as.token.length};
				if (ni == 0)
					as->data.each_field.binding_name = txt_dup(t);
				else if (ni == 1)
					as->data.each_field.arch_param_name = txt_dup(t);
				ni++;
			}
		as->data.each_field.filter_type = lower_type_cst(cv_type_at(s, 0));
		as->data.each_field.body = cst_lower_body(s, &as->data.each_field.body_count);
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
			as->data.multi_bind.targets[ti].name = txt_dup((CvText){pend, pend_len});                                  \
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
					CvText t = cv_token((CstView){ch->as.node, s.src}, TOK_IDENT);
					pend = t.ptr;
					pend_len = (int)t.len;
					pend_active = 1;
				} else if (k >= SN_TYPE_REF && k <= SN_TYPE_HANDLE && pend_active) {
					pend_type = lower_type_cst((CstView){ch->as.node, s.src});
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
					as->data.multi_bind.value = lower_expr_cst((CstView){s.node->children[i].as.node, s.src});
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
			CstView cnv = (CstView){cn, s.src};
			if (cn->kind == SN_CALL_EXPR) {
				as->data.multi_bind.value = lower_expr_cst(cnv);
			} else if (cn->kind == SN_OUT_ARG) {
				int ti = as->data.multi_bind.target_count++;
				as->data.multi_bind.targets[ti].name = txt_dup(cv_token(cnv, TOK_IDENT));
				as->data.multi_bind.targets[ti].is_new = cv_has_token(cnv, TOK_COLON);
				as->data.multi_bind.targets[ti].type =
				    cv_type_count(cnv) > 0 ? lower_type_cst(cv_type_at(cnv, 0)) : NULL;
			}
		}
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

static HirParam *lower_param_cst(CstView p) {
	HirParam *ap = hir_param_create(NULL, NULL);
	ap->name = cv_dup(cv_child(p, SN_PARAM_NAME));
	ap->type = lower_type_cst(cv_type_at(p, 0)); /* NULL for sys params */
	ap->is_own = cv_has_token(p, TOK_OWN);
	return ap;
}

/* ---- tuple-group registry (CST equivalent of main.c expand_archetype_tuple_groups) ----
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
				CvText t = {src + ch->as.token.offset, ch->as.token.length};
				if (!in_paren && !g->name)
					g->name = txt_dup(t);
				else if (in_paren)
					g->suffix[g->nsuf++] = txt_dup(t);
			}
		} else {
			SyntaxNodeKind kk = ch->as.node->kind;
			if (kk >= SN_TYPE_REF && kk <= SN_TYPE_HANDLE && g->member.tag == HIR_TYPE_UNKNOWN) {
				HirType *mt = lower_type_cst((CstView){ch->as.node, src});
				if (mt)
					g->member = *mt;
			}
		}
	}
	if (g->name && g->nsuf > 0)
		g_tgroup_count++;
}

static void build_tgroups(const SyntaxNode *root, const char *src) {
	g_tgroup_count = 0;
	for (int i = 0; i < root->child_count; i++) {
		if (root->children[i].tag != SE_NODE)
			continue;
		const SyntaxNode *d = root->children[i].as.node;
		/* top-level tuple group: `pos (x, y) :: T` */
		if (d->kind == SN_CONST_DECL && cv_has_token((CstView){d, src}, TOK_LPAREN))
			register_tgroup(d, src, 0, d->child_count, NULL);
		/* inline archetype tuple field: `pos (x, y) :: T` inside `arche { … }` */
		else if (d->kind == SN_ARCHETYPE_DECL) {
			for (int k = 0; k < d->child_count; k++) {
				if (d->children[k].tag != SE_NODE || d->children[k].as.node->kind != SN_FIELD_NAME)
					continue;
				/* a `(` immediately following this FIELD_NAME marks an inline tuple */
				int j = k + 1;
				if (j < d->child_count && d->children[j].tag == SE_TOKEN &&
				    d->children[j].as.token.kind == TOK_LPAREN) {
					int e = j;
					while (e < d->child_count &&
					       !(d->children[e].tag == SE_NODE && d->children[e].as.node->kind == SN_FIELD_NAME && e != k))
						e++;
					char *nm = cv_dup((CstView){d->children[k].as.node, src});
					register_tgroup(d, src, j, e, nm);
					free(nm);
				}
			}
		}
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

static HirDecl *lower_decl_cst(CstView d) {
	switch (cv_kind(d)) {
	case SN_USE_DECL:
		return NULL;
	case SN_WORLD_DECL: {
		HirDecl *ad = hir_decl_create(HIR_DECL_WORLD);
		ad->data.world = calloc(1, sizeof(HirWorldDecl));
		ad->data.world->name = txt_dup(cv_token(d, TOK_IDENT));
		return ad;
	}
	case SN_ARCHETYPE_DECL: {
		HirDecl *ad = hir_decl_create(HIR_DECL_ARCHETYPE);
		HirArchetypeDecl *aa = calloc(1, sizeof(HirArchetypeDecl));
		aa->name = cv_dup(cv_child(d, SN_TYPE_DEF_NAME));
		int nf = cv_count(d, SN_FIELD_NAME);
		int cap = nf > 0 ? nf : 1;
		aa->fields = calloc(cap, sizeof(HirField *));
		aa->field_count = 0;
		for (int i = 0; i < d.node->child_count; i++) {
			if (d.node->children[i].tag != SE_NODE || d.node->children[i].as.node->kind != SN_FIELD_NAME)
				continue;
			CstView fn = {d.node->children[i].as.node, d.src};
			/* type = a type node before the next field; else a bare field (`arche { a, b }`)
			 * whose component type is the field name itself (resolved through aliases). */
			CstView ty = {NULL, d.src};
			for (int k = i + 1; k < d.node->child_count; k++)
				if (d.node->children[k].tag == SE_NODE) {
					SyntaxNodeKind kk = d.node->children[k].as.node->kind;
					if (kk == SN_FIELD_NAME)
						break;
					if (kk >= SN_TYPE_REF && kk <= SN_TYPE_HANDLE) {
						ty.node = d.node->children[k].as.node;
						break;
					}
				}
			char *raw = cv_dup(fn);
			/* bare field matching a top-level group, or an inline `pos (x,y) :: T` field
			 * (both registered in the tuple-group table) → one tuple-typed column. */
			if (aa->field_count >= cap) {
				cap *= 2;
				aa->fields = realloc(aa->fields, cap * sizeof(HirField *));
			}
			CstTupleGroup *g = tgroup_lookup(raw);
			if (g) {
				/* tuple group: flatten to scalar columns `pos_x`, `pos_y` (mirrors the
				 * AstProgram path; codegen has no tuple type). `pos.x`/`Body.pos.x` accesses
				 * are collapsed to the combined column name during statement lowering. */
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
			af->name = cv_dup(fn);
			if (cv_present(ty)) {
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
		ap->name = cv_dup(cv_child(d, SN_FUNC_DEF_NAME));
		ap->is_extern = cv_has_token(d, TOK_EXTERN);
		int np = cv_count(d, SN_PARAM);
		ap->params = calloc(np ? np : 1, sizeof(HirParam *));
		for (int i = 0; i < np; i++)
			ap->params[i] = lower_param_cst(cv_child_at(d, SN_PARAM, i));
		ap->param_count = np;
		/* out-params: the `(out)` list, written in place (0 = no outputs) */
		int nout = cv_count(d, SN_OUT_PARAM);
		ap->out_params = calloc(nout ? nout : 1, sizeof(HirParam *));
		for (int i = 0; i < nout; i++)
			ap->out_params[i] = lower_param_cst(cv_child_at(d, SN_OUT_PARAM, i));
		ap->out_param_count = nout;
		ap->stmts = cst_lower_body(d, &ap->stmt_count);
		ad->data.proc = ap;
		return ad;
	}
	case SN_SYS_DECL: {
		HirDecl *ad = hir_decl_create(HIR_DECL_SYS);
		HirSysDecl *as = calloc(1, sizeof(HirSysDecl));
		as->name = cv_dup(cv_child(d, SN_FUNC_DEF_NAME));
		/* Lower the body first; a tuple-group param then expands into one scalar param
		 * per component (`pos` → `pos_x`, `pos_y`) and its `pos.x` body accesses are
		 * rewritten to the flattened names (mirrors the AstProgram-path sys lowering). */
		as->stmts = cst_lower_body(d, &as->stmt_count);
		int np = cv_count(d, SN_PARAM);
		int pcount = 0;
		for (int i = 0; i < np; i++) {
			char *pn = cv_dup(cv_child(cv_child_at(d, SN_PARAM, i), SN_PARAM_NAME));
			CstTupleGroup *g = tgroup_lookup(pn);
			pcount += g ? g->nsuf : 1;
			free(pn);
		}
		as->params = calloc(pcount ? pcount : 1, sizeof(HirParam *));
		as->param_count = 0;
		for (int i = 0; i < np; i++) {
			CstView p = cv_child_at(d, SN_PARAM, i);
			char *pn = cv_dup(cv_child(p, SN_PARAM_NAME));
			CstTupleGroup *g = tgroup_lookup(pn);
			if (!g) {
				as->params[as->param_count++] = lower_param_cst(p);
				free(pn);
				continue;
			}
			for (int sx = 0; sx < as->stmt_count; sx++)
				tuple_rewrite_stmt(as->stmts[sx], pn);
			int is_own = cv_has_token(p, TOK_OWN);
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
		af->name = cv_dup(cv_child(d, SN_FUNC_DEF_NAME));
		af->is_extern = cv_has_token(d, TOK_EXTERN);
		int np = cv_count(d, SN_PARAM);
		af->params = calloc(np ? np : 1, sizeof(HirParam *));
		for (int i = 0; i < np; i++)
			af->params[i] = lower_param_cst(cv_child_at(d, SN_PARAM, i));
		af->param_count = np;
		/* return types: TYPE_REF nodes that are NOT inside params (params are wrapped in SN_PARAM) */
		int nt = cv_type_count(d);
		af->return_types = calloc(nt ? nt : 1, sizeof(HirType *));
		af->return_type_count = 0;
		for (int i = 0; i < nt; i++)
			af->return_types[af->return_type_count++] = lower_type_cst(cv_type_at(d, i));
		af->stmts = cst_lower_body(d, &af->stmt_count);
		ad->data.func = af;
		return ad;
	}
	case SN_CONST_DECL: {
		/* Tuple-group type definition `pos (x, y) :: T` mints nominal component types
		 * (pos_x, pos_y); it's compile-time only and erased before codegen. */
		if (cv_has_token(d, TOK_LPAREN))
			return NULL;
		HirDecl *ad = hir_decl_create(HIR_DECL_CONST);
		HirConstDecl *ac = calloc(1, sizeof(HirConstDecl));
		ac->name = txt_dup(cv_token(d, TOK_IDENT));
		ac->value = lower_expr_cst(cv_node_at_expr(d, 0));
		ad->data.constant = ac;
		return ad;
	}
	case SN_STATIC_DECL: {
		HirDecl *ad = hir_decl_create(HIR_DECL_STATIC);
		HirStaticDecl *sd = calloc(1, sizeof(HirStaticDecl));
		if (cv_has_token(d, TOK_LPAREN)) {
			/* `static Name(count)` / `static pool<Name>(count)` — archetype allocation */
			sd->kind = HIR_STATIC_ARCHETYPE;
			CstView ty = cv_type_at(d, 0);
			char *an = NULL;
			if (cv_present(ty) && cv_has_token(ty, TOK_LT)) {
				int seen = 0; /* pool<Name>: the archetype is the 2nd IDENT */
				for (int i = 0; i < ty.node->child_count; i++)
					if (ty.node->children[i].tag == SE_TOKEN && ty.node->children[i].as.token.kind == TOK_IDENT) {
						CvText t = {ty.src + ty.node->children[i].as.token.offset,
						            ty.node->children[i].as.token.length};
						if (seen++) {
							an = txt_dup(t);
							break;
						}
					}
			} else if (cv_present(ty)) {
				an = txt_dup(cv_token(ty, TOK_IDENT));
			}
			sd->archetype.archetype_name = an ? an : txt_dup((CvText){"", 0});
			/* `(capacity [, init_length]) [{ field: value, ... }]`. Args inside `()` are
			 * positional: capacity → field_values[0] (field_names[0]=NULL), init_length →
			 * the known row count (drives bounds-check elision). Each `field: value` in the
			 * `{}` block appends a named initializer. Expr nodes appear in both bracket
			 * kinds, so track which we're inside. */
			int cap_alloc = d.node->child_count + 1;
			sd->archetype.field_names = calloc(cap_alloc, sizeof(char *));
			sd->archetype.field_values = calloc(cap_alloc, sizeof(HirExpr *));
			sd->archetype.field_count = 0;
			int phase = 0; /* 0=outside, 1=in (), 2=in {} */
			int paren_arg = 0;
			const char *pend = NULL;
			int pend_len = 0;
			for (int i = 0; i < d.node->child_count; i++) {
				SyntaxElem *ch = &d.node->children[i];
				if (ch->tag == SE_TOKEN) {
					switch (ch->as.token.kind) {
					case TOK_LPAREN:
						phase = 1;
						break;
					case TOK_RPAREN:
						phase = 0;
						break;
					case TOK_LBRACE:
						phase = 2;
						break;
					case TOK_RBRACE:
						phase = 0;
						break;
					case TOK_IDENT:
						if (phase == 2) {
							pend = d.src + ch->as.token.offset;
							pend_len = (int)ch->as.token.length;
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
				CstView ev = {ch->as.node, d.src};
				if (phase == 1) {
					if (paren_arg == 0) {
						sd->archetype.field_values[0] = lower_expr_cst(ev);
						sd->archetype.field_count = 1;
					} else if (paren_arg == 1) {
						sd->archetype.init_length = lower_expr_cst(ev);
					}
					paren_arg++;
				} else if (phase == 2 && pend) {
					int fc = sd->archetype.field_count;
					sd->archetype.field_names[fc] = txt_dup((CvText){pend, pend_len});
					sd->archetype.field_values[fc] = lower_expr_cst(ev);
					sd->archetype.field_count++;
					pend = NULL;
				}
			}
		} else {
			/* `static name : T[size]` — static array. The name is wrapped in the first
			 * type node (TYPE_REF); the declared array type is the second. */
			sd->kind = HIR_STATIC_ARRAY;
			CstView name_ty = cv_type_at(d, 0);
			sd->array.name = txt_dup(cv_token(name_ty, TOK_IDENT));
			CstView arr_ty = cv_type_at(d, 1);
			HirType *full = lower_type_cst(arr_ty);
			/* unwrap to the element type (codegen wants element + size, not the array) */
			sd->array.element_type = (full && full->elem) ? full->elem : full;
			if (cv_present(arr_ty))
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
		}
		ad->data.static_decl = sd;
		return ad;
	}
	case SN_FUNC_GROUP_DECL: {
		/* `func g = { a, b };` — name in FUNC_DEF_NAME, members are the bare IDENTs. */
		HirDecl *ad = hir_decl_create(HIR_DECL_FUNC_GROUP);
		HirFuncGroupDecl *fg = calloc(1, sizeof(HirFuncGroupDecl));
		fg->name = cv_dup(cv_child(d, SN_FUNC_DEF_NAME));
		int nmem = 0;
		for (int i = 0; i < d.node->child_count; i++)
			if (d.node->children[i].tag == SE_TOKEN && d.node->children[i].as.token.kind == TOK_IDENT)
				nmem++;
		fg->member_names = calloc(nmem ? nmem : 1, sizeof(char *));
		fg->member_count = 0;
		for (int i = 0; i < d.node->child_count; i++)
			if (d.node->children[i].tag == SE_TOKEN && d.node->children[i].as.token.kind == TOK_IDENT) {
				CvText t = {d.src + d.node->children[i].as.token.offset, d.node->children[i].as.token.length};
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

/* ---- module inlining: CST-path equivalent of main.c's resolve_uses ----
 * `use foo;` inlines foo.arche's decls at the use site, auto-prefixing every
 * name foo declares (and foo-internal references to them) with `foo_`. main.c
 * registers each used module's CST here before calling lower_to_hir. */
typedef struct {
	char *name;
	const SyntaxNode *root;
	const char *src;
} LowerModule;
static LowerModule g_modules[64];
static int g_module_count = 0;

static char *dupz(const char *s) {
	char *r = malloc(strlen(s) + 1);
	strcpy(r, s);
	return r;
}

void lower_add_module(const char *name, const SyntaxNode *root, const char *src) {
	if (g_module_count >= 64 || !name || !root)
		return;
	g_modules[g_module_count].name = dupz(name);
	g_modules[g_module_count].root = root;
	g_modules[g_module_count].src = src;
	g_module_count++;
}

/* If `name` is in set, return a freshly-allocated `prefix_name`; else NULL. */
static char *prefixed_dup(const char *name, const char *prefix, char **set, int count) {
	if (!name)
		return NULL;
	for (int i = 0; i < count; i++)
		if (strcmp(name, set[i]) == 0) {
			char *r = malloc(strlen(prefix) + 1 + strlen(name) + 1);
			sprintf(r, "%s_%s", prefix, name);
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
/* HirType.name points into the CST (not owned) — swap the pointer, never free it. */
static void rn_const(const char **slot, const char *prefix, char **set, int count) {
	char *p = prefixed_dup(*slot, prefix, set, count);
	if (p)
		*slot = p;
}

static void hir_rn_type(HirType *t, const char *prefix, char **set, int count) {
	if (!t)
		return;
	if (t->tag == HIR_TYPE_NAMED)
		rn_const(&t->name, prefix, set, count);
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
		return d->data.static_decl->kind == HIR_STATIC_ARRAY ? d->data.static_decl->array.name
		                                                     : d->data.static_decl->archetype.archetype_name;
	case HIR_DECL_CONST:
		return d->data.constant->name;
	case HIR_DECL_WORLD:
		return d->data.world->name;
	}
	return NULL;
}

/* CST-driven entry, gated by ARCHE_LOWER_CST (validated against the IR goldens). */
HirProgram *lower_to_hir(const SyntaxNode *root, const char *src) {
	if (!root)
		return NULL;
	HirProgram *ast = hir_program_create();
	CstView r = cv_root(root, src);
	build_tgroups(root, src); /* tuple-group consts → archetype-field expansion table */
	int cap = cv_node_count(r);
	for (int m = 0; m < g_module_count; m++)
		cap += cv_node_count(cv_root(g_modules[m].root, g_modules[m].src));
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
		if (k < SN_WORLD_DECL || k > SN_USE_DECL)
			continue;
		CstView dv = {root->children[i].as.node, src};

		if (k == SN_USE_DECL) {
			/* Inline the named module's decls here, auto-prefixing every name it
			 * declares (and its internal references to them) with `<module>_`. */
			char *mod_name = txt_dup(cv_token(dv, TOK_IDENT));
			LowerModule *mod = NULL;
			for (int m = 0; m < g_module_count; m++)
				if (strcmp(g_modules[m].name, mod_name) == 0) {
					mod = &g_modules[m];
					break;
				}
			free(mod_name);
			if (!mod)
				continue;

			int first = ast->decl_count;
			const SyntaxNode *mr = mod->root;
			for (int j = 0; j < mr->child_count; j++) {
				if (mr->children[j].tag != SE_NODE)
					continue;
				SyntaxNodeKind mk = mr->children[j].as.node->kind;
				if (mk < SN_WORLD_DECL || mk > SN_USE_DECL || mk == SN_USE_DECL)
					continue;
				CstView mdv = {mr->children[j].as.node, mod->src};
				HirDecl *md = lower_decl_cst(mdv);
				if (md)
					ast->decls[ast->decl_count++] = md;
			}
			/* Collect this module's exported names (still bare), then prefix. */
			int ndefs = ast->decl_count - first;
			char **exports = calloc(ndefs ? ndefs : 1, sizeof(char *));
			int en = 0;
			for (int d = first; d < ast->decl_count; d++) {
				const char *nm = hir_decl_name(ast->decls[d]);
				if (nm)
					exports[en++] = dupz(nm);
			}
			for (int d = first; d < ast->decl_count; d++)
				hir_rn_decl(ast->decls[d], mod->name, exports, en);
			if (inlined < 64) {
				mod_prefix[inlined] = dupz(mod->name);
				mod_exports[inlined] = exports;
				mod_export_n[inlined] = en;
				inlined++;
			}
			continue;
		}

		HirDecl *ad = lower_decl_cst(dv);
		if (ad)
			ast->decls[ast->decl_count++] = ad;
	}

	/* Cross-module bare references: a module export rewritten to `<mod>_<name>`
	 * leaves bare references from elsewhere dangling. For each export whose bare
	 * name has no top-level definition in the final program, rewrite bare refs to
	 * it into the prefixed name; collisions with an existing top-level name (e.g.
	 * a core function) are left alone so bare names keep their current meaning. */
	for (int m = 0; m < inlined; m++) {
		char *dangling[256];
		int dn = 0;
		for (int s = 0; s < mod_export_n[m] && dn < 256; s++) {
			int defined = 0;
			for (int d = 0; d < ast->decl_count; d++) {
				const char *nm = hir_decl_name(ast->decls[d]);
				if (nm && strcmp(nm, mod_exports[m][s]) == 0) {
					defined = 1;
					break;
				}
			}
			if (!defined)
				dangling[dn++] = mod_exports[m][s];
		}
		if (dn > 0)
			for (int d = 0; d < ast->decl_count; d++)
				hir_rn_decl(ast->decls[d], mod_prefix[m], dangling, dn);
	}
	for (int m = 0; m < inlined; m++) {
		for (int s = 0; s < mod_export_n[m]; s++)
			free(mod_exports[m][s]);
		free(mod_exports[m]);
		free(mod_prefix[m]);
	}
	/* Collapse nested tuple-field accesses (`arch.pos.x` → `arch.pos_x`) to match the
	 * flattened archetype columns; no-op when no tuple groups are declared. */
	if (g_tgroup_count > 0)
		for (int i = 0; i < ast->decl_count; i++)
			tuple_collapse_decl(ast->decls[i]);
	return ast;
}

/* arche-analyzer — editor analysis service for the arche language server.
 *
 * Runs the compiler's real front-end (prepend core.arche, parse, resolve `use`
 * modules, semantic analysis — exactly as main.c does) and emits an "explicit
 * view" of the program: the implicit information the compiler infers, anchored
 * to source positions, for the editor to render as inlay hints (and, later,
 * diagnostics). The analyzer is the single source of truth — the syntax tree drives
 * syntactic facts via typed views, the node-id-keyed SemModel drives inferred
 * facts — so the output tracks the language as it evolves.
 *
 * Output lines (positions are 1-based, translated to USER-file coordinates):
 *   SYN <line> <col> <padL> <padR> <kind> <text...>                                inlay hint
 *   DIAG <line> <col> <severity> <code> <slug> <note_count> <msg...>               diagnostic
 *   NOTE <line> <col> <msg...>                                                     × note_count after DIAG
 *   DOC <line> <col> <name> <linecount>                                            doc comment (hover)
 *   DOCLINE <text...>                                                              × linecount after DOC
 *
 * Modes:
 *   --dump [file]   one-shot: analyze file (or stdin) and print all lines.
 *   --serve         persistent: warm per-document analysis over a line protocol
 *                   (UPDATE / TOKENS / HINTS / CLOSE), driven by the LSP server.
 *
 * core.arche is prepended before analysis, so every emitted position is in
 * combined (core+user) coordinates; we subtract the core line count and drop
 * anything inside the core region so the editor sees its own buffer's lines.
 */
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "semantic/sem_decls.h"
#include "semantic/sem_diagnostics.h"
#include "semantic/semantic.h"
#include "syntax/syntax_tree.h"
#include "syntax/syntax_view.h"
#include "syntax/token_category.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef ARCHE_CORE_DIR
#define ARCHE_CORE_DIR "core"
#endif
#ifndef ARCHE_STDLIB_DIR
#define ARCHE_STDLIB_DIR "stdlib"
#endif

/* C99 doesn't expose strdup; declare it explicitly (as codegen.c / sem_model.c do). */
char *strdup(const char *s);

/* ---- small file helpers (mirrors main.c; analysis-only, no lower/codegen) ---- */

static char *read_stream(FILE *f) {
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	if (!buf)
		return NULL;
	size_t n;
	while ((n = fread(buf + len, 1, cap - len, f)) > 0) {
		len += n;
		if (len == cap) {
			cap *= 2;
			char *grown = realloc(buf, cap);
			if (!grown) {
				free(buf);
				return NULL;
			}
			buf = grown;
		}
	}
	buf[len] = '\0';
	return buf;
}

static char *read_file(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	char *s = read_stream(f);
	fclose(f);
	return s;
}

static int file_exists(const char *path) {
	struct stat sb;
	return stat(path, &sb) == 0;
}

static char *source_dir_of(const char *path) {
	const char *slash = strrchr(path, '/');
	if (!slash) {
		char *d = malloc(2);
		strcpy(d, ".");
		return d;
	}
	int len = (int)(slash - path);
	char *d = malloc(len + 1);
	memcpy(d, path, len);
	d[len] = '\0';
	return d;
}

static int count_newlines(const char *s) {
	int n = 0;
	for (; *s; s++)
		if (*s == '\n')
			n++;
	return n;
}

/* ---- module loading: register `use` modules with semantic only ---- */

typedef struct {
	SyntaxNode *root;
	char *src;
} ModuleHold;

typedef struct {
	ModuleHold *items;
	int count, cap;
} ModuleHolds;

static void holds_push(ModuleHolds *h, SyntaxNode *root, char *src) {
	if (h->count >= h->cap) {
		h->cap = h->cap ? h->cap * 2 : 4;
		h->items = realloc(h->items, (size_t)h->cap * sizeof(ModuleHold));
	}
	h->items[h->count].root = root;
	h->items[h->count].src = src;
	h->count++;
}

static void holds_free(ModuleHolds *h) {
	for (int i = 0; i < h->count; i++) {
		syntax_node_free(h->items[i].root);
		free(h->items[i].src);
	}
	free(h->items);
}

/* Diagnostics found before the SemanticContext exists (parse errors, module-load
 * failures) buffer here, then get flushed into the ctx once it's created. Single
 * string arg suffices for the kinds we surface: parse_error carries the parser's
 * message; module_* carries the module name. */
typedef struct {
	SemDiagKind kind;
	SourceLoc loc;
	char *str; /* owned */
} DeferredDiag;

typedef struct {
	DeferredDiag *items;
	int count, cap;
} DeferredDiags;

static void deferred_push(DeferredDiags *d, SemDiagKind kind, SourceLoc loc, const char *str) {
	if (d->count >= d->cap) {
		d->cap = d->cap ? d->cap * 2 : 8;
		d->items = realloc(d->items, (size_t)d->cap * sizeof(DeferredDiag));
	}
	d->items[d->count].kind = kind;
	d->items[d->count].loc = loc;
	d->items[d->count].str = str ? strdup(str) : NULL;
	d->count++;
}

static void deferred_free(DeferredDiags *d) {
	for (int i = 0; i < d->count; i++)
		free(d->items[i].str);
	free(d->items);
	d->items = NULL;
	d->count = d->cap = 0;
}

/* Parse each `use <name>;` module and register its syntax tree with the semantic analyzer
 * (which inlines + name-prefixes it). Analysis-only: unlike main.c's resolve_uses
 * we don't touch the lowerer. The module syntax tree + source are borrowed by the registry,
 * so we keep them alive in `holds` until after analysis. */

/* Dedup set of already-loaded modules (per analyze call; reset in resolve_uses_sem). Marking a
 * name before load also makes transitive `#import` cycle-safe. Mirrors compile/compile.c. */
#define SEM_MAX_LOADED_MODS 256
static char *g_sem_loaded_mods[SEM_MAX_LOADED_MODS];
static int g_sem_loaded_count;

static void sem_load_module(const char *name, const char *source_dir, ModuleHolds *holds);
static void sem_load_module_from_path(const char *pathstr, const char *source_dir, ModuleHolds *holds);

/* Derive the module name an `#import` element resolves to: IDENT = the name verbatim (device by name);
 * STRING = a path (module by path), name = basename minus a trailing `.arche`. Returns 1 if it was a
 * STRING path, 0 if IDENT, -1 for any other token. `raw` (caller buf) gets the path text for STRING. */
static int sem_import_token(const char *src, const SyntaxElem *tok, char *name, size_t ncap, char *raw, size_t rcap) {
	if (tok->tag != SE_TOKEN)
		return -1;
	TokenKind k = tok->as.token.kind;
	if (k != TOK_IDENT && k != TOK_STRING)
		return -1;
	size_t off = tok->as.token.offset, len = tok->as.token.length;
	if (k == TOK_STRING && len >= 2) {
		off += 1;
		len -= 2;
	}
	if (len > rcap - 1)
		len = rcap - 1;
	memcpy(raw, src + off, len);
	raw[len] = '\0';
	if (k == TOK_IDENT) {
		snprintf(name, ncap, "%s", raw);
		return 0;
	}
	const char *slash = strrchr(raw, '/');
	const char *base = slash ? slash + 1 : raw;
	snprintf(name, ncap, "%s", base);
	size_t bl = strlen(name);
	if (bl > 6 && strcmp(name + bl - 6, ".arche") == 0)
		name[bl - 6] = '\0';
	return 1;
}

/* Parse one module file, register its syntax tree with the semantic registry (which borrows the syntax tree +
 * source — kept alive in `holds`), then recurse into the module's own `#import`s. */
static int sem_register_module_file(const char *mod_name, const char *path, const char *source_dir, ModuleHolds *holds,
                                    DeclOrigin origin) {
	char *mod_src = read_file(path);
	if (!mod_src)
		return 0;
	ParseResult mp = parse_source(mod_src);
	if (mp.error_count > 0 || !mp.syntax_root) {
		parse_result_free(&mp);
		free(mod_src);
		return 0;
	}
	semantic_add_module(mod_name, mp.syntax_root, mod_src, path, origin);
	const SyntaxNode *root = mp.syntax_root;
	const char *src = mod_src;
	holds_push(holds, mp.syntax_root, mod_src); /* keep alive; registry borrows */
	mp.syntax_root = NULL;
	parse_result_free(&mp);
	for (int u = 0; u < root->child_count; u++) {
		if (root->children[u].tag != SE_NODE || root->children[u].as.node->kind != SN_USE_DECL)
			continue;
		const SyntaxNode *ud = root->children[u].as.node;
		for (int k = 0; k < ud->child_count; k++) {
			char dep[256], raw[512];
			int kind = sem_import_token(src, &ud->children[k], dep, sizeof(dep), raw, sizeof(raw));
			if (kind < 0)
				continue;
			if (kind == 1)
				sem_load_module_from_path(raw, source_dir, holds); /* STRING = path import */
			else
				sem_load_module(dep, source_dir, holds); /* IDENT = device by name */
		}
	}
	return 1;
}

/* A module is a FOLDER `<dir>/<name>/` of `.arche` files merged into one namespace; falls back to a
 * single file `<dir>/<name>.arche`. Returns files registered. */
static int sem_try_load_module_dir(const char *mod_name, const char *dir, const char *source_dir, ModuleHolds *holds,
                                   DeclOrigin origin) {
	char folder[640];
	snprintf(folder, sizeof(folder), "%s/%s", dir, mod_name);
	DIR *d = opendir(folder);
	if (d) {
		int n = 0;
		struct dirent *ent;
		while ((ent = readdir(d)) != NULL) {
			size_t L = strlen(ent->d_name);
			if (L > 6 && strcmp(ent->d_name + L - 6, ".arche") == 0) {
				char fp[1300];
				snprintf(fp, sizeof(fp), "%s/%s", folder, ent->d_name);
				n += sem_register_module_file(mod_name, fp, source_dir, holds, origin);
			}
		}
		closedir(d);
		if (n > 0)
			return n;
	}
	char fp[640];
	snprintf(fp, sizeof(fp), "%s/%s.arche", dir, mod_name);
	if (file_exists(fp))
		return sem_register_module_file(mod_name, fp, source_dir, holds, origin);
	return 0;
}

/* Load module `name` (dedup'd) by searching the source dir, then stdlib, then core. The matched search
 * root determines the unit's provenance (DeclOrigin) — correct by construction, no path-sniffing. */
static void sem_load_module(const char *name, const char *source_dir, ModuleHolds *holds) {
	for (int i = 0; i < g_sem_loaded_count; i++)
		if (strcmp(g_sem_loaded_mods[i], name) == 0)
			return;
	if (g_sem_loaded_count < SEM_MAX_LOADED_MODS)
		g_sem_loaded_mods[g_sem_loaded_count++] = strdup(name); /* mark before load → cycle-safe */
	int loaded = sem_try_load_module_dir(name, source_dir, source_dir, holds, DECL_ORIGIN_USER_MODULE);
	if (!loaded)
		loaded = sem_try_load_module_dir(name, ARCHE_STDLIB_DIR, source_dir, holds, DECL_ORIGIN_STDLIB);
	if (!loaded)
		loaded = sem_try_load_module_dir(name, ARCHE_CORE_DIR, source_dir, holds, DECL_ORIGIN_CORE);
	(void)loaded; /* not-found is reported per import-site by the caller below */
}

/* Load a plain MODULE imported by PATH (`#import { "./util" }`): resolve relative to the importer's
 * dir; module name = basename sans `.arche`. Mirror of compile.c's load_module_from_path. */
static void sem_load_module_from_path(const char *pathstr, const char *source_dir, ModuleHolds *holds) {
	const char *slash = strrchr(pathstr, '/');
	const char *base = slash ? slash + 1 : pathstr;
	char subdir[512];
	if (slash) {
		size_t dl = (size_t)(slash - pathstr);
		if (dl > sizeof(subdir) - 1)
			dl = sizeof(subdir) - 1;
		memcpy(subdir, pathstr, dl);
		subdir[dl] = '\0';
	} else {
		subdir[0] = '\0';
	}
	char mod_name[256];
	snprintf(mod_name, sizeof(mod_name), "%s", base);
	size_t ml = strlen(mod_name);
	if (ml > 6 && strcmp(mod_name + ml - 6, ".arche") == 0)
		mod_name[ml - 6] = '\0';
	if (mod_name[0] == '\0')
		return;
	for (int i = 0; i < g_sem_loaded_count; i++)
		if (strcmp(g_sem_loaded_mods[i], mod_name) == 0)
			return;
	if (g_sem_loaded_count < SEM_MAX_LOADED_MODS)
		g_sem_loaded_mods[g_sem_loaded_count++] = strdup(mod_name);
	char dir[800];
	if (subdir[0])
		snprintf(dir, sizeof(dir), "%s/%s", source_dir, subdir);
	else
		snprintf(dir, sizeof(dir), "%s", source_dir);
	sem_try_load_module_dir(mod_name, dir, dir, holds, DECL_ORIGIN_USER_MODULE); /* path import = user's tree */
}

/* The open document may be a member file of a DEVICE folder (`<dir>/<name>/…` with a `.ds.arche`
 * datasheet). The compiler only ever sees such a file as part of the whole device (loaded via an
 * `#import`), so its datasheet's global type vocabulary is in scope; but the editor analyzes one file
 * in isolation, where nothing pulls the datasheet in. Register the sibling datasheet(s) under the
 * folder's module name and ask semantic to inline that module into the root, so the open impl file's
 * bare references to device types (`fd`, …) resolve exactly as they do in a full build. Skips when the
 * open file IS the datasheet (it already defines those types) or the folder has no datasheet. */
static void register_self_device(const char *path, const char *source_dir, ModuleHolds *holds) {
	const char *slash = strrchr(source_dir, '/');
	const char *mod_name = slash ? slash + 1 : source_dir;
	if (!mod_name[0] || strcmp(mod_name, ".") == 0)
		return;
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	DIR *d = opendir(source_dir);
	if (!d)
		return;
	int registered = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		size_t L = strlen(ent->d_name);
		if (L < 9 || strcmp(ent->d_name + L - 9, ".ds.arche") != 0)
			continue;
		if (strcmp(ent->d_name, base) == 0) /* the open file itself — its decls are already in the root */
			continue;
		char fp[1300];
		snprintf(fp, sizeof(fp), "%s/%s", source_dir, ent->d_name);
		registered += sem_register_module_file(mod_name, fp, source_dir, holds, DECL_ORIGIN_USER_MODULE);
	}
	closedir(d);
	if (registered)
		semantic_set_extra_inline_module(mod_name);
}

static void resolve_uses_sem(const SyntaxNode *root, const char *src, const char *path, ModuleHolds *holds,
                             DeferredDiags *diags) {
	if (!root)
		return;
	/* Clear leftovers from a prior analysis (--serve reuses the process). */
	semantic_reset_modules();
	for (int i = 0; i < g_sem_loaded_count; i++)
		free(g_sem_loaded_mods[i]);
	g_sem_loaded_count = 0;

	char *source_dir = source_dir_of(path);
	for (int u = 0; u < root->child_count; u++) {
		if (root->children[u].tag != SE_NODE || root->children[u].as.node->kind != SN_USE_DECL)
			continue;
		const SyntaxNode *ud = root->children[u].as.node;
		/* One element per import: IDENT = device by name, STRING = plain module by path. Load each. */
		for (int k = 0; k < ud->child_count; k++) {
			char mod_name[256], raw[512];
			int kind = sem_import_token(src, &ud->children[k], mod_name, sizeof(mod_name), raw, sizeof(raw));
			if (kind < 0)
				continue;
			SourceLoc use_loc = {ud->children[k].as.token.line, ud->children[k].as.token.column};
			int before = g_sem_loaded_count;
			if (kind == 1)
				sem_load_module_from_path(raw, source_dir, holds); /* STRING = path import */
			else
				sem_load_module(mod_name, source_dir, holds); /* IDENT = device by name */
			/* If the name was newly marked but nothing registered, it wasn't found. (A dedup hit — the
			 * module already loaded via another import — leaves `before` unchanged and is fine.) */
			if (g_sem_loaded_count > before && !semantic_has_module(mod_name))
				deferred_push(diags, SEM_DIAG_module_not_found, use_loc, mod_name);
		}
	}
	register_self_device(path, source_dir, holds);
	free(source_dir);
}

/* ---- explicit-view emission ---- */

static int g_core_lines; /* combined→user line translation: user_line = line - g_core_lines.
                          * 0 when core is NOT prepended (i.e. the document IS core.arche). */

/* core.arche is invariant across documents and edits, so read + measure it once.
 * This is the main saving that makes the warm server cheap: every analysis reuses
 * the cached prelude instead of re-reading 130+ lines from disk per keystroke. */
static char *g_core;
static int g_core_loaded;
static int g_core_newlines; /* real newline count of g_core; g_core_lines is the per-analysis offset */

static void ensure_core(void) {
	if (g_core_loaded)
		return;
	g_core_loaded = 1;
	char core_path[512];
	snprintf(core_path, sizeof(core_path), "%s/core.arche", ARCHE_CORE_DIR);
	g_core = read_file(core_path);
	g_core_newlines = (g_core && g_core[0]) ? count_newlines(g_core) : 0;
	g_core_lines = g_core_newlines;
}

/* True when `path` resolves to core.arche itself, so prepending core would define
 * every prelude symbol twice. Compare by (device, inode) so absolute/relative/
 * symlinked editor paths all match the canonical prelude location. */
static int path_is_core(const char *path) {
	if (!path || !path[0])
		return 0;
	char core_path[512];
	snprintf(core_path, sizeof(core_path), "%s/core.arche", ARCHE_CORE_DIR);
	struct stat sp, sc;
	if (stat(path, &sp) != 0 || stat(core_path, &sc) != 0)
		return 0;
	return sp.st_dev == sc.st_dev && sp.st_ino == sc.st_ino;
}

/* Render an internal type name as it would be written in arche source (longhand). */
static const char *display_type(const char *ty) {
	if (strcmp(ty, "char_array") == 0 || strcmp(ty, "str") == 0)
		return "char[]"; /* the interned string prim displays as "str"; the inlay hint shows char[] */
	return ty;
}

/* Emit a SYN line, translating combined→user coords and dropping the core region.
 * Format: SYN <line> <col> <padL> <padR> <kind> <text>. Position is the column the
 * virtual text anchors at; padL/padR are render-time spaces around the label, so
 * the result reads as the longhand of the surrounding real tokens. */
static void emit_syn(int line, int col, int padL, int padR, const char *kind, const char *text) {
	int uline = line - g_core_lines;
	if (uline <= 0)
		return; /* inside the prepended core region */
	printf("SYN %d %d %d %d %s %s\n", uline, col, padL ? 1 : 0, padR ? 1 : 0, kind, text);
}

/* Off by default — also render the redundant FORM-type inlays. A presentation choice the editor makes
 * per the user's inlay setting (HINTS full / --dump --full), never a compile concern. */
static int g_full_type_hints = 0;

/* A FORM type (func/proc/sys/policy/archetype) is already spelled out by the source form, so its inlay
 * is redundant; a value type's `⟨type⟩` is hidden in source, so it's the useful one. The redundancy is
 * decided by the TYPE, not the node kind (the parser makes `add :: func(){}` an `SN_CONST_DECL` too). */
static int type_is_form(const SemModel *model, SemanticContext *ctx, SyntaxView binding) {
	TypeId tid = sem_model_expr_type_id(model, sv_id(binding));
	TyKind k = tyid_kind(sem_context_arena(ctx), tid);
	return k == TYK_FUNC || k == TYK_PROC || k == TYK_SYS || k == TYK_POLICY || k == TYK_ARCHETYPE_CATEGORY;
}

/* The inferred-type inlay: fill the elided `⟨type⟩` slot of the unified grammar so the view reads as
 * the longhand —
 *   `r := e`  →  `r : T = e`   (anchor before the `=`)
 *   `x :: e`  →  `x : T : e`   (anchor before the 2nd `:`)
 * ONE rule for EVERY binding/declaration form (locals, consts, statics, func/proc/sys/policy/arche): the
 * compiler records `type-of(RHS)` keyed by the binding node id, and this renders it. A node shows a hint
 * iff the compiler typed it AND the source didn't already write the `⟨type⟩` slot — and, by default, the
 * type isn't a redundant form type (hidden unless full mode). Skips type aliases (emit_typeref_hint). */
static void emit_type_hint(SyntaxView binding, SemanticContext *ctx) {
	const SemModel *model = sem_context_model(ctx);
	if (sem_model_bind_alias(model, sv_id(binding)))
		return; /* a nominal type alias, not a value binding */
	if (sv_type_count(binding) > 0)
		return; /* the `⟨type⟩` slot is already written in source */
	TypeId tid = sem_model_expr_type_id(model, sv_id(binding));
	if (tid == TYID_UNKNOWN)
		return;
	if (!g_full_type_hints && type_is_form(model, ctx, binding))
		return; /* redundant: the form already states its type — hidden by default (still in the model) */
	char tybuf[128];
	const char *ty = tyid_display(sem_context_arena(ctx), tid, tybuf, sizeof(tybuf));
	if (!ty || !ty[0])
		return;
	CvPos anchor = sv_token_pos(binding, TOK_EQ); /* the `=` of `:=`, else the 2nd `:` of `::` */
	if (!anchor.line)
		anchor = sv_token_pos_at(binding, TOK_COLON, 1);
	if (!anchor.line)
		return;
	emit_syn(anchor.line, anchor.column, 1, 1, "type", display_type(ty));
}

/* A type position naming an alias shows its backing type, e.g. `count (int)`. */
static void emit_typeref_hint(SyntaxView tr, SemanticContext *ctx) {
	SynText id = sv_token(tr, TOK_IDENT);
	if (!id.ptr)
		return;
	char name[256];
	size_t L = id.len < sizeof(name) - 1 ? id.len : sizeof(name) - 1;
	memcpy(name, id.ptr, L);
	name[L] = '\0';
	const char *backing = semantic_resolve_type_alias(ctx, name);
	if (!backing || strcmp(backing, name) == 0)
		return; /* not an alias (or already its own backing) */
	if (semantic_alias_is_transparent(ctx, name))
		return; /* transparent (tier-1) alias — the name IS the backing, no distinction to show */
	char text[260];
	snprintf(text, sizeof(text), "(%s)", display_type(backing));
	/* Annotation after the alias name: pad-left only (space between name and `(`). */
	CvPos end = sv_last_token_pos(tr);
	if (!end.line)
		return;
	emit_syn(end.line, end.column + (int)end.length, 1, 0, "alias", text);
}

/* Call-site parameter hint (gopls-style): show the resolved parameter's name
 * before the argument, prefixed `own ` when it takes ownership. Recorded by
 * semantic analysis (needs call resolution), keyed by the argument's node. */
static void emit_param_hint(SyntaxView arg, SemanticContext *ctx) {
	const SemHints *h = sem_context_hints(ctx);
	const char *name = sem_hints_param_name(h, sv_id(arg));
	if (!name)
		return;
	char text[300];
	snprintf(text, sizeof(text), "%s%s:", sem_hints_param_is_own(h, sv_id(arg)) ? "own " : "", name);
	/* Anchor at the argument's start: pad-right only (space between `name:` and arg). */
	CvPos start = sv_first_token_pos(arg);
	if (!start.line)
		return;
	emit_syn(start.line, start.column, 0, 1, "param", text);
}

/* Elided-`move` hint: a bare move-only name (array/slice or opaque) in an ownership-taking position
 * (bind/assign RHS, `own`-param arg) is an implicit move. Show a ghost `move` before it so the
 * consumed-here transfer is visible. Recorded by semantic analysis, keyed by the name node. */
static void emit_move_hint(SyntaxView node, SemanticContext *ctx) {
	const SemHints *h = sem_context_hints(ctx);
	if (!sem_hints_is_elided_move(h, sv_id(node)))
		return;
	CvPos start = sv_first_token_pos(node);
	if (!start.line)
		return;
	emit_syn(start.line, start.column, 0, 1, "move", "move");
}

/* Proc names gathered from the syntax tree (combined core+user). A proc is an SN_PROC_DECL node, so this is
 * a purely syntactic fact — no whole-program query needed. Borrowed slices into the analysis source,
 * which outlives the walk. Reset per emit_hints; the buffer is reused (grows once). */
static SynText *g_proc_names;
static int g_proc_name_count, g_proc_name_cap;

static void collect_proc_names(SyntaxView v) {
	if (!v.node)
		return;
	/* Unified grammar: a proc is `name :: proc(...)…` — an SN_CONST_DECL with an SN_PROC_EXPR RHS.
	 * The name is the IDENT immediately before the first top-level `:` (skips `@decorator` idents). */
	int is_unified_proc = 0;
	if (sv_kind(v) == SN_CONST_DECL) {
		for (int i = 0; i < v.node->child_count; i++)
			if (v.node->children[i].tag == SE_NODE && v.node->children[i].as.node->kind == SN_PROC_EXPR) {
				is_unified_proc = 1;
				break;
			}
	}
	if (is_unified_proc) {
		SynText nm = {NULL, 0};
		for (int i = 0; i < v.node->child_count; i++) {
			SyntaxElem *e = &v.node->children[i];
			if (e->tag != SE_TOKEN)
				continue;
			if (e->as.token.kind == TOK_COLON)
				break;
			if (e->as.token.kind == TOK_IDENT)
				nm = (SynText){v.src + e->as.token.offset, e->as.token.length};
		}
		if (nm.ptr) {
			if (g_proc_name_count == g_proc_name_cap) {
				g_proc_name_cap = g_proc_name_cap ? g_proc_name_cap * 2 : 32;
				g_proc_names = realloc(g_proc_names, (size_t)g_proc_name_cap * sizeof(SynText));
			}
			g_proc_names[g_proc_name_count++] = nm;
		}
	}
	if (sv_kind(v) == SN_PROC_DECL) {
		SynText nm = sv_text(sv_child(v, SN_FUNC_DEF_NAME));
		if (nm.ptr) {
			if (g_proc_name_count == g_proc_name_cap) {
				g_proc_name_cap = g_proc_name_cap ? g_proc_name_cap * 2 : 32;
				g_proc_names = realloc(g_proc_names, (size_t)g_proc_name_cap * sizeof(SynText));
			}
			g_proc_names[g_proc_name_count++] = nm;
		}
	}
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE)
			collect_proc_names((SyntaxView){v.node->children[i].as.node, v.src});
}

static int name_is_proc(SynText n) {
	for (int i = 0; i < g_proc_name_count; i++)
		if (g_proc_names[i].len == n.len && memcmp(g_proc_names[i].ptr, n.ptr, n.len) == 0)
			return 1;
	return 0;
}

/* A bare proc/extern call statement (`printf(x);`) omits its out-list — that omission means "no
 * captured results", but the call is still an ACTION. Render a ghost `()` right after it so a reader
 * sees it may have effects. The capture form `f(x)(out)` is an SN_PROC_CALL_STMT (not an
 * SN_EXPR_STMT, so not reached here) and already shows its out-list. */
static void emit_effect_hint(SyntaxView expr_stmt) {
	SyntaxView call = sv_child(expr_stmt, SN_CALL_EXPR);
	if (!call.node)
		return;
	SynText callee = sv_text(sv_child(call, SN_CALLEE_NAME));
	if (!callee.ptr || !name_is_proc(callee))
		return;
	CvPos end = sv_last_token_pos(call);
	if (!end.line)
		return;
	emit_syn(end.line, end.column + (int)end.length, 0, 0, "effect", "()");
}

/* Inlay the IMPLICIT failure policy at an index/slice op: the ghost default (`!abort` in a proc,
 * `!clamp` in a func) right after the op. This is a PURE PROJECTION of the bounds prover's verdict
 * (`sem_model_policy_elided`) — the SAME bit codegen reads to elide the policy macro — so the hint
 * shows exactly when a runtime policy actually applies. No provability logic of its own:
 *   - explicit `!name`        → already in source, no ghost
 *   - prover proved in-bounds → policy elided (incl. constant-safe indices), no ghost
 *   - otherwise               → a policy applies, render its ghost. */
static void emit_policy_hint(SyntaxView op, SemanticContext *ctx, int in_func) {
	if (sv_present(sv_child(op, SN_POLICY_REF)))
		return; /* explicit policy already visible in source */
	if (sem_model_policy_elided(sem_context_model(ctx), sv_id(op)))
		return; /* prover proved it in-bounds — no policy applies (same verdict codegen elides on) */
	CvPos end = sv_last_token_pos(op);
	if (!end.line)
		return;
	emit_syn(end.line, end.column + (int)end.length, 0, 0, "policy", in_func ? "!clamp" : "!abort");
}

static void walk(SyntaxView v, SemanticContext *ctx, int in_func) {
	if (!v.node)
		return;
	emit_param_hint(v, ctx); /* any node may be a resolved call argument */
	emit_move_hint(v, ctx);  /* any name may be an implicitly-moved (consumed) bare binding */
	/* Every binding/declaration form gets the same elided-`⟨type⟩` inlay (keyed by node id); whether a
	 * redundant form type is shown is decided inside emit_type_hint. The parser makes proc/func/sys/arche
	 * decls `SN_CONST_DECL` (value = a *_EXPR form), so the value-binding kinds cover them too. */
	SyntaxNodeKind vk = sv_kind(v);
	if (vk == SN_BIND_STMT || vk == SN_CONST_DECL || vk == SN_STATIC_DECL || vk == SN_FUNC_DECL ||
	    vk == SN_PROC_DECL || vk == SN_SYS_DECL || vk == SN_ARCHETYPE_DECL)
		emit_type_hint(v, ctx);
	if (sv_kind(v) == SN_TYPE_REF)
		emit_typeref_hint(v, ctx);
	else if (sv_kind(v) == SN_EXPR_STMT)
		emit_effect_hint(v); /* bare proc call → ghost `()` */
	else if (sv_kind(v) == SN_INDEX_EXPR || sv_kind(v) == SN_SLICE_EXPR)
		emit_policy_hint(v, ctx, in_func); /* implicit failure policy → ghost `!abort`/`!clamp` */
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			SyntaxView c = {v.node->children[i].as.node, v.src};
			/* a func/policy body is total (default !clamp); a proc/sys body defaults to !abort */
			SyntaxNodeKind ck = sv_kind(c);
			int child_in_func = in_func;
			if (ck == SN_FUNC_EXPR || ck == SN_POLICY_EXPR)
				child_in_func = 1;
			else if (ck == SN_PROC_EXPR || ck == SN_SYS_EXPR)
				child_in_func = 0;
			walk(c, ctx, child_in_func);
		}
}

/* ---- analysis unit (shared by --dump and --serve) ---- */

typedef struct {
	char *combined;          /* core + user buffer; owned */
	SyntaxNode *syntax_root; /* owned */
	SemanticContext *ctx;    /* owned; NULL on failure */
	ModuleHolds holds;       /* owned module syntax trees + sources */
} Analysis;

/* Analyze a user buffer (takes ownership of `user`), mirroring main.c's front-end:
 * prepend cached core, parse, resolve `use` modules (semantic-only), analyze. */
static Analysis analyze(char *user, const char *path) {
	Analysis a = {NULL, NULL, NULL, {NULL, 0, 0}};
	ensure_core();
	/* Don't prepend core to core itself — that double-defines every prelude symbol.
	 * When the document IS core.arche, analyze it bare and zero the line offset. */
	int prepend = g_core && g_core[0] && !path_is_core(path);
	g_core_lines = prepend ? g_core_newlines : 0;
	if (prepend) {
		size_t cl = strlen(g_core), ul = strlen(user);
		a.combined = malloc(cl + ul + 1);
		memcpy(a.combined, g_core, cl);
		memcpy(a.combined + cl, user, ul + 1);
		free(user);
	} else {
		a.combined = user; /* ownership moved */
	}
	ParseResult pr = parse_source(a.combined);
	a.syntax_root = pr.syntax_root;
	pr.syntax_root = NULL;

	/* Capture parse errors before freeing pr — they get pushed into ctx below so
	 * the editor sees them through the same channel as semantic errors. This is
	 * the single biggest live-typing coverage win: half-typed code (which always
	 * has parse errors) now produces diagnostics instead of silence. */
	DeferredDiags deferred = {NULL, 0, 0};
	for (size_t i = 0; i < pr.error_count; i++) {
		SourceLoc loc = {pr.errors[i].line, pr.errors[i].column};
		deferred_push(&deferred, SEM_DIAG_parse_error, loc, pr.errors[i].message);
	}
	parse_result_free(&pr);

	if (!a.syntax_root) {
		/* No syntax tree → no semantic analysis; parse errors are all we have. The caller
		 * gets an analysis with ctx=NULL, but we created it for the deferred-emit
		 * path: emit parse errors before returning so they're not lost. */
		if (deferred.count) {
			/* We need a ctx to push into; analyze() returns ctx=NULL when there's
			 * no syntax tree. For now drop these parse errors when syntax tree is null — the user
			 * sees stderr from the parser already. Future: build a fallback ctx. */
		}
		deferred_free(&deferred);
		return a;
	}

	resolve_uses_sem(a.syntax_root, a.combined, path && path[0] ? path : ".", &a.holds, &deferred);
	a.ctx = semantic_analyze_cst(a.syntax_root, a.combined);

	/* Flush deferred diagnostics now that the ctx exists. The typed wrappers do
	 * the byte-stable stderr print + diag_push, so editors and CLI both see them. */
	if (a.ctx) {
		for (int i = 0; i < deferred.count; i++) {
			DeferredDiag *d = &deferred.items[i];
			switch (d->kind) {
			case SEM_DIAG_parse_error:
				sem_emit_parse_error(a.ctx, d->loc, d->str ? d->str : "");
				break;
			case SEM_DIAG_module_not_found:
				sem_emit_module_not_found(a.ctx, d->loc, d->str ? d->str : "");
				break;
			case SEM_DIAG_module_parse_failed:
				sem_emit_module_parse_failed(a.ctx, d->loc, d->str ? d->str : "");
				break;
			default:
				break;
			}
		}
	}
	deferred_free(&deferred);
	return a;
}

static void analysis_free(Analysis *a) {
	if (a->ctx)
		semantic_context_free(a->ctx);
	holds_free(&a->holds);
	syntax_node_free(a->syntax_root);
	free(a->combined);
	a->combined = NULL;
	a->syntax_root = NULL;
	a->ctx = NULL;
	a->holds = (ModuleHolds){NULL, 0, 0};
}

static void emit_hints(const Analysis *a, int full) {
	if (!a->ctx)
		return;
	g_full_type_hints = full;
	SyntaxView root = sv_root(a->syntax_root, a->combined);
	g_proc_name_count = 0;
	collect_proc_names(root); /* gather proc names first (a call may precede its decl) */
	walk(root, a->ctx, 0);
}

/* Emit diagnostics collected during semantic analysis, translated to user coords.
 * Errors with no source position default to user line 1 col 1 so they still surface;
 * anything inside the prepended core region is dropped (belongs to the prelude).
 *
 * Wire protocol (self-framing — robust to mid-emit interruptions in the warm server):
 *   DIAG <line> <col> <severity> <code> <slug> <note_count> <message…>
 *   NOTE <line> <col> <message…>     × note_count
 *
 * <code> is the stable identifier (e.g. "E0001"); editors should surface it as
 * LSP `Diagnostic.code`. `<note_count>` lets the parser know exactly how many
 * NOTE lines belong to this DIAG — no in-band terminator needed. A `code` of
 * "-" is emitted for legacy diagnostics that haven't been bound to a kind yet. */
static void emit_diags(const Analysis *a) {
	if (!a->ctx)
		return;
	int n = sem_diag_count(a->ctx);
	for (int i = 0; i < n; i++) {
		const SemDiag *d = sem_diag_at(a->ctx, i);
		if (!d)
			continue;
		int line = 1, col = 1;
		if (d->has_loc) {
			int uline = d->loc.line - g_core_lines;
			if (uline <= 0)
				continue; /* inside the core prelude */
			line = uline;
			col = d->loc.column;
		}
		printf("DIAG %d %d %s %s %s %d %s\n", line, col, d->severity ? "error" : "warning", d->code ? d->code : "-",
		       d->name, d->note_count, d->message);
		for (int j = 0; j < d->note_count; j++) {
			const SemDiagNote *nt = &d->notes[j];
			int nline = nt->loc.line - g_core_lines;
			if (nline <= 0)
				nline = 1;
			printf("NOTE %d %d %s\n", nline, nt->loc.column, nt->message);
		}
	}
}

/* Documentation comments, for editor hover. Self-framing like DIAG/NOTE:
 *   DOC <line> <col> <name> <linecount>
 *   DOCLINE <text…>                     × linecount
 * <line>/<col> point at the documented declaration's first token (user coords);
 * <name> is its identifier. Each DOCLINE is one `///` line with the marker and a
 * leading space stripped (see sv_decl_doc_lines). The editor joins them as the
 * hover body. Needs only the syntax tree, so it works even when analysis is partial. */
static void emit_docs(const Analysis *a) {
	if (!a->syntax_root)
		return;
	SyntaxView root = sv_root(a->syntax_root, a->combined);
	int nn = sv_node_count(root);
	for (int i = 0; i < nn; i++) {
		SyntaxView top = sv_node_at(root, i);
		/* Descend one level into a `#foreign`/`#module`/`#file` block region so docs on its
		 * inner decls still surface for hover. */
		int is_region = sv_kind(top) == SN_REGION;
		int inner = is_region ? sv_node_count(top) : 1;
		for (int j = 0; j < inner; j++) {
			SyntaxView decl = is_region ? sv_node_at(top, j) : top;
			SyntaxNodeKind k = sv_kind(decl);
			if (k < SN_WORLD_DECL || k > SN_USE_DECL)
				continue;

			SynText lines[256];
			int n = sv_decl_doc_lines(root, decl, lines, NULL, 256);
			if (n <= 0)
				continue;

			CvPos p = sv_first_token_pos(decl);
			int uline = p.line - g_core_lines;
			if (uline <= 0)
				continue; /* core region */

			SynText name = sv_text(sv_child(decl, SN_FUNC_DEF_NAME));
			if (!name.ptr)
				name = sv_text(sv_child(decl, SN_TYPE_DEF_NAME));
			printf("DOC %d %d %.*s %d\n", uline, p.column, name.ptr ? (int)name.len : 4, name.ptr ? name.ptr : "item",
			       n);
			for (int j2 = 0; j2 < n; j2++)
				printf("DOCLINE %.*s\n", (int)lines[j2].len, lines[j2].ptr);
		}
	}
}

/* Syntax-highlighting tokens, same `offset length line col CATEGORY` format the
 * editor already consumes — but served from the warm parse and translated to user
 * coordinates (core-region tokens dropped). Needs only the syntax tree, not analysis. */
static void walk_tokens(const SyntaxNode *node) {
	for (int i = 0; i < node->child_count; i++) {
		const SyntaxElem *e = &node->children[i];
		if (e->tag == SE_NODE) {
			walk_tokens(e->as.node);
		} else {
			const char *cat = arche_token_category(e->as.token.kind, node->kind);
			if (!cat)
				continue;
			int uline = e->as.token.line - g_core_lines;
			if (uline <= 0)
				continue; /* core region */
			printf("%u %u %d %d %s\n", e->as.token.offset, e->as.token.length, uline, e->as.token.column, cat);
		}
	}
}

static void emit_tokens(const Analysis *a) {
	if (a->syntax_root)
		walk_tokens(a->syntax_root);
}

/* ---- goto navigation (definition / type / implementation / declaration) ----
 *
 * The editor sends a cursor (USER line/col) and a kind; we run the SAME resolution the compiler
 * does — node-id-keyed SemModel DefId channels (callee_def/ref_def), the interned TypeId of an
 * expression, and the @drop registry — and emit the target site(s):
 *   LOC <line> <col> <path>      target location, 1-based, in the TARGET file's own coordinates
 * Zero LOC lines = unresolved. Multiple = several implementation sites. The blank-line terminator
 * is written by the caller (run_serve / run_goto). Cross-file by construction: a module/datasheet
 * decl reports its own file; a core decl reports core.arche; an entry decl reports the open path. */

typedef enum { GREF_NONE, GREF_CALL, GREF_REF, GREF_TYPE } GotoRefKind;

/* Byte offset of 1-based (line, col) in src; -1 if the line doesn't exist. col counts bytes, matching
 * the lexer's token columns. */
static long offset_of_linecol(const char *src, int line, int col) {
	int ln = 1;
	long i = 0;
	for (; src[i] && ln < line; i++)
		if (src[i] == '\n')
			ln++;
	if (ln != line)
		return -1;
	return i + (col > 0 ? col - 1 : 0);
}

/* Descend to the deepest node whose span contains byte offset `off`, recording the ancestor chain
 * (chain[0]=root … chain[n-1]=deepest). Returns the chain depth (capped at maxd). */
static int descend_to_offset(SyntaxView root, uint32_t off, SyntaxView *chain, int maxd) {
	int n = 0;
	SyntaxView cur = root;
	while (n < maxd) {
		chain[n++] = cur;
		SyntaxNode *next = NULL;
		for (int i = 0; i < cur.node->child_count; i++) {
			if (cur.node->children[i].tag != SE_NODE)
				continue;
			SyntaxNode *c = cur.node->children[i].as.node;
			if (off >= c->offset && off < c->offset + c->length) {
				next = c;
				break;
			}
		}
		if (!next)
			break;
		cur = (SyntaxView){next, cur.src};
	}
	return n;
}

/* The reference node the cursor is on: the nearest enclosing call / value-ref / type node, walking
 * the chain deepest-first so a call ARGUMENT (a ref) wins over its enclosing call, while a cursor on
 * the callee identifier (an SN_CALLEE_NAME leaf, which carries no resolution) falls through to the call. */
static SyntaxView pick_ref_node(SyntaxView *chain, int n, GotoRefKind *which) {
	for (int i = n - 1; i >= 0; i--) {
		SyntaxNodeKind k = sv_kind(chain[i]);
		if (k == SN_CALL_EXPR) {
			*which = GREF_CALL;
			return chain[i];
		}
		if (k == SN_NAME_EXPR || k == SN_FIELD_EXPR || k == SN_INDEX_EXPR || k == SN_SLICE_EXPR) {
			*which = GREF_REF;
			return chain[i];
		}
		if (k >= SN_TYPE_REF && k <= SN_TYPE_FUNC) {
			*which = GREF_TYPE;
			return chain[i];
		}
	}
	*which = GREF_NONE;
	return (SyntaxView){NULL, NULL};
}

/* Emit one LOC for resolved decl `idx`, mapping its file + line into TARGET-file coordinates:
 *   module/datasheet decl → its own file, line as-is (parsed bare, no core prepend);
 *   core decl (entry buffer, in the prepended region) → core.arche, line as-is;
 *   entry decl → the open document, line shifted down past the core region. */
static void goto_emit_decl(const Analysis *a, const char *path, int idx) {
	const DeclSummary *d = semantic_decl_at(a->ctx, idx);
	if (!d || d->loc.line <= 0)
		return;
	int line = d->loc.line, col = d->loc.column;
	const char *file = semantic_decl_src_file(a->ctx, idx);
	if (file) {
		printf("LOC %d %d %s\n", line, col, file);
	} else if (line <= g_core_lines) {
		char core_path[512];
		snprintf(core_path, sizeof(core_path), "%s/core.arche", ARCHE_CORE_DIR);
		printf("LOC %d %d %s\n", line, col, core_path);
	} else {
		printf("LOC %d %d %s\n", line - g_core_lines, col, path);
	}
}

/* Index of a decl named `name` matching `kind_is_datasheet` (the .ds.arche declaration site), else -1. */
static int datasheet_decl_named(const Analysis *a, const char *name) {
	if (!name)
		return -1;
	for (int i = 0; i < semantic_decl_count(a->ctx); i++) {
		const DeclSummary *d = semantic_decl_at(a->ctx, i);
		if (d->is_datasheet && d->name && strcmp(d->name, name) == 0)
			return i;
	}
	return -1;
}

/* The interned TypeId at the reference: the type node itself (GREF_TYPE) or an expression's type. */
static TypeId goto_ref_type(const Analysis *a, SyntaxView ref, GotoRefKind which) {
	if (which == GREF_TYPE)
		return sem_intern_view(a->ctx, ref);
	return sem_model_expr_type_id(sem_context_model(a->ctx), sv_id(ref));
}

/* The type NAME a TypeId denotes (nominal / handle archetype / backing of a distinct subtype), else NULL. */
static const char *goto_type_name(const Analysis *a, TypeId t) {
	if (t == TYID_UNKNOWN)
		return NULL;
	TypeArena *arena = sem_context_arena(a->ctx);
	const char *tn = tyid_nominal_name(arena, t);
	if (!tn)
		tn = tyid_handle_name(arena, t);
	if (!tn) {
		TypeId b = tyid_backing(arena, t);
		if (b != TYID_UNKNOWN)
			tn = tyid_nominal_name(arena, b);
	}
	return tn;
}

/* The entry-buffer (open-file) decl whose span tightest-contains byte offset `off`, else -1. Used to
 * scope local-variable lookups to the enclosing proc/func. */
static int enclosing_decl_index(const Analysis *a, uint32_t off) {
	int best = -1;
	uint32_t best_len = 0xffffffffu;
	for (int i = 0; i < semantic_decl_count(a->ctx); i++) {
		const DeclSummary *d = semantic_decl_at(a->ctx, i);
		if (!d->node.node || d->node.src != a->combined)
			continue; /* only the open file's own decls hold its locals */
		uint32_t o = d->node.node->offset, l = d->node.node->length;
		if (off >= o && off < o + l && l < best_len) {
			best = i;
			best_len = l;
		}
	}
	return best;
}

/* Recursively find, within `v`, the binding of `name` whose target sits NEAREST before `ref_off`
 * (shadowing-correct enough for goto: the closest preceding `:=`/`::` wins). Writes into *best. */
static void scope_find_bind(SyntaxView v, const char *name, size_t nlen, uint32_t ref_off, CvPos *best) {
	if (!v.node)
		return;
	if (sv_kind(v) == SN_BIND_STMT) {
		SyntaxView tgt = sv_expr_at(v, 0);
		if (sv_present(tgt) && sv_kind(tgt) == SN_NAME_EXPR) {
			SynText t = sv_token(tgt, TOK_IDENT);
			CvPos p = sv_first_token_pos(tgt);
			if (t.ptr && t.len == nlen && memcmp(t.ptr, name, nlen) == 0 && p.line && p.offset <= ref_off &&
			    (!best->line || p.offset > best->offset))
				*best = p;
		}
	}
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE)
			scope_find_bind((SyntaxView){v.node->children[i].as.node, v.src}, name, nlen, ref_off, best);
}

/* Fallback when DefId resolution misses: the cursor is on a local or a parameter (the compiler keeps
 * no node→local edge). Walk the enclosing decl's params, then its body bindings. Returns 1 if emitted. */
static int goto_scope_walk(const Analysis *a, const char *path, SyntaxView ref, uint32_t ref_off) {
	SynText id = sv_token(ref, TOK_IDENT);
	if (!id.ptr)
		return 0;
	int di = enclosing_decl_index(a, ref_off);
	if (di < 0)
		return 0;
	const DeclSummary *d = semantic_decl_at(a->ctx, di);
	for (int i = 0; i < d->param_count; i++) {
		const char *pn = d->params[i].name;
		if (pn && strlen(pn) == id.len && memcmp(pn, id.ptr, id.len) == 0 && d->params[i].loc.line > g_core_lines) {
			printf("LOC %d %d %s\n", d->params[i].loc.line - g_core_lines, d->params[i].loc.column, path);
			return 1;
		}
	}
	CvPos best = {0, 0, 0, 0};
	scope_find_bind(d->body_node, id.ptr, id.len, ref_off, &best);
	if (best.line && best.line > g_core_lines) {
		printf("LOC %d %d %s\n", best.line - g_core_lines, best.column, path);
		return 1;
	}
	return 0;
}

static void emit_goto(const Analysis *a, const char *kind, int uline, int col, const char *path) {
	if (!a->ctx || !a->syntax_root)
		return;
	long off = offset_of_linecol(a->combined, uline + g_core_lines, col);
	if (off < 0)
		return;
	SyntaxView root = sv_root(a->syntax_root, a->combined);
	SyntaxView chain[64];
	int n = descend_to_offset(root, (uint32_t)off, chain, 64);
	GotoRefKind which;
	SyntaxView ref = pick_ref_node(chain, n, &which);
	if (which == GREF_NONE)
		return;

	/* type / impl resolve through the reference's TypeId. */
	if (strcmp(kind, "type") == 0) {
		int ti = semantic_find_type_decl_index(a->ctx, goto_type_name(a, goto_ref_type(a, ref, which)));
		if (ti >= 0)
			goto_emit_decl(a, path, ti);
		return;
	}
	if (strcmp(kind, "impl") == 0) {
		const char *tn = goto_type_name(a, goto_ref_type(a, ref, which));
		int dp = tn ? semantic_drop_proc_decl_index(a->ctx, tn) : -1;
		if (dp >= 0) {
			goto_emit_decl(a, path, dp);
			return;
		}
		int ti = tn ? semantic_find_type_decl_index(a->ctx, tn) : -1;
		if (ti >= 0)
			goto_emit_decl(a, path, ti);
		return;
	}

	int want_decl = strcmp(kind, "decl") == 0;

	/* A cursor on a type name (in any of def/decl) goes to that type's decl, preferring a datasheet
	 * declaration for `decl`. */
	if (which == GREF_TYPE) {
		int ti = semantic_find_type_decl_index(a->ctx, goto_type_name(a, goto_ref_type(a, ref, which)));
		if (ti < 0)
			return;
		const DeclSummary *d = semantic_decl_at(a->ctx, ti);
		int ds = want_decl ? datasheet_decl_named(a, d ? d->name : NULL) : -1;
		goto_emit_decl(a, path, ds >= 0 ? ds : ti);
		return;
	}

	/* Qualified reference (`foo.bar` / `foo.bar(x)`): the node's DefId resolves the WHOLE chain to its
	 * tail (`bar`), but the cursor may be on the leading segment `foo`. When it is, resolve `foo` itself —
	 * a module qualifier → the module's file; otherwise a local/var base → its binding. */
	if (sv_count(ref, SN_FIELD_NAME) >= 1) {
		CvPos base = sv_token_pos(ref, TOK_IDENT); /* the leftmost segment (the base), a direct token of ref */
		if (base.line && (uint32_t)off >= base.offset && (uint32_t)off < base.offset + base.length) {
			char bname[256];
			size_t bl = base.length < sizeof(bname) - 1 ? base.length : sizeof(bname) - 1;
			memcpy(bname, a->combined + base.offset, bl);
			bname[bl] = '\0';
			const char *mf = semantic_module_file(bname);
			if (mf) {
				printf("LOC 1 1 %s\n", mf); /* a module: jump to its file */
				return;
			}
			goto_scope_walk(a, path, ref, (uint32_t)off); /* a local struct/handle base → its binding */
			return;
		}
	}

	/* def / decl on a value reference: the authoritative DefId channel, else scope-walk for a local. */
	const SemModel *model = sem_context_model(a->ctx);
	DefId d = (which == GREF_CALL) ? sem_model_callee_def(model, sv_id(ref)) : sem_model_ref_def(model, sv_id(ref));
	if (!defid_is_none(d)) {
		if (want_decl) {
			const DeclSummary *dd = semantic_decl_at(a->ctx, d.index);
			int ds = datasheet_decl_named(a, dd ? dd->name : NULL);
			if (ds >= 0) {
				goto_emit_decl(a, path, ds);
				return;
			}
		}
		goto_emit_decl(a, path, d.index);
		return;
	}
	goto_scope_walk(a, path, ref, (uint32_t)off);
}

/* ---- one-shot dump ---- */

static int run_dump(const char *path, int full) {
	char *user = path ? read_file(path) : read_stream(stdin);
	if (!user) {
		fprintf(stderr, "arche-analyzer: could not read %s\n", path ? path : "<stdin>");
		return 1;
	}
	Analysis a = analyze(user, path);
	if (!a.syntax_root) {
		fprintf(stderr, "arche-analyzer: parse produced no syntax tree\n");
		analysis_free(&a);
		return 1;
	}
	emit_hints(&a, full);
	emit_diags(&a);
	emit_docs(&a);
	analysis_free(&a);
	return 0;
}

/* One-shot goto query (testing parity with --dump): analyze `path` and print LOC lines for the
 * cursor at 1-based (line, col). `kind` ∈ def|type|impl|decl. */
static int run_goto(const char *kind, int line, int col, const char *path) {
	char *user = read_file(path);
	if (!user) {
		fprintf(stderr, "arche-analyzer: could not read %s\n", path);
		return 1;
	}
	Analysis a = analyze(user, path);
	if (!a.syntax_root) {
		fprintf(stderr, "arche-analyzer: parse produced no syntax tree\n");
		analysis_free(&a);
		return 1;
	}
	emit_goto(&a, kind, line, col, path);
	analysis_free(&a);
	return 0;
}

/* ---- persistent server: warm per-document analysis over a line protocol ----
 *
 * Requests on stdin (one per line; the editor's LSP server is the only client):
 *   UPDATE <bytelen> <path>\n<bytelen bytes>   (re)analyze a document, keep it warm
 *   HINTS <path>\n                              emit SYN lines for it, then a blank line
 *   CLOSE <path>\n                              drop the document
 * Responses: HINTS ends with a blank line; UPDATE/CLOSE reply "OK". Core + modules
 * stay parsed across requests, so each edit costs only the user buffer.
 */

typedef struct {
	char *path;
	Analysis a;
} Doc;

typedef struct {
	Doc *items;
	int count, cap;
} Docs;

static Doc *docs_find(Docs *d, const char *path) {
	for (int i = 0; i < d->count; i++)
		if (strcmp(d->items[i].path, path) == 0)
			return &d->items[i];
	return NULL;
}

static Doc *docs_get_or_add(Docs *d, const char *path) {
	Doc *existing = docs_find(d, path);
	if (existing)
		return existing;
	if (d->count >= d->cap) {
		d->cap = d->cap ? d->cap * 2 : 8;
		d->items = realloc(d->items, (size_t)d->cap * sizeof(Doc));
	}
	Doc *doc = &d->items[d->count++];
	doc->path = strdup(path);
	doc->a = (Analysis){NULL, NULL, NULL, {NULL, 0, 0}};
	return doc;
}

static void docs_remove(Docs *d, const char *path) {
	for (int i = 0; i < d->count; i++)
		if (strcmp(d->items[i].path, path) == 0) {
			analysis_free(&d->items[i].a);
			free(d->items[i].path);
			d->items[i] = d->items[--d->count];
			return;
		}
}

static void docs_free(Docs *d) {
	for (int i = 0; i < d->count; i++) {
		analysis_free(&d->items[i].a);
		free(d->items[i].path);
	}
	free(d->items);
}

/* Read one '\n'-terminated line from f (newline stripped). NULL on EOF. */
static char *read_line(FILE *f) {
	size_t cap = 128, len = 0;
	char *buf = malloc(cap);
	int c;
	while ((c = fgetc(f)) != EOF && c != '\n') {
		if (len + 1 >= cap) {
			cap *= 2;
			buf = realloc(buf, cap);
		}
		buf[len++] = (char)c;
	}
	if (c == EOF && len == 0) {
		free(buf);
		return NULL;
	}
	buf[len] = '\0';
	return buf;
}

static int run_serve(void) {
	Docs docs = {NULL, 0, 0};
	char *line;
	while ((line = read_line(stdin)) != NULL) {
		if (strncmp(line, "UPDATE ", 7) == 0) {
			/* UPDATE <bytelen> <path> */
			char *p = line + 7;
			long n = strtol(p, &p, 10);
			while (*p == ' ')
				p++;
			const char *path = p;
			char *src = malloc((size_t)(n > 0 ? n : 0) + 1);
			size_t got = n > 0 ? fread(src, 1, (size_t)n, stdin) : 0;
			src[got] = '\0';
			Doc *doc = docs_get_or_add(&docs, path);
			analysis_free(&doc->a);
			doc->a = analyze(src, doc->path); /* analyze takes ownership of src */
			printf("OK\n\n");                 /* blank line terminates every response */
			fflush(stdout);
		} else if (strncmp(line, "HINTS ", 6) == 0) {
			/* `HINTS <path>` (default) or `HINTS full <path>` — the editor asks for full type hints per
			 * the user's inlay setting; the server just honors the request. */
			const char *arg = line + 6;
			int full = 0;
			if (strncmp(arg, "full ", 5) == 0) {
				full = 1;
				arg += 5;
			}
			Doc *doc = docs_find(&docs, arg);
			if (doc)
				emit_hints(&doc->a, full);
			printf("\n"); /* blank line terminates the response */
			fflush(stdout);
		} else if (strncmp(line, "TOKENS ", 7) == 0) {
			Doc *doc = docs_find(&docs, line + 7);
			if (doc)
				emit_tokens(&doc->a);
			printf("\n");
			fflush(stdout);
		} else if (strncmp(line, "DIAG ", 5) == 0) {
			Doc *doc = docs_find(&docs, line + 5);
			if (doc)
				emit_diags(&doc->a);
			printf("\n");
			fflush(stdout);
		} else if (strncmp(line, "GOTO ", 5) == 0) {
			/* GOTO <kind> <line> <col> <path> — emit LOC lines for the resolved target(s). */
			char kind[8];
			int ln = 0, col = 0, used = 0;
			if (sscanf(line + 5, "%7s %d %d %n", kind, &ln, &col, &used) >= 3) {
				const char *path = line + 5 + used;
				Doc *doc = docs_find(&docs, path);
				if (doc)
					emit_goto(&doc->a, kind, ln, col, path);
			}
			printf("\n"); /* blank line terminates the response */
			fflush(stdout);
		} else if (strncmp(line, "CLOSE ", 6) == 0) {
			docs_remove(&docs, line + 6);
			printf("OK\n\n");
			fflush(stdout);
		}
		free(line);
	}
	docs_free(&docs);
	return 0;
}

/* Entry point for the analyzer, shared by the folded `arche analyze` subcommand (cli/cmd_analyze.c)
 * and the standalone `arche-analyzer` shim (arche_analyzer_main.c). Declared in arche_analyzer.h. */
int analyze_main(int argc, char *argv[]) {
	if (argc >= 2 && strcmp(argv[1], "--dump") == 0) {
		/* `--dump [--full] [file]` — --full also renders the redundant form-decl type hints. */
		int full = (argc >= 3 && strcmp(argv[2], "--full") == 0);
		const char *file = full ? (argc >= 4 ? argv[3] : NULL) : (argc >= 3 ? argv[2] : NULL);
		return run_dump(file, full);
	}
	if (argc >= 2 && strcmp(argv[1], "--serve") == 0)
		return run_serve();
	if (argc >= 6 && strcmp(argv[1], "--goto") == 0)
		return run_goto(argv[2], atoi(argv[3]), atoi(argv[4]), argv[5]);
	fprintf(stderr,
	        "usage: %s (--dump [--full] [file] | --serve | --goto <kind> <line> <col> <file>)\n", argv[0]);
	return 2;
}

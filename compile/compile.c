#include "compile.h"
#include "../cli/resource.h"
#include "../codegen/codegen.h"
#include "../lexer/lexer.h"
#include "../lower/lower.h"
#include "../parser/parser.h"
#include "../semantic/sem_diagnostics.h"
#include "../semantic/semantic.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef ARCHE_CORE_DIR
#define ARCHE_CORE_DIR "core"
#endif

#ifndef ARCHE_STDLIB_DIR
#define ARCHE_STDLIB_DIR "stdlib"
#endif

#ifndef ARCHE_RUNTIME_DIR
#define ARCHE_RUNTIME_DIR "build/runtime"
#endif

/* C99 doesn't expose mkdtemp; declare it explicitly (as we do for strdup). */
char *mkdtemp(char *tmpl);

static int file_exists(const char *path) {
	struct stat sb;
	return stat(path, &sb) == 0;
}

/* FNV-1a content hash of a file as a 16-hex-digit string in `out` (>=17 bytes). Per-unit object
 * caching keys on this: identical emitted IR → identical object, so an unchanged unit's `.o` is reused
 * across builds (incremental compilation). Returns 0 if the file can't be read. */
static int file_content_hash(const char *path, char out[17]) {
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	unsigned long long h = 1469598103934665603ULL; /* FNV offset basis */
	char buf[8192];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		for (size_t i = 0; i < n; i++) {
			h ^= (unsigned char)buf[i];
			h *= 1099511628211ULL; /* FNV prime */
		}
	fclose(f);
	snprintf(out, 17, "%016llx", h);
	return 1;
}

static char *source_dir_of(const char *path) {
	/* Return directory part of path. If no /, return "." */
	char *last_slash = strrchr(path, '/');
	if (!last_slash) {
		char *dir = malloc(2);
		strcpy(dir, ".");
		return dir;
	}

	int len = last_slash - path;
	char *dir = malloc(len + 1);
	strncpy(dir, path, len);
	dir[len] = '\0';
	return dir;
}

/* The program is non-empty iff the syntax tree root carries at least one declaration node
 * (any SN_*_DECL). A malformed top-level form is wrapped SN_ERROR (parse errors are
 * reported earlier), so it doesn't count as a declaration. */
static int syntax_root_has_decl(const SyntaxNode *syntax_root) {
	if (!syntax_root)
		return 0;
	for (int i = 0; i < syntax_root->child_count; i++) {
		if (syntax_root->children[i].tag != SE_NODE)
			continue;
		SyntaxNodeKind k = syntax_root->children[i].as.node->kind;
		if ((k >= SN_WORLD_DECL && k <= SN_USE_DECL) || k == SN_REGION)
			return 1;
	}
	return 0;
}

static char *read_file_optional(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size == 0) {
		fclose(f);
		return malloc(1); /* return empty string */
	}
	char *buf = malloc(size + 2);
	size_t n = fread(buf, 1, size, f);
	fclose(f);
	if (n > 0)
		buf[n] = '\n';
	buf[n + (n > 0 ? 1 : 0)] = '\0';
	return buf;
}

/* Dedup set of already-loaded module names (per compilation; reset in resolve_uses). Marking a
 * name BEFORE loading also makes transitive `#import` cycle-safe. */
#define MAX_LOADED_MODS 256
static char *g_loaded_mods[MAX_LOADED_MODS];
static int g_loaded_count;

/* Names of loaded modules that are DEVICES (their folder has a `.ds.arche` datasheet). A bare-name
 * `#import { x }` must resolve to a device; a plain module is imported by path. Reset in resolve_uses. */
static char *g_device_mods[MAX_LOADED_MODS];
static int g_device_count;

/* Import-resolution errors (rule 1: bare-name import of a non-device). Reset per compilation in
 * resolve_uses; checked by compile_frontend so a violation fails the build. */
static int g_resolve_errors;

static void mark_device_module(const char *name) {
	for (int i = 0; i < g_device_count; i++)
		if (strcmp(g_device_mods[i], name) == 0)
			return;
	if (g_device_count < MAX_LOADED_MODS)
		g_device_mods[g_device_count++] = strcpy(malloc(strlen(name) + 1), name);
}

static int module_is_device(const char *name) {
	for (int i = 0; i < g_device_count; i++)
		if (strcmp(g_device_mods[i], name) == 0)
			return 1;
	return 0;
}

static int file_is_datasheet(const char *name) {
	size_t L = strlen(name);
	return L >= 9 && strcmp(name + L - 9, ".ds.arche") == 0;
}

static void load_module(const char *name, const char *source_dir);           /* fwd (mutual recursion) */
static void load_module_from_path(const char *path, const char *source_dir); /* fwd */

/* Load every import named by a `#import` node. An IDENT child is a DEVICE imported by name
 * (`#import io`); a STRING child is a plain MODULE imported by path (`#import { "./util" }`). */
static void load_uses_of(const SyntaxNode *ud, const char *src, const char *source_dir) {
	for (int k = 0; k < ud->child_count; k++) {
		if (ud->children[k].tag != SE_TOKEN)
			continue;
		TokenKind tk = ud->children[k].as.token.kind;
		if (tk != TOK_IDENT && tk != TOK_STRING)
			continue;
		char buf[512];
		size_t L = ud->children[k].as.token.length;
		size_t off = ud->children[k].as.token.offset;
		/* A STRING token spans its quotes — strip them. */
		if (tk == TOK_STRING && L >= 2) {
			off += 1;
			L -= 2;
		}
		if (L > sizeof(buf) - 1)
			L = sizeof(buf) - 1;
		memcpy(buf, src + off, L);
		buf[L] = '\0';
		if (tk == TOK_STRING) {
			load_module_from_path(buf, source_dir);
		} else {
			load_module(buf, source_dir);
			/* Rule 1: a bare name must resolve to a DEVICE (a unit with a `.ds.arche`). A plain module
			 * must be imported by path. `core` is exempt (prepended prelude, never a device). */
			if (strcmp(buf, "core") != 0 && !module_is_device(buf)) {
				fprintf(stderr,
				        "Error: imported '%s' by name but it is not a device (no .ds.arche datasheet) — "
				        "import a plain module by path: #import { \"./%s\" }\n",
				        buf, buf);
				g_resolve_errors++;
			}
		}
	}
}

/* Parse one module file, register its lossless syntax tree with both back-ends (which borrow the syntax tree +
 * source), then recurse into the module's own `#import`s. Returns 1 on success. */
static int register_module_file(const char *mod_name, const char *path, const char *source_dir, DeclOrigin origin) {
	char *mod_src = read_file_optional(path);
	if (!mod_src)
		return 0;
	ParseResult mp = parse_source(mod_src);
	if (mp.error_count > 0 || !mp.syntax_root) {
		fprintf(stderr, "Error: Failed to parse module file %s\n", path);
		for (size_t j = 0; j < mp.error_count; j++)
			fprintf(stderr, "  [Line %d] %s\n", mp.errors[j].line, mp.errors[j].message);
		parse_result_free(&mp);
		free(mod_src);
		return 0;
	}
	lower_add_module(mod_name, mp.syntax_root, mod_src, path);
	semantic_add_module(mod_name, mp.syntax_root, mod_src, path, origin);
	const SyntaxNode *root = mp.syntax_root;
	const char *src = mod_src;
	mp.syntax_root = NULL;
	parse_result_free(&mp); /* mod_src + syntax tree kept alive (borrowed by the registries) */
	/* Transitive imports: a module may `#import` other modules (e.g. csv → io). */
	for (int u = 0; u < root->child_count; u++) {
		if (root->children[u].tag != SE_NODE || root->children[u].as.node->kind != SN_USE_DECL)
			continue;
		load_uses_of(root->children[u].as.node, src, source_dir);
	}
	return 1;
}

/* A module is a FOLDER: `<dir>/<name>/` with one or more `.arche` files, all merged into one
 * module namespace. Falls back to a single file `<dir>/<name>.arche`. Returns files registered. */
static int try_load_module_dir(const char *mod_name, const char *dir, const char *source_dir, DeclOrigin origin) {
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
				n += register_module_file(mod_name, fp, source_dir, origin);
				if (file_is_datasheet(ent->d_name))
					mark_device_module(mod_name); /* a `.ds.arche` in the folder makes it a device */
			}
		}
		closedir(d);
		if (n > 0)
			return n;
	}
	char fp[640];
	snprintf(fp, sizeof(fp), "%s/%s.arche", dir, mod_name);
	if (file_exists(fp))
		return register_module_file(mod_name, fp, source_dir, origin);
	return 0;
}

/* Load a plain MODULE imported by PATH (`#import { "./util" }`): resolve `path` relative to the
 * importing file's dir, derive the module name from the path's basename (sans `.arche`), and load the
 * file/folder there. Unlike a bare-name import, no stdlib/core search and no device requirement —
 * importing a `.ds.arche` by path is legal (just not useful). */
static void load_module_from_path(const char *path, const char *source_dir) {
	/* Split `path` into its directory part and basename. */
	const char *slash = strrchr(path, '/');
	const char *base = slash ? slash + 1 : path;
	char subdir[512];
	if (slash) {
		size_t dl = (size_t)(slash - path);
		if (dl > sizeof(subdir) - 1)
			dl = sizeof(subdir) - 1;
		memcpy(subdir, path, dl);
		subdir[dl] = '\0';
	} else {
		subdir[0] = '\0';
	}
	/* Module name = basename without a trailing `.arche`. */
	char mod_name[256];
	snprintf(mod_name, sizeof(mod_name), "%s", base);
	size_t ml = strlen(mod_name);
	if (ml > 6 && strcmp(mod_name + ml - 6, ".arche") == 0)
		mod_name[ml - 6] = '\0';
	if (mod_name[0] == '\0')
		return;
	/* Dedup by module name (the registry namespace is flat). */
	for (int i = 0; i < g_loaded_count; i++)
		if (strcmp(g_loaded_mods[i], mod_name) == 0)
			return;
	if (g_loaded_count < MAX_LOADED_MODS)
		g_loaded_mods[g_loaded_count++] = strcpy(malloc(strlen(mod_name) + 1), mod_name);
	/* Resolve the directory to search relative to the importing file's dir. Transitive imports from
	 * the loaded module resolve relative to that same dir. */
	char dir[800];
	if (subdir[0])
		snprintf(dir, sizeof(dir), "%s/%s", source_dir, subdir);
	else
		snprintf(dir, sizeof(dir), "%s", source_dir);
	if (!try_load_module_dir(mod_name, dir, dir, DECL_ORIGIN_USER_MODULE)) /* path import = user's tree */
		fprintf(stderr, "Error: module path not found: %s\n", path);
}

/* Load module `name` (dedup'd) by searching the source dir, then stdlib, then core. */
static void load_module(const char *name, const char *source_dir) {
	for (int i = 0; i < g_loaded_count; i++)
		if (strcmp(g_loaded_mods[i], name) == 0)
			return;
	if (g_loaded_count < MAX_LOADED_MODS) {
		char *dup = malloc(strlen(name) + 1);
		strcpy(dup, name);
		g_loaded_mods[g_loaded_count++] = dup; /* mark before load → cycle-safe */
	}
	/* STDLIB is authoritative: a stdlib module name (io/net/str/parse/sys/csv/router/…) always
	 * resolves to stdlib, even if the source tree has a same-named subdir (e.g. the test suite's
	 * `tests/unit/language/io/`). The prelude imports `io`, so every program triggers this — without
	 * stdlib-first, a local `io/` dir would shadow it. A user's OWN (non-stdlib) module is still
	 * found by the source-dir search below. */
	int loaded = try_load_module_dir(name, arche_resource_dir(ARCHE_RES_STDLIB), source_dir, DECL_ORIGIN_STDLIB);
	if (!loaded)
		loaded = try_load_module_dir(name, source_dir, source_dir, DECL_ORIGIN_USER_MODULE);
	if (!loaded)
		loaded = try_load_module_dir(name, arche_resource_dir(ARCHE_RES_CORE), source_dir, DECL_ORIGIN_CORE);
	if (!loaded)
		fprintf(stderr, "Error: Module not found: %s\n", name);
}

/* Resolve `#import foo` from the syntax tree: locate each module (folder or file), register it with both
 * back-ends, and recurse into its transitive imports. syntax tree + source are kept alive (borrowed). */
static void resolve_uses(const SyntaxNode *syntax_root, const char *src, const char *source_path) {
	if (!syntax_root)
		return;
	/* Static registries — clear leftovers from a prior compilation (the doctest runner compiles
	 * many examples in one process) so modules aren't inlined twice. */
	lower_reset_modules();
	semantic_reset_modules();
	for (int i = 0; i < g_loaded_count; i++)
		free(g_loaded_mods[i]);
	g_loaded_count = 0;
	for (int i = 0; i < g_device_count; i++)
		free(g_device_mods[i]);
	g_device_count = 0;
	g_resolve_errors = 0;

	char *source_dir = source_dir_of(source_path);
	for (int u = 0; u < syntax_root->child_count; u++) {
		if (syntax_root->children[u].tag != SE_NODE || syntax_root->children[u].as.node->kind != SN_USE_DECL)
			continue;
		load_uses_of(syntax_root->children[u].as.node, src, source_dir);
	}
	free(source_dir);
}

/* Front-end products handed from compile_frontend() to its callers. On success the caller owns
 * `source` (free it) and `sem_ctx` (semantic_context_free); `syntax_root` is kept alive for lowering
 * and follows the same ownership it always had (see compile_source's cleanup — it is not freed). */
typedef struct {
	char *source;
	SyntaxNode *syntax_root;
	SemanticContext *sem_ctx;
	int core_lines;
} Frontend;

/* Prepend the core prelude, parse, resolve `#import` modules, and semantically analyze. On success
 * fills *out and returns 0; on any error prints diagnostics to stderr, releases its own partial
 * allocations, and returns 1. Shared by `build` (compile_source) and `check` (compile_check) so
 * both run a byte-identical front-end. */
static int compile_frontend(const char *user_source, const char *source_path, Frontend *out) {
	/* Load and prepend core library. Count its newlines so we can subtract them
	 * from any combined-source line numbers we surface — the user wrote line N of
	 * their file, not line N + core_lines of the combined stream. */
	char core_path[512];
	snprintf(core_path, sizeof(core_path), "%s/core.arche", arche_resource_dir(ARCHE_RES_CORE));
	char *core_src = read_file_optional(core_path);

	/* Don't prepend core to core itself (double-defines every prelude symbol). When the
	 * compiled file IS core.arche, drop the prepend by treating core as empty here.
	 * Identity by (device, inode) so any path spelling of the prelude matches. */
	if (core_src && source_path && source_path[0]) {
		struct stat sp, sc;
		if (stat(source_path, &sp) == 0 && stat(core_path, &sc) == 0 && sp.st_dev == sc.st_dev &&
		    sp.st_ino == sc.st_ino) {
			free(core_src);
			core_src = NULL;
		}
	}

	int core_lines = 0;
	char *source;
	if (core_src && strlen(core_src) > 0) {
		for (const char *p = core_src; *p; p++)
			if (*p == '\n')
				core_lines++;
		/* Combine: core + user source */
		size_t cl = strlen(core_src), ul = strlen(user_source);
		source = malloc(cl + ul + 1);
		memcpy(source, core_src, cl);
		memcpy(source + cl, user_source, ul + 1);
		free(core_src);
	} else {
		if (core_src)
			free(core_src); /* core.arche was empty */
		source = malloc(strlen(user_source) + 1);
		strcpy(source, user_source);
	}

	/* Diagnostics from semantic analysis carry combined-source line numbers; tell
	 * the diagnostic printer to subtract the core prelude before printing. */
	semantic_set_print_line_offset(core_lines);

	/* Lexical analysis and parsing */
	ParseResult parse_result = parse_source(source);

	if (parse_result.error_count > 0) {
		for (size_t i = 0; i < parse_result.error_count; i++) {
			int line = parse_result.errors[i].line - core_lines;
			if (line < 1)
				line = 1; /* error inside core; clamp so we don't show negative lines */
			fprintf(stderr, "[Line %d, Col %d] Error: %s\n", line, parse_result.errors[i].column,
			        parse_result.errors[i].message);
		}
		parse_result_free(&parse_result);
		free(source);
		return 1;
	}

	/* The parser produces ONLY the lossless syntax tree; semantic analysis collects its resolved
	 * DeclSummary table directly from it (no abstract AST is built anywhere). Keep the syntax tree
	 * alive through lowering (syntax-tree-driven lowering reads it); the rest of the parse result
	 * is freed now. */
	SyntaxNode *syntax_root = parse_result.syntax_root;
	parse_result.syntax_root = NULL;
	parse_result_free(&parse_result);

	/* Empty-program check is now syntax-tree-based: the program is empty unless the syntax tree
	 * root carries at least one (non-error) declaration node. */
	if (!syntax_root_has_decl(syntax_root)) {
		fprintf(stderr, "Error: Empty program\n");
		free(source);
		return 1;
	}

	/* Resolve use declarations (module loading): register each module's syntax tree with the syntax tree
	 * analyzer + lowerer, which inline + name-prefix it. Tuple-group flattening for archetype
	 * fields happens inside those syntax tree passes too, so no AstProgram pre-pass is needed. */
	resolve_uses(syntax_root, source, source_path);
	if (g_resolve_errors > 0) { /* rule 1: a bare name imported a non-device */
		fflush(stderr);
		free(source);
		return 1;
	}

	/* Semantic analysis: reconstruct the abstract AST from the lossless syntax tree (+ registered
	 * module syntax trees) and analyze that (rustc/Go-style: syntax tree → AST → check). The parser-built
	 * AstProgram is not consulted. The side model (keyed by syntax tree node id) feeds lowering. */
	SemanticContext *sem_ctx = semantic_analyze_cst(syntax_root, source);

	if (!sem_ctx || semantic_has_errors(sem_ctx)) {
		fprintf(stderr, "Semantic analysis failed\n");
		fflush(stderr);
		if (sem_ctx)
			semantic_context_free(sem_ctx);
		free(source);
		return 1;
	}

	out->source = source;
	out->syntax_root = syntax_root;
	out->sem_ctx = sem_ctx;
	out->core_lines = core_lines;
	return 0;
}

int compile_check(const char *user_source, const char *source_path, const CompileOpts *opts) {
	(void)opts; /* lint config is applied globally via semantic_set_lint_* before this call */
	Frontend fe;
	if (compile_frontend(user_source, source_path, &fe) != 0)
		return 1;
	semantic_context_free(fe.sem_ctx);
	free(fe.source);
	return 0;
}

int compile_source(const char *user_source, const char *source_path, const char *out_path, const CompileOpts *opts) {
	EmitKind emit = opts ? opts->emit : EMIT_LINK;
	int quiet = opts ? opts->quiet : 0;

	Frontend fe;
	if (compile_frontend(user_source, source_path, &fe) != 0)
		return 1;
	char *source = fe.source;
	SyntaxNode *syntax_root = fe.syntax_root;
	SemanticContext *sem_ctx = fe.sem_ctx;

	/* Lower the lossless syntax tree → AST (the only lowering path). Resolved types come from the
	 * semantic side model (keyed by syntax tree node id, globally unique across inlined modules);
	 * `use` modules are inlined from their registered syntax trees (see resolve_uses / lower_add_module). */
	lower_set_model(sem_context_model(sem_ctx));
	lower_set_sem(sem_ctx);
	HirProgram *ast = lower_to_hir(syntax_root, source);

	/* Code generation */
	CodegenContext *codegen_ctx = codegen_create(ast, sem_ctx);

	/* All exits past this point flow through `cleanup:` so the toolchain temp
	 * files, the work dir, and the IR/AST/syntax tree allocations are released exactly
	 * once. rc stays 1 until a path proves success. */
	int rc = 1;

	/* Builds past codegen put every intermediate in a private mkdtemp work dir
	 * (mode 0700, unpredictable name) — so concurrent compiles never collide on
	 * a shared /tmp/arche_<pid> path and there is no predictable-name symlink
	 * race. `--emit=llvm-ir` writes straight to the caller's out_path. */
	const char *ir_file;
	char temp_ir[512] = "", opt_file[512] = "", asm_file[512] = "";
	char workdir[] = "/tmp/arche_XXXXXX";
	int have_workdir = 0;
	/* Per-unit codegen needs a work dir for the per-unit `.ll` modules even when the final deliverable
	 * is raw IR (it llvm-links them into out_path). */
	int per_unit_build = codegen_per_unit_enabled();
	if (emit == EMIT_LLVM_IR && !per_unit_build) {
		ir_file = out_path;
	} else {
		if (!mkdtemp(workdir)) {
			perror("Failed to create temp dir");
			goto cleanup;
		}
		have_workdir = 1;
		snprintf(temp_ir, sizeof(temp_ir), "%s/out.ll", workdir);
		snprintf(opt_file, sizeof(opt_file), "%s/out_opt.ll", workdir);
		snprintf(asm_file, sizeof(asm_file), "%s/out.s", workdir);
		ir_file = (emit == EMIT_LLVM_IR) ? out_path : temp_ir;
	}

	if (per_unit_build) {
		/* Per-unit codegen: emit one LLVM module per compilation unit, then llvm-link them into the
		 * combined IR the rest of the pipeline consumes. Cross-unit references resolve via the declares
		 * each unit module emits + external/mangled symbols. (Per-object caching is the next step; this
		 * proves per-unit emission + linkage produce a correct program.) */
		int max_unit = 0;
		for (int i = 0; i < ast->decl_count; i++)
			if (ast->decls[i]->unit > max_unit)
				max_unit = ast->decls[i]->unit;
		/* 1) Emit one LLVM module per compilation unit. */
		int unit_err = 0;
		for (int u = 0; u <= max_unit; u++) {
			char uf[600];
			snprintf(uf, sizeof(uf), "%s/unit_%d.ll", workdir, u);
			CodegenContext *uctx = codegen_create(ast, sem_ctx);
			codegen_set_emit_unit(uctx, u);
			FILE *ufp = fopen(uf, "w");
			if (!ufp) {
				perror("Failed to open per-unit IR file");
				codegen_free(uctx);
				unit_err = 1;
				break;
			}
			codegen_generate(uctx, ufp);
			fclose(ufp);
			if (codegen_had_error(uctx))
				unit_err = 1;
			codegen_free(uctx);
			if (unit_err)
				break;
		}
		if (unit_err) {
			rc = 1;
			goto cleanup;
		}
		if (emit == EMIT_LINK) {
			/* 2a) Incremental link: compile each unit to an object CACHED by its IR content hash (an
			 * unchanged unit — e.g. the stdlib — reuses its `.o` across builds), then link the objects.
			 * This is the separate-compilation payoff: editing one unit recompiles only that unit. */
			const char *rt = arche_resource_dir(ARCHE_RES_RUNTIME);
			const char *home = getenv("HOME");
			char cachedir[700];
			snprintf(cachedir, sizeof(cachedir), "%s/.cache/arche/obj", home ? home : "/tmp");
			char mk[760];
			snprintf(mk, sizeof(mk), "mkdir -p '%s'", cachedir);
			if (system(mk) != 0) {
				fprintf(stderr, "Failed to create object cache dir\n");
				goto cleanup;
			}
			char objs[16384];
			int oj = 0, obj_err = 0;
			for (int u = 0; u <= max_unit; u++) {
				char uf[640], key[17];
				snprintf(uf, sizeof(uf), "%s/unit_%d.ll", workdir, u);
				if (!file_content_hash(uf, key)) {
					obj_err = 1;
					break;
				}
				char ocache[800];
				snprintf(ocache, sizeof(ocache), "%s/%s.o", cachedir, key);
				if (!file_exists(ocache)) {
					char uopt[680], us[680], cmd[2400];
					snprintf(uopt, sizeof(uopt), "%s/unit_%d_opt.ll", workdir, u);
					snprintf(us, sizeof(us), "%s/unit_%d.s", workdir, u);
					snprintf(cmd, sizeof(cmd), "opt -O2 -mcpu=x86-64-v3 -S -o %s %s", uopt, uf);
					if (system(cmd) != 0) {
						obj_err = 1;
						break;
					}
					snprintf(cmd, sizeof(cmd),
					         "llc -function-sections -data-sections -code-model=large -mcpu=x86-64-v3 -o %s %s", us,
					         uopt);
					if (system(cmd) != 0) {
						obj_err = 1;
						break;
					}
					snprintf(cmd, sizeof(cmd), "cc -no-pie -mcmodel=large -c -o '%s' %s", ocache, us);
					if (system(cmd) != 0) {
						obj_err = 1;
						break;
					}
				}
				oj += snprintf(objs + oj, sizeof(objs) - (size_t)oj, " '%s'", ocache);
			}
			if (obj_err) {
				rc = 1;
				goto cleanup;
			}
			char cc_cmd[20000];
			int cl = snprintf(
			    cc_cmd, sizeof(cc_cmd),
			    "cc -no-pie -mcmodel=large -Wl,--gc-sections -o %s%s %s/stack_check.o %s/io.o %s/net.o %s/term.o -lc",
			    out_path, objs, rt, rt, rt, rt);
			int link_count = opts ? opts->link_count : 0;
			for (int li = 0; li < link_count && cl > 0 && cl < (int)sizeof(cc_cmd); li++)
				cl += snprintf(cc_cmd + cl, sizeof(cc_cmd) - (size_t)cl, " %s", opts->link_paths[li]);
			if (system(cc_cmd) != 0) {
				fprintf(stderr, "Failed to link per-unit objects\n");
				goto cleanup;
			}
			rc = 0;
			goto cleanup;
		}
		/* 2b) Other emit modes (IR/asm/obj): llvm-link the unit modules into the combined IR the shared
		 * opt/llc/cc pipeline below consumes. */
		char link_cmd[16384];
		int lp = snprintf(link_cmd, sizeof(link_cmd), "llvm-link -S -o %s", ir_file);
		for (int u = 0; u <= max_unit; u++)
			lp += snprintf(link_cmd + lp, sizeof(link_cmd) - (size_t)lp, " %s/unit_%d.ll", workdir, u);
		if (system(link_cmd) != 0) {
			fprintf(stderr, "Failed to llvm-link per-unit modules\n");
			goto cleanup;
		}
	} else {
		FILE *ir_output = fopen(ir_file, "w");
		if (!ir_output) {
			perror("Failed to open IR output file");
			goto cleanup;
		}
		codegen_generate(codegen_ctx, ir_output);
		fclose(ir_output);
		if (codegen_had_error(codegen_ctx)) {
			/* Codegen already printed the diagnostic; fail the build, don't run the emitted IR. */
			rc = 1;
			goto cleanup;
		}
	}
	if (!quiet)
		printf("Generated LLVM IR: %s\n", ir_file);

	if (emit == EMIT_LLVM_IR) {
		rc = 0;
		goto cleanup;
	}

	/* Run `opt -O2` first to promote allocas to SSA (mem2reg), do basic CSE,
	 * loop-invariant code motion, etc. `-mcpu=x86-64-v3` matches llc so the loop
	 * vectorizer uses the AVX2 width instead of the generic 2-wide default. */
	{
		char opt_cmd[1024];
		int m = snprintf(opt_cmd, sizeof(opt_cmd), "opt -O2 -mcpu=x86-64-v3 -S -o %s %s", opt_file, ir_file);
		if (m < 0 || m >= (int)sizeof(opt_cmd)) {
			fprintf(stderr, "opt command too long\n");
			goto cleanup;
		}
		if (!quiet)
			printf("Optimizing IR...\n");
		if (system(opt_cmd) != 0) {
			fprintf(stderr, "Failed to optimize LLVM IR\n");
			goto cleanup;
		}
	}

	/* llc → assembly. -mcpu=x86-64-v3 = portable AVX2 baseline (Haswell+): lets
	 * `<4 x double>` lower to ymm-wide vmulpd/vfmadd without pinning to this CPU.
	 * For `--emit=asm` the assembly IS the deliverable, so llc writes it straight to out_path. */
	const char *asm_target = (emit == EMIT_ASM) ? out_path : asm_file;
	{
		char llc_cmd[1024];
		/* Per-unit codegen gives arche funcs external linkage (cross-object refs), so the optimizer no
		 * longer DCEs unreferenced ones. `-function-sections`/`-data-sections` + the linker's
		 * `--gc-sections` (below) restore dead-symbol stripping at link time. */
		const char *sections = getenv("ARCHE_PER_UNIT") ? "-function-sections -data-sections " : "";
		int m = snprintf(llc_cmd, sizeof(llc_cmd), "llc %s-code-model=large -mcpu=x86-64-v3 -o %s %s", sections,
		                 asm_target, opt_file);
		if (m < 0 || m >= (int)sizeof(llc_cmd)) {
			fprintf(stderr, "llc command too long\n");
			goto cleanup;
		}
		if (!quiet)
			printf("Compiling to assembly...\n");
		if (system(llc_cmd) != 0) {
			fprintf(stderr, "Failed to compile LLVM IR to assembly\n");
			goto cleanup;
		}
	}

	if (emit == EMIT_ASM) {
		if (!quiet)
			printf("Generated assembly: %s\n", out_path);
		rc = 0;
		goto cleanup;
	}

	/* `--emit=obj`: assemble to an object file (`cc -c`), no link. */
	if (emit == EMIT_OBJ) {
		char obj_cmd[1024];
		int m = snprintf(obj_cmd, sizeof(obj_cmd), "cc -no-pie -mcmodel=large -c -o %s %s", out_path, asm_file);
		if (m < 0 || m >= (int)sizeof(obj_cmd)) {
			fprintf(stderr, "assemble command too long\n");
			goto cleanup;
		}
		if (!quiet)
			printf("Assembling object: %s\n", out_path);
		if (system(obj_cmd) != 0) {
			fprintf(stderr, "Failed to assemble object file\n");
			goto cleanup;
		}
		rc = 0;
		goto cleanup;
	}

	/* cc: assemble + link the runtime objects (and any --link inputs). */
	{
		const char *rt = arche_resource_dir(ARCHE_RES_RUNTIME);
		char cc_cmd[8192];
		const char *gc = getenv("ARCHE_PER_UNIT") ? "-Wl,--gc-sections " : "";
		int cc_len = snprintf(cc_cmd, sizeof(cc_cmd),
		                      "cc %s-no-pie -mcmodel=large -o %s %s %s/stack_check.o %s/io.o %s/net.o %s/term.o -lc",
		                      gc, out_path, asm_file, rt, rt, rt, rt);
		if (cc_len < 0 || cc_len >= (int)sizeof(cc_cmd)) {
			fprintf(stderr, "link command too long\n");
			goto cleanup;
		}
		int link_count = opts ? opts->link_count : 0;
		for (int li = 0; li < link_count; li++) {
			int m = snprintf(cc_cmd + cc_len, sizeof(cc_cmd) - (size_t)cc_len, " %s", opts->link_paths[li]);
			/* Refuse to silently drop a linker input on overflow — that would
			 * miscompile (missing symbols) rather than fail loudly. */
			if (m < 0 || m >= (int)sizeof(cc_cmd) - cc_len) {
				fprintf(stderr, "link command too long; refusing to drop --link inputs\n");
				goto cleanup;
			}
			cc_len += m;
		}
		if (!quiet)
			printf("Linking executable...\n");
		if (system(cc_cmd) != 0) {
			fprintf(stderr, "Failed to link executable\n");
			goto cleanup;
		}
	}

	if (!quiet)
		printf("Successfully generated executable: %s\n", out_path);
	rc = 0;

cleanup:
	if (have_workdir) {
		if (temp_ir[0])
			unlink(temp_ir);
		if (opt_file[0])
			unlink(opt_file);
		if (asm_file[0])
			unlink(asm_file);
		rmdir(workdir);
	}
	/* AST must be freed before syntax tree (HIR_TYPE_NAMED ptrs reference into the syntax tree). */
	codegen_free(codegen_ctx);
	semantic_context_free(sem_ctx);
	hir_program_free(ast);
	free(source);
	return rc;
}

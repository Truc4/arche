#include "compile.h"
#include "module_resolve.h"
#include "variant_select.h"
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

/* ODR verification (unconditional in per-unit mode). Every `define
 * linkonce_odr` symbol emitted in more than one unit module MUST have a byte-identical body — that is
 * the invariant the linker's duplicate-folding relies on. A divergence would be a SILENT miscompile
 * (the linker keeps one arbitrary copy), so this turns the assumption into a checked, loud failure.
 * Returns 1 on any divergence (reported to stderr), 0 if all consistent. */
static int verify_odr(const char *workdir, int max_unit) {
	char **syms = NULL, **bodies = NULL;
	int n = 0, cap = 0, bad = 0;
	for (int u = 0; u <= max_unit; u++) {
		char uf[640];
		snprintf(uf, sizeof(uf), "%s/unit_%d.ll", workdir, u);
		FILE *f = fopen(uf, "r");
		if (!f)
			continue;
		char line[8192];
		while (fgets(line, sizeof(line), f)) {
			if (strncmp(line, "define linkonce_odr ", 20) != 0)
				continue;
			char *at = strchr(line, '@');
			char *lp = at ? strchr(at, '(') : NULL;
			if (!at || !lp)
				continue;
			size_t sl = (size_t)(lp - (at + 1));
			char *sym = malloc(sl + 1);
			memcpy(sym, at + 1, sl);
			sym[sl] = '\0';
			/* Accumulate the whole definition: the `define` line through the closing `}` line. */
			size_t bcap = 16384, blen = 0;
			char *body = malloc(bcap);
			for (;;) {
				size_t ln = strlen(line);
				if (blen + ln + 1 > bcap) {
					bcap = (blen + ln + 1) * 2;
					body = realloc(body, bcap);
				}
				memcpy(body + blen, line, ln + 1);
				blen += ln;
				if (line[0] == '}')
					break;
				if (!fgets(line, sizeof(line), f))
					break;
			}
			int found = -1;
			for (int i = 0; i < n; i++)
				if (strcmp(syms[i], sym) == 0) {
					found = i;
					break;
				}
			if (found >= 0) {
				if (strcmp(bodies[found], body) != 0) {
					fprintf(stderr, "ODR VIOLATION: linkonce_odr symbol @%s differs across units (unit_%d.ll)\n", sym,
					        u);
					bad = 1;
				}
				free(sym);
				free(body);
			} else {
				if (n == cap) {
					cap = cap ? cap * 2 : 64;
					syms = realloc(syms, (size_t)cap * sizeof(char *));
					bodies = realloc(bodies, (size_t)cap * sizeof(char *));
				}
				syms[n] = sym;
				bodies[n] = body;
				n++;
			}
		}
		fclose(f);
	}
	for (int i = 0; i < n; i++) {
		free(syms[i]);
		free(bodies[i]);
	}
	free(syms);
	free(bodies);
	return bad;
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

/* ---- Module resolution: the SEARCH POLICY (which dir, which files, dedup, variants) lives in the
 * shared resolver (compile/module_resolve.c) so the compiler and the editor analyzer can never
 * disagree about which file backs an `#import`. The compiler supplies its own registration (parse +
 * lower + semantic) and device/dedup bookkeeping via these callbacks. */

static int compile_mark_seen(void *ctx, const char *name) {
	(void)ctx;
	for (int i = 0; i < g_loaded_count; i++)
		if (strcmp(g_loaded_mods[i], name) == 0)
			return 1;
	if (g_loaded_count < MAX_LOADED_MODS)
		g_loaded_mods[g_loaded_count++] = strcpy(malloc(strlen(name) + 1), name); /* mark before load → cycle-safe */
	return 0;
}

static int compile_register_file(void *ctx, const char *mod_name, const char *path, const char *source_dir,
                                 DeclOrigin origin) {
	(void)ctx;
	return register_module_file(mod_name, path, source_dir, origin);
}

static void compile_mark_device(void *ctx, const char *mod_name) {
	(void)ctx;
	mark_device_module(mod_name);
}

/* Active per-device variant selection. Loaded once from `ARCHE_SELECT` (env) the first time a
 * device is resolved; the CLI/manifest layers (higher precedence) will preload this map in a later
 * phase. The SAME selection drives the analyzer, so the editor and the build agree on backends. */
static VariantMap g_compile_variants;
static int g_compile_variants_loaded;

static const char *compile_select_variant(void *ctx, const char *mod_name) {
	(void)ctx;
	if (!g_compile_variants_loaded) {
		variant_map_load_env(&g_compile_variants);
		g_compile_variants_loaded = 1;
	}
	return variant_map_lookup(&g_compile_variants, mod_name);
}

static const ModuleResolver g_compile_resolver = {
    NULL, compile_mark_seen, compile_register_file, compile_mark_device, compile_select_variant,
};

/* Load a plain MODULE imported by PATH (`#import { "./util" }`). */
static void load_module_from_path(const char *path, const char *source_dir) {
	if (!arche_module_load_by_path(&g_compile_resolver, path, source_dir))
		fprintf(stderr, "Error: module path not found: %s\n", path);
}

/* Load module `name` (dedup'd) by searching stdlib, then the source dir, then core. */
static void load_module(const char *name, const char *source_dir) {
	if (!arche_module_load_by_name(&g_compile_resolver, name, source_dir))
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
		 * each unit module emits + external/mangled symbols. A correctness/readiness mode that proves
		 * per-unit emission + linkage produce a correct program — NOT a build-speed mode (there is no
		 * per-object cache; see the always-on ODR verifier below and the doc comment on CodegenContext). */
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
		/* ODR tripwire — ALWAYS run in per-unit mode (not opt-in): `llvm-link` folds `linkonce_odr`
		 * duplicates, so a divergent shared body would be silently merged to one arbitrary copy. This
		 * makes that a loud, deterministic build failure instead. Cheap: string work over files on disk. */
		if (verify_odr(workdir, max_unit)) {
			fprintf(stderr, "Per-unit ODR verification failed (shared definitions diverge across units)\n");
			rc = 1;
			goto cleanup;
		}
		/* llvm-link the per-unit modules into the combined IR the shared opt/llc/cc pipeline below
		 * consumes. Per-unit is an EXPERIMENTAL correctness/readiness mode — it proves codegen can split
		 * and exercises the mangled/external symbol model — NOT a build-speed mode; there is no
		 * per-object caching (that was removed as speculative; see the plan). */
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

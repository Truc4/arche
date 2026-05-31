#include "compile.h"
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

/* The program is non-empty iff the CST root carries at least one declaration node
 * (any SN_*_DECL). A malformed top-level form is wrapped SN_ERROR (parse errors are
 * reported earlier), so it doesn't count as a declaration. */
static int cst_root_has_decl(const SyntaxNode *cst_root) {
	if (!cst_root)
		return 0;
	for (int i = 0; i < cst_root->child_count; i++) {
		if (cst_root->children[i].tag != SE_NODE)
			continue;
		SyntaxNodeKind k = cst_root->children[i].as.node->kind;
		if (k >= SN_WORLD_DECL && k <= SN_USE_DECL)
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

static void load_module(const char *name, const char *source_dir); /* fwd (mutual recursion) */

/* Extract the module name (first IDENT token) from a `#import <name>;` (SN_USE_DECL) node. */
static int module_name_of(const SyntaxNode *ud, const char *src, char *out, size_t sz) {
	out[0] = '\0';
	for (int k = 0; k < ud->child_count; k++)
		if (ud->children[k].tag == SE_TOKEN && ud->children[k].as.token.kind == TOK_IDENT) {
			size_t L = ud->children[k].as.token.length;
			if (L > sz - 1)
				L = sz - 1;
			memcpy(out, src + ud->children[k].as.token.offset, L);
			out[L] = '\0';
			return 1;
		}
	return 0;
}

/* Parse one module file, register its lossless CST with both back-ends (which borrow the CST +
 * source), then recurse into the module's own `#import`s. Returns 1 on success. */
static int register_module_file(const char *mod_name, const char *path, const char *source_dir) {
	char *mod_src = read_file_optional(path);
	if (!mod_src)
		return 0;
	ParseResult mp = parse_source(mod_src);
	if (mp.error_count > 0 || !mp.cst_root) {
		fprintf(stderr, "Error: Failed to parse module file %s\n", path);
		for (size_t j = 0; j < mp.error_count; j++)
			fprintf(stderr, "  [Line %d] %s\n", mp.errors[j].line, mp.errors[j].message);
		parse_result_free(&mp);
		free(mod_src);
		return 0;
	}
	lower_add_module(mod_name, mp.cst_root, mod_src);
	semantic_add_module(mod_name, mp.cst_root, mod_src);
	const SyntaxNode *root = mp.cst_root;
	const char *src = mod_src;
	mp.cst_root = NULL;
	parse_result_free(&mp); /* mod_src + CST kept alive (borrowed by the registries) */
	/* Transitive imports: a module may `#import` other modules (e.g. csv → io). */
	for (int u = 0; u < root->child_count; u++) {
		if (root->children[u].tag != SE_NODE || root->children[u].as.node->kind != SN_USE_DECL)
			continue;
		char dep[256];
		if (module_name_of(root->children[u].as.node, src, dep, sizeof(dep)))
			load_module(dep, source_dir);
	}
	return 1;
}

/* A module is a FOLDER: `<dir>/<name>/` with one or more `.arche` files, all merged into one
 * module namespace. Falls back to a single file `<dir>/<name>.arche`. Returns files registered. */
static int try_load_module_dir(const char *mod_name, const char *dir, const char *source_dir) {
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
				n += register_module_file(mod_name, fp, source_dir);
			}
		}
		closedir(d);
		if (n > 0)
			return n;
	}
	char fp[640];
	snprintf(fp, sizeof(fp), "%s/%s.arche", dir, mod_name);
	if (file_exists(fp))
		return register_module_file(mod_name, fp, source_dir);
	return 0;
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
	int loaded = try_load_module_dir(name, source_dir, source_dir);
	if (!loaded)
		loaded = try_load_module_dir(name, ARCHE_STDLIB_DIR, source_dir);
	if (!loaded)
		loaded = try_load_module_dir(name, ARCHE_CORE_DIR, source_dir);
	if (!loaded)
		fprintf(stderr, "Error: Module not found: %s\n", name);
}

/* Resolve `#import foo;` from the CST: locate each module (folder or file), register it with both
 * back-ends, and recurse into its transitive imports. CST + source are kept alive (borrowed). */
static void resolve_uses(const SyntaxNode *cst_root, const char *src, const char *source_path) {
	if (!cst_root)
		return;
	/* Static registries — clear leftovers from a prior compilation (the doctest runner compiles
	 * many examples in one process) so modules aren't inlined twice. */
	lower_reset_modules();
	semantic_reset_modules();
	for (int i = 0; i < g_loaded_count; i++)
		free(g_loaded_mods[i]);
	g_loaded_count = 0;

	char *source_dir = source_dir_of(source_path);
	for (int u = 0; u < cst_root->child_count; u++) {
		if (cst_root->children[u].tag != SE_NODE || cst_root->children[u].as.node->kind != SN_USE_DECL)
			continue;
		char mod_name[256];
		if (module_name_of(cst_root->children[u].as.node, src, mod_name, sizeof(mod_name)))
			load_module(mod_name, source_dir);
	}
	free(source_dir);
}

int compile_source(const char *user_source, const char *source_path, const char *out_path, const CompileOpts *opts) {
	int emit_llvm = opts ? opts->emit_llvm : 0;
	int quiet = opts ? opts->quiet : 0;

	/* Load and prepend core library. Count its newlines so we can subtract them
	 * from any combined-source line numbers we surface — the user wrote line N of
	 * their file, not line N + core_lines of the combined stream. */
	char core_path[512];
	snprintf(core_path, sizeof(core_path), "%s/core.arche", ARCHE_CORE_DIR);
	char *core_src = read_file_optional(core_path);

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

	/* The parser produces ONLY the lossless CST now (no abstract AstProgram); the
	 * abstract AST is built solely by cst_to_program inside semantic analysis.
	 * Keep the CST alive through lowering (CST-driven lowering reads it); the rest
	 * of the parse result is freed now. */
	SyntaxNode *cst_root = parse_result.cst_root;
	parse_result.cst_root = NULL;
	parse_result_free(&parse_result);

	/* Empty-program check is now CST-based: the program is empty unless the CST
	 * root carries at least one (non-error) declaration node. */
	if (!cst_root_has_decl(cst_root)) {
		fprintf(stderr, "Error: Empty program\n");
		free(source);
		return 1;
	}

	/* Resolve use declarations (module loading): register each module's CST with the CST
	 * analyzer + lowerer, which inline + name-prefix it. Tuple-group flattening for archetype
	 * fields happens inside those CST passes too, so no AstProgram pre-pass is needed. */
	resolve_uses(cst_root, source, source_path);

	/* Semantic analysis: reconstruct the abstract AST from the lossless CST (+ registered
	 * module CSTs) and analyze that (rustc/Go-style: CST → AST → check). The parser-built
	 * AstProgram is not consulted. The side model (keyed by CST node id) feeds lowering. */
	SemanticContext *sem_ctx = semantic_analyze_cst(cst_root, source);

	if (!sem_ctx || semantic_has_errors(sem_ctx)) {
		fprintf(stderr, "Semantic analysis failed\n");
		fflush(stderr);
		if (sem_ctx)
			semantic_context_free(sem_ctx);
		free(source);
		return 1;
	}

	/* Lower the lossless CST → AST (the only lowering path). Resolved types come from the
	 * semantic side model (keyed by CST node id, globally unique across inlined modules);
	 * `use` modules are inlined from their registered CSTs (see resolve_uses / lower_add_module). */
	lower_set_model(sem_context_model(sem_ctx));
	lower_set_sem(sem_ctx);
	HirProgram *ast = lower_to_hir(cst_root, source);

	/* Code generation */
	CodegenContext *codegen_ctx = codegen_create(ast, sem_ctx);

	/* All exits past this point flow through `cleanup:` so the toolchain temp
	 * files, the work dir, and the IR/AST/CST allocations are released exactly
	 * once. rc stays 1 until a path proves success. */
	int rc = 1;

	/* Non-emit builds put every intermediate in a private mkdtemp work dir
	 * (mode 0700, unpredictable name) — so concurrent compiles never collide on
	 * a shared /tmp/arche_<pid> path and there is no predictable-name symlink
	 * race. emit-llvm writes straight to the caller's out_path. */
	const char *ir_file;
	char temp_ir[512] = "", opt_file[512] = "", asm_file[512] = "";
	char workdir[] = "/tmp/arche_XXXXXX";
	int have_workdir = 0;
	if (emit_llvm) {
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
		ir_file = temp_ir;
	}

	FILE *ir_output = fopen(ir_file, "w");
	if (!ir_output) {
		perror("Failed to open IR output file");
		goto cleanup;
	}
	codegen_generate(codegen_ctx, ir_output);
	fclose(ir_output);
	if (!quiet)
		printf("Generated LLVM IR: %s\n", ir_file);

	if (emit_llvm) {
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
	 * `<4 x double>` lower to ymm-wide vmulpd/vfmadd without pinning to this CPU. */
	{
		char llc_cmd[1024];
		int m =
		    snprintf(llc_cmd, sizeof(llc_cmd), "llc -code-model=large -mcpu=x86-64-v3 -o %s %s", asm_file, opt_file);
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

	/* cc: assemble + link the runtime objects (and any --link inputs). */
	{
		char cc_cmd[8192];
		int cc_len =
		    snprintf(cc_cmd, sizeof(cc_cmd),
		             "cc -no-pie -mcmodel=large -o %s %s " ARCHE_RUNTIME_DIR "/stack_check.o " ARCHE_RUNTIME_DIR
		             "/io.o " ARCHE_RUNTIME_DIR "/net.o " ARCHE_RUNTIME_DIR "/term.o -lc",
		             out_path, asm_file);
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
	/* AST must be freed before CST (HIR_TYPE_NAMED ptrs reference into the CST). */
	codegen_free(codegen_ctx);
	semantic_context_free(sem_ctx);
	hir_program_free(ast);
	free(source);
	return rc;
}

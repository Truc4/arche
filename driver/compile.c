#include "compile.h"
#include "../codegen/codegen.h"
#include "../lexer/lexer.h"
#include "../lower/lower.h"
#include "../parser/parser.h"
#include "../semantic/sem_diagnostics.h"
#include "../semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef ARCHE_CORE_DIR
#define ARCHE_CORE_DIR "core"
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

/* Resolve `use foo;` declarations from the CST: for each, locate the module file, parse it,
 * and register its lossless CST with the lowerer and the CST analyzer (both inline + name-
 * prefix the module themselves, so nothing is inlined into a AstProgram here). The module's CST
 * + source are kept alive — the registries borrow them. */
static void resolve_uses(const SyntaxNode *cst_root, const char *src, const char *source_path) {
	if (!cst_root)
		return;
	for (int u = 0; u < cst_root->child_count; u++) {
		if (cst_root->children[u].tag != SE_NODE || cst_root->children[u].as.node->kind != SN_USE_DECL)
			continue;
		const SyntaxNode *ud = cst_root->children[u].as.node;

		/* module name = the IDENT token in `use <name> ;` (`use` itself is TOK_USE) */
		char mod_name[256];
		mod_name[0] = '\0';
		for (int k = 0; k < ud->child_count; k++)
			if (ud->children[k].tag == SE_TOKEN && ud->children[k].as.token.kind == TOK_IDENT) {
				size_t L = ud->children[k].as.token.length;
				if (L > sizeof(mod_name) - 1)
					L = sizeof(mod_name) - 1;
				memcpy(mod_name, src + ud->children[k].as.token.offset, L);
				mod_name[L] = '\0';
				break;
			}
		if (!mod_name[0])
			continue;

		/* Try the source file's directory first, then the core library. */
		char *source_dir = source_dir_of(source_path);
		char path1[512], path2[512];
		snprintf(path1, sizeof(path1), "%s/%s.arche", source_dir, mod_name);
		free(source_dir);
		snprintf(path2, sizeof(path2), "%s/%s.arche", ARCHE_CORE_DIR, mod_name);
		const char *found = file_exists(path1) ? path1 : (file_exists(path2) ? path2 : NULL);
		if (!found) {
			fprintf(stderr, "Error: Module not found: %s\n", mod_name);
			continue;
		}

		char *mod_src = read_file_optional(found);
		if (!mod_src) {
			fprintf(stderr, "Error: Failed to open module: %s\n", found);
			continue;
		}

		/* Parse without prepending core (avoid duplicate declarations). */
		ParseResult mod_parse = parse_source(mod_src);
		if (mod_parse.error_count > 0 || !mod_parse.cst_root) {
			fprintf(stderr, "Error: Failed to parse module %s\n", mod_name);
			for (size_t j = 0; j < mod_parse.error_count; j++)
				fprintf(stderr, "  [Line %d] %s\n", mod_parse.errors[j].line, mod_parse.errors[j].message);
			parse_result_free(&mod_parse);
			free(mod_src);
			continue;
		}

		/* Register the module CST with both back-ends; they borrow the CST + source (leaf
		 * spans reference it), so transfer ownership out of the parse result and keep it. */
		lower_add_module(mod_name, mod_parse.cst_root, mod_src);
		semantic_add_module(mod_name, mod_parse.cst_root, mod_src);
		mod_parse.cst_root = NULL;
		parse_result_free(&mod_parse); /* frees the module's parser-built AstProgram */
	}
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

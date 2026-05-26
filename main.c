#include "codegen/codegen.h"
#include "lexer/lexer.h"
#include "lower/lower.h"
#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ARCHE_CORE_DIR
#define ARCHE_CORE_DIR "core"
#endif

#ifndef ARCHE_RUNTIME_DIR
#define ARCHE_RUNTIME_DIR "build/runtime"
#endif

static char *read_file_optional(const char *path);

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
 * (any SN_*_DECL). Replaces the old `prog->decl_count == 0` check now that the
 * parser builds no AstProgram. A malformed top-level form is wrapped SN_ERROR (parse
 * errors are reported earlier), so it doesn't count as a declaration. */
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

static void usage(const char *prog) {
	fprintf(stderr, "Usage: %s [-o executable] [--link <path>] input.arche\n", prog);
	fprintf(stderr, "       %s [-emit-llvm -o output.ll] input.arche\n", prog);
	fprintf(stderr, "       --link <path>  Pass additional .c or .o file to cc at link time\n");
	exit(1);
}

/* Maximum number of --link paths accepted on one command line */
#define MAX_LINK_PATHS 32

int main(int argc, char *argv[]) {
	const char *input_file = NULL;
	const char *output_file = NULL;
	int emit_llvm = 0;

	/* Extra files to pass to cc at link time (--link <path>) */
	const char *link_paths[MAX_LINK_PATHS];
	int link_count = 0;

	/* Lint config — both on by default; CLI can disable or promote to errors. */
	int lint_pcbf_enabled = 1, lint_pcbf_werror = 0;
	int lint_pne_enabled = 1, lint_pne_werror = 0;

	/* Parse command-line arguments */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0) {
			if (i + 1 >= argc) {
				usage(argv[0]);
			}
			output_file = argv[++i];
		} else if (strcmp(argv[i], "--link") == 0) {
			if (i + 1 >= argc) {
				usage(argv[0]);
			}
			if (link_count < MAX_LINK_PATHS) {
				link_paths[link_count++] = argv[++i];
			} else {
				fprintf(stderr, "Error: too many --link arguments (max %d)\n", MAX_LINK_PATHS);
				return 1;
			}
		} else if (strcmp(argv[i], "-emit-llvm") == 0) {
			emit_llvm = 1;
		} else if (strcmp(argv[i], "-Wno-proc-could-be-func") == 0) {
			lint_pcbf_enabled = 0;
		} else if (strcmp(argv[i], "-Wno-proc-no-effect") == 0) {
			lint_pne_enabled = 0;
		} else if (strcmp(argv[i], "-Werror=proc-could-be-func") == 0) {
			lint_pcbf_werror = 1;
		} else if (strcmp(argv[i], "-Werror=proc-no-effect") == 0) {
			lint_pne_werror = 1;
		} else if (strcmp(argv[i], "-Werror") == 0) {
			lint_pcbf_werror = 1;
			lint_pne_werror = 1;
		} else if (argv[i][0] != '-') {
			input_file = argv[i];
		}
	}

	semantic_set_lint_proc_could_be_func(lint_pcbf_enabled, lint_pcbf_werror);
	semantic_set_lint_proc_no_effect(lint_pne_enabled, lint_pne_werror);

	if (!input_file) {
		usage(argv[0]);
	}

	/* Limit memory to 512MB to prevent runaway compilation */
	struct rlimit mem_limit;
	mem_limit.rlim_cur = 512 * 1024 * 1024;
	mem_limit.rlim_max = 512 * 1024 * 1024;
	int limit_result = setrlimit(RLIMIT_AS, &mem_limit);
	if (limit_result != 0) {
		perror("Error: Could not set memory limit");
		return 1;
	}

	if (!output_file) {
		/* Default output: build/basename without extension */
		const char *base = strrchr(input_file, '/');
		if (!base)
			base = input_file;
		else
			base++;

		int len = strlen(base) + 20;
		output_file = malloc(len);
		strcpy((char *)output_file, "build/");
		strcat((char *)output_file, base);
		char *dot = strrchr((char *)output_file, '.');
		if (dot) {
			strcpy(dot, "");
		}
	}

	/* Read input file */
	FILE *input = fopen(input_file, "r");
	if (!input) {
		perror("Failed to open input file");
		return 1;
	}

	fseek(input, 0, SEEK_END);
	long file_size = ftell(input);
	fseek(input, 0, SEEK_SET);

	char *source = malloc(file_size + 1);
	if (fread(source, 1, file_size, input) != (size_t)file_size) {
		perror("Failed to read input file");
		free(source);
		fclose(input);
		return 1;
	}
	source[file_size] = '\0';
	fclose(input);

	/* Load and prepend core library */
	char core_path[512];
	snprintf(core_path, sizeof(core_path), "%s/core.arche", ARCHE_CORE_DIR);
	char *core_src = read_file_optional(core_path);

	char *combined_source = NULL;
	if (core_src && strlen(core_src) > 0) {
		/* Combine: core + user source */
		combined_source = malloc(strlen(core_src) + strlen(source) + 1);
		strcpy(combined_source, core_src);
		strcat(combined_source, source);
		free(core_src);
		free(source);
		source = combined_source;
	} else if (core_src) {
		free(core_src); /* core.arche was empty */
	}

	/* Lexical analysis and parsing */
	ParseResult parse_result = parse_source(source);

	if (parse_result.error_count > 0) {
		for (size_t i = 0; i < parse_result.error_count; i++) {
			fprintf(stderr, "[Line %d, Col %d] Error: %s\n", parse_result.errors[i].line, parse_result.errors[i].column,
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
	resolve_uses(cst_root, source, input_file);

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

	/* Generate LLVM IR to temporary file or specified output */
	const char *ir_file;
	char temp_ir[256] = "";
	if (emit_llvm) {
		ir_file = output_file;
	} else {
		snprintf(temp_ir, sizeof(temp_ir), "/tmp/arche_%d.ll", (int)getpid());
		ir_file = temp_ir;
	}

	FILE *ir_output = fopen(ir_file, "w");
	if (!ir_output) {
		perror("Failed to open IR output file");
		codegen_free(codegen_ctx);
		semantic_context_free(sem_ctx);
		hir_program_free(ast);
		free(source);
		return 1;
	}

	codegen_generate(codegen_ctx, ir_output);
	fclose(ir_output);

	printf("Generated LLVM IR: %s\n", ir_file);

	/* If emit-llvm flag, we're done */
	if (emit_llvm) {
		codegen_free(codegen_ctx);
		semantic_context_free(sem_ctx);
		hir_program_free(ast);
		free(source);
		return 0;
	}

	/* Compile IR to executable using opt, llc, and cc */
	char opt_file[256], asm_file[256];
	snprintf(opt_file, sizeof(opt_file), "/tmp/arche_%d_opt.ll", (int)getpid());
	snprintf(asm_file, sizeof(asm_file), "/tmp/arche_%d.s", (int)getpid());

	/* Run `opt -O2` first to promote allocas to SSA (mem2reg), do basic CSE,
	 * loop-invariant code motion, etc. The codegen emits stack-resident loop
	 * counters and accumulators because that's easier than building SSA
	 * directly; opt cleans them up before llc sees them. */
	char opt_cmd[512];
	/* `-mcpu=x86-64-v3` matches llc's target so the loop vectorizer (which runs here in opt)
	 * uses the AVX2 vector width instead of the generic 2-wide default. */
	snprintf(opt_cmd, sizeof(opt_cmd), "opt -O2 -mcpu=x86-64-v3 -S -o %s %s", opt_file, ir_file);
	printf("Optimizing IR...\n");
	int ret = system(opt_cmd);
	if (ret != 0) {
		fprintf(stderr, "Failed to optimize LLVM IR\n");
		unlink(ir_file);
		codegen_free(codegen_ctx);
		semantic_context_free(sem_ctx);
		hir_program_free(ast);
		free(source);
		return 1;
	}

	/* Call llc to generate assembly. -mcpu=x86-64-v3 = portable AVX2 baseline
	 * (Haswell-era and newer): AVX, AVX2, BMI, BMI2, F16C, FMA, LZCNT, MOVBE,
	 * XSAVE. Lets `<4 x double>` IR lower to ymm-wide vmulpd/vfmadd instead of
	 * paired SSE2 mulpd, without tying the binary to the dev-machine CPU. */
	char llc_cmd[512];
	snprintf(llc_cmd, sizeof(llc_cmd), "llc -code-model=large -mcpu=x86-64-v3 -o %s %s", asm_file, opt_file);
	printf("Compiling to assembly...\n");
	ret = system(llc_cmd);
	if (ret != 0) {
		fprintf(stderr, "Failed to compile LLVM IR to assembly\n");
		/* Copy IR for debugging */
		char debug_copy[256];
		snprintf(debug_copy, sizeof(debug_copy), "cp %s tests/tmp/ir_debug.ll 2>/dev/null || cp %s /tmp/debug.ll",
		         ir_file, ir_file);
		system(debug_copy);
		unlink(ir_file);
		codegen_free(codegen_ctx);
		semantic_context_free(sem_ctx);
		hir_program_free(ast);
		free(source);
		return 1;
	}

	/* Call cc to assemble and link with runtime objects */
	/* Base command: fixed runtime objects */
	char cc_cmd[4096];
	int cc_len = snprintf(cc_cmd, sizeof(cc_cmd),
	                      "cc -no-pie -mcmodel=large -o %s %s " ARCHE_RUNTIME_DIR "/stack_check.o " ARCHE_RUNTIME_DIR
	                      "/io.o " ARCHE_RUNTIME_DIR "/net.o " ARCHE_RUNTIME_DIR "/term.o "
	                      "-lc",
	                      output_file, asm_file);

	/* Append any --link paths supplied on the command line */
	for (int li = 0; li < link_count && cc_len < (int)sizeof(cc_cmd) - 1; li++) {
		cc_len += snprintf(cc_cmd + cc_len, sizeof(cc_cmd) - (size_t)cc_len, " %s", link_paths[li]);
	}

	printf("Linking executable...\n");
	ret = system(cc_cmd);
	if (ret != 0) {
		fprintf(stderr, "Failed to link executable\n");
		unlink(ir_file);
		unlink(asm_file);
		codegen_free(codegen_ctx);
		semantic_context_free(sem_ctx);
		hir_program_free(ast);
		free(source);
		return 1;
	}

	printf("Successfully generated executable: %s\n", output_file);

	/* Cleanup temporary files (save IR for inspection during development) */
	char save_ir[256];
	snprintf(save_ir, sizeof(save_ir), "cp %s tests/tmp/ir_last.ll 2>/dev/null", ir_file);
	system(save_ir);
	unlink(ir_file);
	unlink(asm_file);

	/* Cleanup — AST must be freed before CST (HIR_TYPE_NAMED ptrs into CST) */
	codegen_free(codegen_ctx);
	semantic_context_free(sem_ctx);
	hir_program_free(ast);
	free(source);

	return 0;
}

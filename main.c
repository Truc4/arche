#include "codegen/codegen.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ARCHE_CORE_DIR
#define ARCHE_CORE_DIR "core"
#endif

#ifndef ARCHE_RUNTIME_DIR
#define ARCHE_RUNTIME_DIR "build/runtime"
#endif

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
	fprintf(stderr, "Usage: %s [-o executable] input.arche\n", prog);
	fprintf(stderr, "       %s [-emit-llvm -o output.ll] input.arche\n", prog);
	exit(1);
}

int main(int argc, char *argv[]) {
	const char *input_file = NULL;
	const char *output_file = NULL;
	int emit_llvm = 0;

	/* Parse command-line arguments */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0) {
			if (i + 1 >= argc) {
				usage(argv[0]);
			}
			output_file = argv[++i];
		} else if (strcmp(argv[i], "-emit-llvm") == 0) {
			emit_llvm = 1;
		} else if (argv[i][0] != '-') {
			input_file = argv[i];
		}
	}

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
		Program *prog = parse_result.ast;
		parse_result_free(&parse_result);
		program_free(prog);
		free(source);
		return 1;
	}

	Program *prog = parse_result.ast;
	parse_result_free(&parse_result);

	if (!prog || prog->decl_count == 0) {
		fprintf(stderr, "Error: Empty program\n");
		if (prog)
			program_free(prog);
		free(source);
		return 1;
	}

	/* Semantic analysis */
	SemanticContext *sem_ctx = semantic_analyze(prog);

	if (!sem_ctx || semantic_has_errors(sem_ctx)) {
		fprintf(stderr, "Semantic analysis failed\n");
		fflush(stderr);
		if (sem_ctx)
			semantic_context_free(sem_ctx);
		program_free(prog);
		free(source);
		return 1;
	}

	/* Code generation */
	CodegenContext *codegen_ctx = codegen_create(prog, sem_ctx);

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
		program_free(prog);
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
		program_free(prog);
		free(source);
		return 0;
	}

	/* Compile IR to executable using llc and cc */
	char asm_file[256];
	snprintf(asm_file, sizeof(asm_file), "/tmp/arche_%d.s", (int)getpid());

	/* Call llc to generate assembly */
	char llc_cmd[512];
	snprintf(llc_cmd, sizeof(llc_cmd), "llc -o %s %s", asm_file, ir_file);
	printf("Compiling to assembly...\n");
	int ret = system(llc_cmd);
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
		program_free(prog);
		free(source);
		return 1;
	}

	/* Call cc to assemble and link with runtime objects */
	char cc_cmd[1024];
	snprintf(cc_cmd, sizeof(cc_cmd), "cc -no-pie -o %s %s " ARCHE_RUNTIME_DIR "/stack_check.o " ARCHE_RUNTIME_DIR "/io.o -lc", output_file,
	         asm_file);
	printf("Linking executable...\n");
	ret = system(cc_cmd);
	if (ret != 0) {
		fprintf(stderr, "Failed to link executable\n");
		unlink(ir_file);
		unlink(asm_file);
		codegen_free(codegen_ctx);
		semantic_context_free(sem_ctx);
		program_free(prog);
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

	/* Cleanup */
	codegen_free(codegen_ctx);
	semantic_context_free(sem_ctx);
	program_free(prog);
	free(source);

	return 0;
}

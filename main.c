#include "lexer/lexer.h"
#include "parser/parser.h"
#include "semantic/semantic.h"
#include "codegen/codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

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

	if (!output_file) {
		/* Default output: replace .arche with executable name */
		int len = strlen(input_file);
		output_file = malloc(len + 10);
		strcpy((char *)output_file, input_file);
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

	/* Lexical analysis and parsing */
	Lexer lexer;
	lexer_init(&lexer, source);

	Parser parser;
	parser_init(&parser, &lexer);

	Program *prog = parse_program(&parser);

	if (!prog || parser.had_error) {
		fprintf(stderr, "Parsing failed\n");
		if (prog) program_free(prog);
		free(source);
		return 1;
	}

	/* Semantic analysis */
	SemanticContext *sem_ctx = semantic_analyze(prog);

	if (semantic_has_errors(sem_ctx)) {
		fprintf(stderr, "Semantic analysis failed\n");
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
		unlink(ir_file);
		codegen_free(codegen_ctx);
		semantic_context_free(sem_ctx);
		program_free(prog);
		free(source);
		return 1;
	}

	/* Call cc to assemble and link */
	char cc_cmd[512];
	snprintf(cc_cmd, sizeof(cc_cmd), "cc -o %s %s", output_file, asm_file);
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

	/* Cleanup temporary files */
	unlink(ir_file);
	unlink(asm_file);

	/* Cleanup */
	codegen_free(codegen_ctx);
	semantic_context_free(sem_ctx);
	program_free(prog);
	free(source);

	return 0;
}

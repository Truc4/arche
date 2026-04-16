CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -DARCHE_CORE_DIR=\"$(abspath core)\"
TARGET = arche
LEXER_BIN = lexer-bin
PARSER_TEST_BIN = parser-test
FMT_BIN = arche-fmt
SEMANTIC_TEST_BIN = semantic-test
CODEGEN_TEST_BIN = codegen-test
LIBARCH = libarch.a
LIBARCH_OBJS = lexer/lexer.o ast/ast.o parser/parser.o

# Source files
SRCS = lexer/lexer.c \
       ast/ast.c \
       parser/parser.c \
       semantic/semantic.c \
       codegen/codegen.c

OBJS = $(SRCS:.c=.o)
COMPILER_OBJS = lexer/lexer.o ast/ast.o parser/parser.o semantic/semantic.o codegen/codegen.o main.o
LEXER_OBJS = lexer/lexer.o lexer/lexer_main.o
PARSER_TEST_OBJS = lexer/lexer.o ast/ast.o parser/parser.o tests/parser_tests.o
FMT_OBJS = lexer/lexer.o ast/ast.o parser/parser.o arche_fmt.o
SEMANTIC_TEST_OBJS = lexer/lexer.o ast/ast.o parser/parser.o semantic/semantic.o tests/semantic_tests.o
CODEGEN_TEST_OBJS = lexer/lexer.o ast/ast.o parser/parser.o semantic/semantic.o codegen/codegen.o tests/codegen_tests.o

# Default target
all: $(TARGET) $(LEXER_BIN) $(PARSER_TEST_BIN) $(FMT_BIN) $(SEMANTIC_TEST_BIN) $(CODEGEN_TEST_BIN) $(LIBARCH)

# Build main compiler executable
$(TARGET): $(COMPILER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Build lexer-only executable
$(LEXER_BIN): $(LEXER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Build parser tests executable
$(PARSER_TEST_BIN): $(PARSER_TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Build formatter executable
$(FMT_BIN): $(FMT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Build semantic tests executable
$(SEMANTIC_TEST_BIN): $(SEMANTIC_TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Build codegen tests executable
$(CODEGEN_TEST_BIN): $(CODEGEN_TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Build syntax/parsing library
$(LIBARCH): $(LIBARCH_OBJS)
	$(AR) rcs $@ $^

# Compile object files
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Run the compiler (produces executable)
run: $(TARGET)
	./$(TARGET) examples/stuff.arche
	@echo
	@echo "Running generated executable:"
	@./examples/stuff

# Run the lexer
run-lexer: $(LEXER_BIN)
	./$(LEXER_BIN) examples/hello.arc

# Run lexer tests
test-lexer: $(LEXER_BIN)
	./tests/run_lexer_tests.sh

# Run parser tests
test-parser: $(PARSER_TEST_BIN)
	./$(PARSER_TEST_BIN)

# Run semantic tests
test-semantic: $(SEMANTIC_TEST_BIN)
	./$(SEMANTIC_TEST_BIN)

# Run codegen unit tests
test-codegen-unit: $(CODEGEN_TEST_BIN)
	./$(CODEGEN_TEST_BIN)

# Run all tests
test: test-lexer test-parser test-semantic test-codegen-unit test-codegen

# Test code generation
test-codegen: $(TARGET)
	rm -f examples/hello_world/hello_world
	./$(TARGET) examples/hello_world/hello_world.arche
	@test -x examples/hello_world/hello_world && ./examples/hello_world/hello_world > /tmp/test_output.txt && grep -q "Hello, World!" /tmp/test_output.txt && echo "✓ Codegen test passed (hello_world)" || echo "✗ Codegen test failed"

# Clean all generated artifacts
clean:
	find . -name "*.o" -delete
	find . -name "*.a" -delete
	rm -f $(TARGET) $(LEXER_BIN) $(PARSER_TEST_BIN) $(FMT_BIN) $(SEMANTIC_TEST_BIN) $(CODEGEN_TEST_BIN)
	rm -f bench-physics bench-strings bench-lifecycle bench-mixed
	find examples/ -type f ! -name "*.c" ! -name "*.arche" ! -name "*.sh" -delete
	find tests/ -type f ! -name "*.c" ! -name "*.h" ! -name "*.sh" ! -name "*.arche" -delete
	find design_analysis/ -type f ! -name "*.c" ! -name "*.h" ! -name "*.sh" ! -name "*.arche" -delete
	rm -f *.txt test_*.sh run_*.sh 2>/dev/null || true

# Design analysis benchmarks (data-driven design decisions, not language perf)
bench-physics: design_analysis/array_ops/physics_update.c
	@mkdir -p design_analysis/array_ops/results
	$(CC) -Wall -Wextra -std=c99 -O3 -march=native -o bench-physics design_analysis/array_ops/physics_update.c -lm
	./bench-physics

bench-strings: design_analysis/string_ops/fixed_length.c
	@mkdir -p design_analysis/string_ops/results
	$(CC) -Wall -Wextra -std=c99 -O3 -o bench-strings design_analysis/string_ops/fixed_length.c
	./bench-strings

bench-lifecycle: design_analysis/array_ops/lifecycle_operations.c
	@mkdir -p design_analysis/array_ops/results
	$(CC) -Wall -Wextra -std=c99 -O3 -march=native -o bench-lifecycle design_analysis/array_ops/lifecycle_operations.c -lm
	./bench-lifecycle

bench-mixed: design_analysis/array_ops/mixed_workload.c
	@mkdir -p design_analysis/array_ops/results
	$(CC) -Wall -Wextra -std=c99 -O3 -march=native -o bench-mixed design_analysis/array_ops/mixed_workload.c -lm
	./bench-mixed

# Phony targets
.PHONY: all run run-lexer test test-lexer test-parser test-semantic test-codegen test-codegen-unit clean bench-physics bench-strings bench-lifecycle bench-mixed

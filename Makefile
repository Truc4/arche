CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -DARCHE_CORE_DIR=\"$(abspath core)\"
BUILD_DIR = build
TARGET = $(BUILD_DIR)/arche
LEXER_BIN = $(BUILD_DIR)/lexer-bin
PARSER_TEST_BIN = $(BUILD_DIR)/parser-test
FMT_BIN = $(BUILD_DIR)/arche-fmt
SEMANTIC_TEST_BIN = $(BUILD_DIR)/semantic-test
CODEGEN_TEST_BIN = $(BUILD_DIR)/codegen-test
LIBARCH = $(BUILD_DIR)/libarch.a
LIBARCH_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/ast/ast.o $(BUILD_DIR)/parser/parser.o

# Source files
SRCS = lexer/lexer.c \
       ast/ast.c \
       parser/parser.c \
       semantic/semantic.c \
       codegen/codegen.c

OBJS = $(SRCS:.c=.o)
COMPILER_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/ast/ast.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/semantic/semantic.o $(BUILD_DIR)/codegen/codegen.o $(BUILD_DIR)/main.o
LEXER_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/lexer/lexer_main.o
PARSER_TEST_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/ast/ast.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/tests/parser_tests.o
FMT_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/ast/ast.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/arche_fmt.o
SEMANTIC_TEST_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/ast/ast.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/semantic/semantic.o $(BUILD_DIR)/tests/semantic_tests.o
CODEGEN_TEST_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/ast/ast.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/semantic/semantic.o $(BUILD_DIR)/codegen/codegen.o $(BUILD_DIR)/tests/codegen_tests.o

# Default target
all: $(BUILD_DIR) $(TARGET) $(LEXER_BIN) $(PARSER_TEST_BIN) $(FMT_BIN) $(SEMANTIC_TEST_BIN) $(CODEGEN_TEST_BIN) $(LIBARCH)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)/lexer $(BUILD_DIR)/ast $(BUILD_DIR)/parser $(BUILD_DIR)/semantic $(BUILD_DIR)/codegen $(BUILD_DIR)/tests

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
$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	mkdir -p $(dir $@)
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
	LEXER_BIN=$(LEXER_BIN) ./tests/run_lexer_tests.sh

# Run parser tests
test-parser: $(PARSER_TEST_BIN)
	./$(PARSER_TEST_BIN)

# Run semantic tests
test-semantic: $(SEMANTIC_TEST_BIN)
	./$(SEMANTIC_TEST_BIN)

# Run codegen unit tests
test-codegen-unit: $(CODEGEN_TEST_BIN)
	./$(CODEGEN_TEST_BIN)

# Test all .arche files compile and run
test-arche: $(TARGET)
	@echo "Testing .arche file compilation..."
	@PASS=0; FAIL=0; ERROR=0; \
	for test_file in tests/arche/*.arche; do \
		test_name=$$(basename "$$test_file" .arche); \
		test_out="$(BUILD_DIR)/test_$$test_name"; \
		printf "Testing %-40s " "$$test_name..."; \
		\
		if ./$(TARGET) -o "$$test_out" "$$test_file" > /tmp/test_err_$$test_name 2>&1; then \
			if [ -x "$$test_out" ] && "$$test_out" > /tmp/test_run_$$test_name 2>&1; then \
				echo "✓ PASS"; \
				PASS=$$((PASS + 1)); \
			else \
				exit_code=$$?; \
				echo "✗ FAIL (runtime error: $$exit_code)"; \
				FAIL=$$((FAIL + 1)); \
				tail -5 /tmp/test_run_$$test_name | sed 's/^/    /'; \
			fi; \
		else \
			echo "⚠ ERROR (compile error)"; \
			ERROR=$$((ERROR + 1)); \
			tail -5 /tmp/test_err_$$test_name | sed 's/^/    /'; \
		fi; \
	done; \
	echo ""; \
	echo "Results: $$PASS passed, $$FAIL failed, $$ERROR errors"; \
	[ $$FAIL -eq 0 ] && [ $$ERROR -eq 0 ]

# Test example files against C reference implementations
test-examples: $(TARGET)
	@./tests/run_example_tests.sh

# Run all tests - EVERYTHING
test: $(TARGET)
	./tests/run_all_tests.sh

# Test code generation
test-codegen: $(TARGET)
	./$(TARGET) -o $(BUILD_DIR)/hello_world examples/hello_world/hello_world.arche
	@test -x $(BUILD_DIR)/hello_world && ./$(BUILD_DIR)/hello_world > /tmp/test_output.txt && grep -q "Hello, World!" /tmp/test_output.txt && echo "✓ Codegen test passed (hello_world)" || echo "✗ Codegen test failed"

# Clean all generated artifacts
clean:
	rm -rf $(BUILD_DIR)
	find examples/ -type f ! -name "*.c" ! -name "*.arche" ! -name "*.sh" -delete
	find design_analysis/ -type f ! -name "*.c" ! -name "*.h" ! -name "*.sh" ! -name "*.arche" -delete

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

# Format all Arche source files
format: $(FMT_BIN)
	for f in $$(find . -name "*.arche" -type f); do \
		if ./$(FMT_BIN) "$$f" > /tmp/fmt_tmp 2>/dev/null; then \
			mv /tmp/fmt_tmp "$$f"; \
			echo "✓ $$f"; \
		else \
			echo "✗ $$f (parse error)"; \
		fi; \
	done

.PHONY: build

# Phony targets
.PHONY: all run run-lexer test test-lexer test-parser test-semantic test-codegen test-codegen-unit test-arche test-examples clean bench-physics bench-strings bench-lifecycle bench-mixed format

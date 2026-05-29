CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -DARCHE_CORE_DIR=\"$(abspath core)\" -DARCHE_RUNTIME_DIR=\"$(abspath build/runtime)\" -DARCHE_EXPLAIN_DIR=\"$(abspath docs/explain)\"
BUILD_DIR = build
TARGET = $(BUILD_DIR)/arche
VPATH = tests
LEXER_BIN = $(BUILD_DIR)/lexer-bin
PARSER_TEST_BIN = $(BUILD_DIR)/parser-test
FMT_BIN = $(BUILD_DIR)/arche-fmt
CST_TOKENS_BIN = $(BUILD_DIR)/arche-cst-tokens
ANALYZER_BIN = $(BUILD_DIR)/arche-analyzer
CST_ROUNDTRIP_BIN = $(BUILD_DIR)/arche-cst-roundtrip
CST_VIEW_TEST_BIN = $(BUILD_DIR)/cst-view-test
SEMANTIC_TEST_BIN = $(BUILD_DIR)/semantic-test
CODEGEN_TEST_BIN = $(BUILD_DIR)/codegen-test
LOWER_TEST_BIN = $(BUILD_DIR)/lower-test
LIBARCH = $(BUILD_DIR)/libarch.a
LIBARCH_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/cst/cst.o $(BUILD_DIR)/cst/syntax_tree.o $(BUILD_DIR)/cst/cst_view.o $(BUILD_DIR)/parser/parser.o

# Source files
SRCS = lexer/lexer.c \
       cst/cst.c \
       cst/syntax_tree.c \
       cst/cst_view.c \
       cst/token_category.c \
       cst/format_cst.c \
       parser/parser.c \
       semantic/semantic.c \
       semantic/sem_model.c \
       semantic/sem_hints.c \
       semantic/sem_diagnostics.c \
       semantic/sem_types.c \
       semantic/tycheck.c \
       codegen/codegen.c

RUNTIME_SRCS = runtime/stack_check.c runtime/io.c runtime/net.c runtime/term.c
RUNTIME_OBJS = $(RUNTIME_SRCS:.c=.o)

OBJS = $(SRCS:.c=.o)
COMPILER_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/cst/cst.o $(BUILD_DIR)/cst/syntax_tree.o $(BUILD_DIR)/cst/cst_view.o $(BUILD_DIR)/hir/hir.o $(BUILD_DIR)/lower/lower.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/semantic/semantic.o $(BUILD_DIR)/semantic/sem_model.o $(BUILD_DIR)/semantic/sem_hints.o $(BUILD_DIR)/semantic/sem_diagnostics.o $(BUILD_DIR)/semantic/sem_types.o $(BUILD_DIR)/semantic/tycheck.o $(BUILD_DIR)/codegen/codegen.o $(BUILD_DIR)/main.o
LEXER_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/lexer/lexer_main.o
PARSER_TEST_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/cst/cst.o $(BUILD_DIR)/cst/syntax_tree.o $(BUILD_DIR)/cst/cst_view.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/semantic/semantic.o $(BUILD_DIR)/semantic/sem_model.o $(BUILD_DIR)/semantic/sem_hints.o $(BUILD_DIR)/semantic/sem_diagnostics.o $(BUILD_DIR)/semantic/sem_types.o $(BUILD_DIR)/semantic/tycheck.o $(BUILD_DIR)/unit/compiler/parser_tests.o
FMT_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/cst/cst.o $(BUILD_DIR)/cst/syntax_tree.o $(BUILD_DIR)/cst/cst_view.o $(BUILD_DIR)/cst/format_cst.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/arche_fmt.o
CST_TOKENS_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/cst/cst.o $(BUILD_DIR)/cst/syntax_tree.o $(BUILD_DIR)/cst/cst_view.o $(BUILD_DIR)/cst/token_category.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/arche_cst_tokens.o
ANALYZER_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/cst/cst.o $(BUILD_DIR)/cst/syntax_tree.o $(BUILD_DIR)/cst/cst_view.o $(BUILD_DIR)/cst/token_category.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/semantic/semantic.o $(BUILD_DIR)/semantic/sem_model.o $(BUILD_DIR)/semantic/sem_hints.o $(BUILD_DIR)/semantic/sem_diagnostics.o $(BUILD_DIR)/semantic/sem_types.o $(BUILD_DIR)/semantic/tycheck.o $(BUILD_DIR)/arche_analyzer.o
CST_ROUNDTRIP_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/cst/cst.o $(BUILD_DIR)/cst/syntax_tree.o $(BUILD_DIR)/cst/cst_view.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/arche_cst_roundtrip.o
CST_VIEW_TEST_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/cst/cst.o $(BUILD_DIR)/cst/syntax_tree.o $(BUILD_DIR)/cst/cst_view.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/unit/compiler/cst_view_tests.o
SEMANTIC_TEST_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/cst/cst.o $(BUILD_DIR)/cst/syntax_tree.o $(BUILD_DIR)/cst/cst_view.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/semantic/semantic.o $(BUILD_DIR)/semantic/sem_model.o $(BUILD_DIR)/semantic/sem_hints.o $(BUILD_DIR)/semantic/sem_diagnostics.o $(BUILD_DIR)/semantic/sem_types.o $(BUILD_DIR)/semantic/tycheck.o $(BUILD_DIR)/unit/compiler/semantic_tests.o
CODEGEN_TEST_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/cst/cst.o $(BUILD_DIR)/cst/syntax_tree.o $(BUILD_DIR)/cst/cst_view.o $(BUILD_DIR)/hir/hir.o $(BUILD_DIR)/lower/lower.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/semantic/semantic.o $(BUILD_DIR)/semantic/sem_model.o $(BUILD_DIR)/semantic/sem_hints.o $(BUILD_DIR)/semantic/sem_diagnostics.o $(BUILD_DIR)/semantic/sem_types.o $(BUILD_DIR)/semantic/tycheck.o $(BUILD_DIR)/codegen/codegen.o $(BUILD_DIR)/unit/compiler/codegen_tests.o
LOWER_TEST_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/cst/cst.o $(BUILD_DIR)/cst/syntax_tree.o $(BUILD_DIR)/cst/cst_view.o $(BUILD_DIR)/hir/hir.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/semantic/semantic.o $(BUILD_DIR)/semantic/sem_model.o $(BUILD_DIR)/semantic/sem_hints.o $(BUILD_DIR)/semantic/sem_diagnostics.o $(BUILD_DIR)/semantic/sem_types.o $(BUILD_DIR)/semantic/tycheck.o $(BUILD_DIR)/lower/lower.o $(BUILD_DIR)/unit/compiler/lower_tests.o

# Default target
all: $(BUILD_DIR) $(TARGET) $(LEXER_BIN) $(PARSER_TEST_BIN) $(FMT_BIN) $(CST_TOKENS_BIN) $(ANALYZER_BIN) $(SEMANTIC_TEST_BIN) $(CODEGEN_TEST_BIN) $(LOWER_TEST_BIN) $(LIBARCH) $(BUILD_DIR)/runtime/stack_check.o $(BUILD_DIR)/runtime/io.o $(BUILD_DIR)/runtime/net.o $(BUILD_DIR)/runtime/term.o

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)/lexer $(BUILD_DIR)/cst $(BUILD_DIR)/hir $(BUILD_DIR)/lower $(BUILD_DIR)/parser $(BUILD_DIR)/semantic $(BUILD_DIR)/codegen $(BUILD_DIR)/unit/compiler $(BUILD_DIR)/runtime

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

# Build CST token dumper (powers editor syntax highlighting)
$(CST_TOKENS_BIN): $(CST_TOKENS_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Build the editor analysis service (powers inlay hints + diagnostics)
$(ANALYZER_BIN): $(ANALYZER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Build CST round-trip verifier (proves the CST is lossless)
$(CST_ROUNDTRIP_BIN): $(CST_ROUNDTRIP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Build CST view-layer unit tests
$(CST_VIEW_TEST_BIN): $(CST_VIEW_TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Build semantic tests executable
$(SEMANTIC_TEST_BIN): $(SEMANTIC_TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Build codegen tests executable
$(CODEGEN_TEST_BIN): $(CODEGEN_TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Build lower tests executable
$(LOWER_TEST_BIN): $(LOWER_TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Build syntax/parsing library
$(LIBARCH): $(LIBARCH_OBJS)
	$(AR) rcs $@ $^

# Compile object files. -MMD -MP emits a .d alongside each .o listing the headers
# it includes, so editing a header (e.g. cst.h struct, lexer.h enum) rebuilds the
# dependent objects instead of leaving stale, mismatched layouts.
$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

# Pull in the generated header-dependency files (none on a clean build).
-include $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)

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

# Run lower tests
test-lower: $(LOWER_TEST_BIN)
	./$(LOWER_TEST_BIN)

# Run all tests with LIT
test: $(TARGET) $(PARSER_TEST_BIN) $(SEMANTIC_TEST_BIN) $(CODEGEN_TEST_BIN) $(BUILD_DIR)/runtime/stack_check.o $(BUILD_DIR)/runtime/io.o $(BUILD_DIR)/runtime/net.o $(BUILD_DIR)/runtime/term.o
	lit -v tests/

# Test folder with pattern: make test-folder FOLDER=path PATTERN="*.arche"
test-folder: $(TARGET) $(BUILD_DIR)
	@if [ -z "$(FOLDER)" ]; then echo "Usage: make test-folder FOLDER=path [PATTERN='*.arche']"; exit 1; fi
	@mkdir -p $(BUILD_DIR)/tests
	@PATTERN="$${PATTERN:=*.arche}"; \
	for f in $(FOLDER)$$PATTERN; do \
		echo "Testing $$f..."; \
		./$(TARGET) -o $(BUILD_DIR)/tests/test_arche "$$f" 2>&1 | tail -1; \
		$(BUILD_DIR)/tests/test_arche 2>&1 || echo "FAILED: $$f"; \
	done

# Test code generation
test-codegen: $(TARGET)
	./$(TARGET) -o $(BUILD_DIR)/hello_world examples/hello_world/hello_world.arche
	@test -x $(BUILD_DIR)/hello_world && ./$(BUILD_DIR)/hello_world > /tmp/test_output.txt && grep -q "Hello, World!" /tmp/test_output.txt && echo "✓ Codegen test passed (hello_world)" || echo "✗ Codegen test failed"

# Clean all generated artifacts.
# Does NOT delete benchmark CSVs — those are expensive to regenerate (15-20 min for 100M rows).
# Use `make clean-data` separately when you want to free disk space.
clean:
	rm -rf $(BUILD_DIR)
	find examples/ -type f ! -name "*.c" ! -name "*.arche" ! -name "*.sh" -delete
	find design_analysis/ -type d -name __pycache__ -exec rm -rf {} + 2>/dev/null || true

# Delete benchmark datasets. Run explicitly when you want to reclaim disk space.
clean-data:
	find design_analysis/benchmarks/etl/data/ -type f -name "*.csv" -delete
	find design_analysis/benchmarks/systems/data/ -type f -name "*.csv" -delete 2>/dev/null || true

# Design analysis benchmarks (data-driven design decisions, not language perf)
bench-physics: design_analysis/array_ops/physics_update.c
	@mkdir -p design_analysis/array_ops/results
	$(CC) -Wall -Wextra -std=c99 -O3 -march=native -o $(BUILD_DIR)/bench-physics design_analysis/array_ops/physics_update.c -lm
	./$(BUILD_DIR)/bench-physics

bench-strings: design_analysis/string_ops/fixed_length.c
	@mkdir -p design_analysis/string_ops/results
	$(CC) -Wall -Wextra -std=c99 -O3 -o $(BUILD_DIR)/bench-strings design_analysis/string_ops/fixed_length.c
	./$(BUILD_DIR)/bench-strings

bench-lifecycle: design_analysis/array_ops/lifecycle_operations.c
	@mkdir -p design_analysis/array_ops/results
	$(CC) -Wall -Wextra -std=c99 -O3 -march=native -o $(BUILD_DIR)/bench-lifecycle design_analysis/array_ops/lifecycle_operations.c -lm
	./$(BUILD_DIR)/bench-lifecycle

bench-mixed: design_analysis/array_ops/mixed_workload.c
	@mkdir -p design_analysis/array_ops/results
	$(CC) -Wall -Wextra -std=c99 -O3 -march=native -o $(BUILD_DIR)/bench-mixed design_analysis/array_ops/mixed_workload.c -lm
	./$(BUILD_DIR)/bench-mixed

# Format all Arche source files and the compiler's C/H sources.
# Skips Python venv / site-packages directories so we don't try to format
# numpy/pyarrow's bundled C headers.
format: $(FMT_BIN)
	for f in $$(find . -name "*.arche" -type f \
	             -not -path "*/.venv/*" \
	             -not -path "*/site-packages/*" \
	             -not -path "*/__pycache__/*"); do \
		tmp=$$(mktemp --suffix=.arche); \
		if timeout 5 ./$(FMT_BIN) "$$f" > "$$tmp" 2>/dev/null \
		   && timeout 5 ./$(FMT_BIN) "$$tmp" > /dev/null 2>&1; then \
			mv "$$tmp" "$$f"; \
			echo "✓ $$f"; \
		else \
			rm -f "$$tmp"; \
			echo "✗ $$f (parse error or output would not round-trip — left unchanged)"; \
		fi; \
	done
	for f in $$(find . \( -name "*.c" -o -name "*.h" \) -type f \
	             -not -path "./build/*" \
	             -not -path "*/.venv/*" \
	             -not -path "*/site-packages/*" \
	             -not -path "*/__pycache__/*" \
	             -not -path "./tests/known_failures/*"); do \
		clang-format -i "$$f"; \
		echo "✓ $$f"; \
	done

.PHONY: build

# Verify the CST is lossless: every .arche file reconstructs byte-for-byte from the CST.
CORPUS = $(shell find . -name "*.arche" -type f \
	-not -path "./build/*" -not -path "*/.venv/*" -not -path "*/site-packages/*" \
	-not -path "*/__pycache__/*" -not -path "./tests/known_failures/*")
verify-cst: $(CST_ROUNDTRIP_BIN)
	@./$(CST_ROUNDTRIP_BIN) $(CORPUS) \
		&& echo "verify-cst: all $(words $(CORPUS)) files round-trip losslessly"

# CST view-layer unit tests
test-cst-view: $(CST_VIEW_TEST_BIN)
	@./$(CST_VIEW_TEST_BIN)

# Guard against silent codegen changes: emit LLVM IR for representative programs and
# diff against checked-in goldens (first run captures them). VERIFY_CG_PROGRAMS is the
# fixed representative set; see tests/codegen_golden/.
VERIFY_CG_DIR = tests/codegen_golden
VERIFY_CG_PROGRAMS = \
	examples/simple/simple.arche \
	examples/with_params/with_params.arche \
	examples/hello_world/hello_world.arche \
	examples/simple_with_print/simple_with_print.arche \
	examples/archetype/test_archetype.arche \
	examples/archetype/test_archetype_verbose.arche \
	examples/archetype/multidim_example.arche \
	tests/unit/language/use/use_simple.arche \
	tests/unit/language/csv/csv_load_named.arche \
	tests/unit/language/tuples/tuple_array_ops.arche \
	tests/unit/language/tuples/tuple_compound_assign.arche
verify-codegen: $(TARGET)
	@mkdir -p $(VERIFY_CG_DIR); fail=0; \
	for f in $(VERIFY_CG_PROGRAMS); do \
		base=$$(basename $$f .arche); ir=/tmp/cg_$$base.ll; \
		if ! ./$(TARGET) -emit-llvm -o $$ir $$f >/dev/null 2>&1; then \
			echo "verify-codegen: $$f failed to emit IR"; fail=1; continue; fi; \
		if [ -f $(VERIFY_CG_DIR)/$$base.ll ]; then \
			if ! diff -q $(VERIFY_CG_DIR)/$$base.ll $$ir >/dev/null; then \
				echo "CODEGEN DIFF: $$f"; diff $(VERIFY_CG_DIR)/$$base.ll $$ir | head -20; fail=1; fi; \
		else cp $$ir $(VERIFY_CG_DIR)/$$base.ll; echo "golden captured: $$base"; fi; \
	done; \
	[ $$fail -eq 0 ] && echo "verify-codegen: IR matches golden" || (echo "verify-codegen: FAILED"; exit 1)

# Phony targets
.PHONY: all run run-lexer test test-lexer test-parser test-semantic test-codegen test-codegen-unit test-lit test-lower clean clean-data bench-physics bench-strings bench-lifecycle bench-mixed format verify-cst verify-codegen

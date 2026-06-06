CC = gcc
ARCHE_VERSION := $(shell cat VERSION 2>/dev/null || echo 0.0.0-dev)
CFLAGS = -Wall -Wextra -Werror -std=c99 -DARCHE_CORE_DIR=\"$(abspath core)\" -DARCHE_STDLIB_DIR=\"$(abspath stdlib)\" -DARCHE_RUNTIME_DIR=\"$(abspath build/runtime)\" -DARCHE_EXPLAIN_DIR=\"$(abspath docs/explain)\" -DARCHE_VERSION=\"$(ARCHE_VERSION)\"
BUILD_DIR = build
TARGET = $(BUILD_DIR)/arche
VPATH = tests
LEXER_BIN = $(BUILD_DIR)/lexer-bin
FMT_BIN = $(BUILD_DIR)/arche-fmt
SYNTAX_TOKENS_BIN = $(BUILD_DIR)/arche-syntax-tokens
ANALYZER_BIN = $(BUILD_DIR)/arche-analyzer
SYNTAX_ROUNDTRIP_BIN = $(BUILD_DIR)/arche-syntax-roundtrip
SYNTAX_VIEW_TEST_BIN = $(BUILD_DIR)/syntax-view-test
SEMANTIC_TEST_BIN = $(BUILD_DIR)/semantic-test
CODEGEN_TEST_BIN = $(BUILD_DIR)/codegen-test
LOWER_TEST_BIN = $(BUILD_DIR)/lower-test
LIBARCH = $(BUILD_DIR)/libarch.a
LIBARCH_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/syntax/type_ref.o $(BUILD_DIR)/syntax/syntax_tree.o $(BUILD_DIR)/syntax/syntax_view.o $(BUILD_DIR)/parser/parser.o

# Source files
SRCS = lexer/lexer.c \
       syntax/type_ref.c \
       syntax/syntax_tree.c \
       syntax/syntax_view.c \
       syntax/token_category.c \
       syntax/format_syntax.c \
       parser/parser.c \
       compile/compile.c \
       doctest/doctest_extract.c \
       doctest/doctest_run.c \
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
# CLI multitool: dispatch + table-driven arg parser + one object per subcommand.
CLI_OBJS = $(BUILD_DIR)/cli/args.o $(BUILD_DIR)/cli/cli.o $(BUILD_DIR)/cli/resource.o $(BUILD_DIR)/cli/cmd_build.o $(BUILD_DIR)/cli/cmd_run.o $(BUILD_DIR)/cli/cmd_check.o $(BUILD_DIR)/cli/cmd_test.o $(BUILD_DIR)/cli/cmd_fmt.o $(BUILD_DIR)/cli/cmd_explain.o $(BUILD_DIR)/cli/cmd_analyze.o $(BUILD_DIR)/cli/cmd_completion.o $(BUILD_DIR)/cli/cmd_version.o $(BUILD_DIR)/cli/cmd_init.o $(BUILD_DIR)/cli/cmd_fill.o
# Satellite tools folded into the `arche` binary as subcommands (fmt, analyze): their objects join
# the main link. The standalone arche-fmt / arche-analyzer binaries still build during the migration.
FOLD_OBJS = $(BUILD_DIR)/syntax/format_syntax.o $(BUILD_DIR)/syntax/token_category.o $(BUILD_DIR)/arche_analyzer.o
COMPILER_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/syntax/type_ref.o $(BUILD_DIR)/syntax/syntax_tree.o $(BUILD_DIR)/syntax/syntax_view.o $(BUILD_DIR)/hir/hir.o $(BUILD_DIR)/lower/lower.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/compile/compile.o $(BUILD_DIR)/doctest/doctest_extract.o $(BUILD_DIR)/doctest/doctest_run.o $(BUILD_DIR)/semantic/semantic.o $(BUILD_DIR)/semantic/sem_model.o $(BUILD_DIR)/semantic/sem_hints.o $(BUILD_DIR)/semantic/sem_diagnostics.o $(BUILD_DIR)/semantic/sem_types.o $(BUILD_DIR)/semantic/tycheck.o $(BUILD_DIR)/codegen/codegen.o $(CLI_OBJS) $(FOLD_OBJS) $(BUILD_DIR)/main.o
LEXER_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/lexer/lexer_main.o
FMT_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/syntax/type_ref.o $(BUILD_DIR)/syntax/syntax_tree.o $(BUILD_DIR)/syntax/syntax_view.o $(BUILD_DIR)/syntax/format_syntax.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/arche_fmt.o
SYNTAX_TOKENS_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/syntax/type_ref.o $(BUILD_DIR)/syntax/syntax_tree.o $(BUILD_DIR)/syntax/syntax_view.o $(BUILD_DIR)/syntax/token_category.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/arche_syntax_tokens.o
# Standalone arche-analyzer = the analyzer object + a thin shim main (analyze_main lives in
# arche_analyzer.o, shared with the folded `arche analyze` subcommand).
ANALYZER_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/syntax/type_ref.o $(BUILD_DIR)/syntax/syntax_tree.o $(BUILD_DIR)/syntax/syntax_view.o $(BUILD_DIR)/syntax/token_category.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/semantic/semantic.o $(BUILD_DIR)/semantic/sem_model.o $(BUILD_DIR)/semantic/sem_hints.o $(BUILD_DIR)/semantic/sem_diagnostics.o $(BUILD_DIR)/semantic/sem_types.o $(BUILD_DIR)/semantic/tycheck.o $(BUILD_DIR)/arche_analyzer.o $(BUILD_DIR)/arche_analyzer_main.o
SYNTAX_ROUNDTRIP_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/syntax/type_ref.o $(BUILD_DIR)/syntax/syntax_tree.o $(BUILD_DIR)/syntax/syntax_view.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/arche_syntax_roundtrip.o
SYNTAX_VIEW_TEST_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/syntax/type_ref.o $(BUILD_DIR)/syntax/syntax_tree.o $(BUILD_DIR)/syntax/syntax_view.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/unit/compiler/syntax_view_tests.o
SEMANTIC_TEST_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/syntax/type_ref.o $(BUILD_DIR)/syntax/syntax_tree.o $(BUILD_DIR)/syntax/syntax_view.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/semantic/semantic.o $(BUILD_DIR)/semantic/sem_model.o $(BUILD_DIR)/semantic/sem_hints.o $(BUILD_DIR)/semantic/sem_diagnostics.o $(BUILD_DIR)/semantic/sem_types.o $(BUILD_DIR)/semantic/tycheck.o $(BUILD_DIR)/unit/compiler/semantic_tests.o
CODEGEN_TEST_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/syntax/type_ref.o $(BUILD_DIR)/syntax/syntax_tree.o $(BUILD_DIR)/syntax/syntax_view.o $(BUILD_DIR)/hir/hir.o $(BUILD_DIR)/lower/lower.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/semantic/semantic.o $(BUILD_DIR)/semantic/sem_model.o $(BUILD_DIR)/semantic/sem_hints.o $(BUILD_DIR)/semantic/sem_diagnostics.o $(BUILD_DIR)/semantic/sem_types.o $(BUILD_DIR)/semantic/tycheck.o $(BUILD_DIR)/codegen/codegen.o $(BUILD_DIR)/unit/compiler/codegen_tests.o
LOWER_TEST_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/syntax/type_ref.o $(BUILD_DIR)/syntax/syntax_tree.o $(BUILD_DIR)/syntax/syntax_view.o $(BUILD_DIR)/hir/hir.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/semantic/semantic.o $(BUILD_DIR)/semantic/sem_model.o $(BUILD_DIR)/semantic/sem_hints.o $(BUILD_DIR)/semantic/sem_diagnostics.o $(BUILD_DIR)/semantic/sem_types.o $(BUILD_DIR)/semantic/tycheck.o $(BUILD_DIR)/lower/lower.o $(BUILD_DIR)/unit/compiler/lower_tests.o

# Default target
# `arche fmt` replaces the standalone arche-fmt (its target is still defined, buildable on demand).
# arche-analyzer (LSP) + arche-syntax-tokens stay for editor integration.
all: $(BUILD_DIR) $(TARGET) $(LEXER_BIN) $(SYNTAX_TOKENS_BIN) $(ANALYZER_BIN) $(SEMANTIC_TEST_BIN) $(CODEGEN_TEST_BIN) $(LOWER_TEST_BIN) $(SYNTAX_VIEW_TEST_BIN) $(LIBARCH) $(BUILD_DIR)/runtime/stack_check.o $(BUILD_DIR)/runtime/io.o $(BUILD_DIR)/runtime/net.o $(BUILD_DIR)/runtime/term.o

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)/lexer $(BUILD_DIR)/syntax $(BUILD_DIR)/hir $(BUILD_DIR)/lower $(BUILD_DIR)/parser $(BUILD_DIR)/compile $(BUILD_DIR)/doctest $(BUILD_DIR)/semantic $(BUILD_DIR)/codegen $(BUILD_DIR)/cli $(BUILD_DIR)/unit/compiler $(BUILD_DIR)/runtime

# Build main compiler executable
$(TARGET): $(COMPILER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Build lexer-only executable
$(LEXER_BIN): $(LEXER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Build formatter executable
$(FMT_BIN): $(FMT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Build syntax tree token dumper (powers editor syntax highlighting)
$(SYNTAX_TOKENS_BIN): $(SYNTAX_TOKENS_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Build the editor analysis service (powers inlay hints + diagnostics)
$(ANALYZER_BIN): $(ANALYZER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Build syntax tree round-trip verifier (proves the syntax tree is lossless)
$(SYNTAX_ROUNDTRIP_BIN): $(SYNTAX_ROUNDTRIP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Build syntax tree view-layer unit tests
$(SYNTAX_VIEW_TEST_BIN): $(SYNTAX_VIEW_TEST_OBJS)
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
# it includes, so editing a header (e.g. type_ref.h, lexer.h enum) rebuilds the
# dependent objects instead of leaving stale, mismatched layouts.
$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

# Pull in the generated header-dependency files (none on a clean build).
# Auto-dependency files only — `-type f` so a test artifact directory that happens to end in `.d`
# (e.g. a `%t.d` lit temp dir) can never be pulled in here and break the build.
-include $(shell find $(BUILD_DIR) -name '*.d' -type f 2>/dev/null)

# Run the compiler (produces executable)
run: $(TARGET)
	./$(TARGET) build examples/stuff.arche
	@echo
	@echo "Running generated executable:"
	@./examples/stuff

# Run the lexer
run-lexer: $(LEXER_BIN)
	./$(LEXER_BIN) examples/hello.arc

# Run lexer tests
test-lexer: $(LEXER_BIN)
	LEXER_BIN=$(LEXER_BIN) ./tests/run_lexer_tests.sh

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
test: $(TARGET) $(SEMANTIC_TEST_BIN) $(CODEGEN_TEST_BIN) $(SYNTAX_VIEW_TEST_BIN) $(BUILD_DIR)/runtime/stack_check.o $(BUILD_DIR)/runtime/io.o $(BUILD_DIR)/runtime/net.o $(BUILD_DIR)/runtime/term.o
	lit -v tests/
	$(MAKE) test-doc

# Run doctests (```arche examples in /// doc comments) over the real source tree.
# The synthetic runner fixtures in tests/unit/doctest/ are exercised by lit above
# (some are intentional failures wrapped in `not`); this sweeps the production
# sources so any documented example is CI-gated. Skips files with no examples.
test-doc: $(TARGET) $(BUILD_DIR)/runtime/stack_check.o $(BUILD_DIR)/runtime/io.o $(BUILD_DIR)/runtime/net.o $(BUILD_DIR)/runtime/term.o
	./$(TARGET) test core/... stdlib/... examples/...

# Memory-safety regression gate (AddressSanitizer + UndefinedBehaviorSanitizer). Rebuilds the
# context-freeing unit-test binaries in an ISOLATED object tree (build-asan/) — so the normal
# build/ stays usable — and runs them, FAILING on the first leak / use-after-free / UB. Scoped to
# the two binaries that free the SemanticContext and are sanitizer-clean (semantic-test, lower-test).
# codegen-test (pre-existing gen_value_name leak) is excluded until separately addressed — a gate is
# only honest if it is green.
# CFLAGS is APPENDED so the -DARCHE_*_DIR resource defines survive.
test-asan memcheck:
	$(MAKE) BUILD_DIR=build-asan \
		CFLAGS='$(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all -g' \
		build-asan/semantic-test build-asan/lower-test
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ./build-asan/semantic-test
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ./build-asan/lower-test

# Test folder with pattern: make test-folder FOLDER=path PATTERN="*.arche"
test-folder: $(TARGET) $(BUILD_DIR)
	@if [ -z "$(FOLDER)" ]; then echo "Usage: make test-folder FOLDER=path [PATTERN='*.arche']"; exit 1; fi
	@mkdir -p $(BUILD_DIR)/tests
	@PATTERN="$${PATTERN:=*.arche}"; \
	for f in $(FOLDER)$$PATTERN; do \
		echo "Testing $$f..."; \
		./$(TARGET) build -o $(BUILD_DIR)/tests/test_arche "$$f" 2>&1 | tail -1; \
		$(BUILD_DIR)/tests/test_arche 2>&1 || echo "FAILED: $$f"; \
	done

# Test code generation
test-codegen: $(TARGET)
	./$(TARGET) build -o $(BUILD_DIR)/hello_world examples/hello_world/hello_world.arche
	@test -x $(BUILD_DIR)/hello_world && ./$(BUILD_DIR)/hello_world > /tmp/test_output.txt && grep -q "Hello, World!" /tmp/test_output.txt && echo "✓ Codegen test passed (hello_world)" || echo "✗ Codegen test failed"

# Clean all generated artifacts.
# Does NOT delete benchmark CSVs — those are expensive to regenerate (15-20 min for 100M rows).
# Use `make clean-data` separately when you want to free disk space.
clean:
	rm -rf $(BUILD_DIR) build-asan
	find examples/ -type f ! -name "*.c" ! -name "*.arche" ! -name "*.sh" -delete
	find design_analysis/ -type d -name __pycache__ -exec rm -rf {} + 2>/dev/null || true

# Delete benchmark datasets. Run explicitly when you want to reclaim disk space.
clean-data:
	find design_analysis/benchmarks/etl/data/ -type f -name "*.csv" -delete
	find design_analysis/benchmarks/systems/data/ -type f -name "*.csv" -delete 2>/dev/null || true

# Design analysis benchmarks (data-driven design decisions, not language perf)
bench-physics: design_analysis/array_ops/physics_update.c
	@mkdir -p design_analysis/array_ops/results
	$(CC) -Wall -Wextra -Werror -std=c99 -O3 -march=native -o $(BUILD_DIR)/bench-physics design_analysis/array_ops/physics_update.c -lm
	./$(BUILD_DIR)/bench-physics

bench-strings: design_analysis/string_ops/fixed_length.c
	@mkdir -p design_analysis/string_ops/results
	$(CC) -Wall -Wextra -Werror -std=c99 -O3 -o $(BUILD_DIR)/bench-strings design_analysis/string_ops/fixed_length.c
	./$(BUILD_DIR)/bench-strings

bench-lifecycle: design_analysis/array_ops/lifecycle_operations.c
	@mkdir -p design_analysis/array_ops/results
	$(CC) -Wall -Wextra -Werror -std=c99 -O3 -march=native -o $(BUILD_DIR)/bench-lifecycle design_analysis/array_ops/lifecycle_operations.c -lm
	./$(BUILD_DIR)/bench-lifecycle

bench-mixed: design_analysis/array_ops/mixed_workload.c
	@mkdir -p design_analysis/array_ops/results
	$(CC) -Wall -Wextra -Werror -std=c99 -O3 -march=native -o $(BUILD_DIR)/bench-mixed design_analysis/array_ops/mixed_workload.c -lm
	./$(BUILD_DIR)/bench-mixed

# Format all Arche source files and the compiler's C/H sources.
# Skips Python venv / site-packages directories so we don't try to format
# numpy/pyarrow's bundled C headers.
#
# CLANG_FORMAT is pinned to a single major version (read from .clang-format-version)
# so local and CI agree byte-for-byte. clang-format's defaults shift between
# majors and the .clang-format file doesn't enumerate every option — pinning
# the version is the only way to keep the two in sync without lots of options
# churn.
#
# The picker logic: try `clang-format-<N>` first (apt.llvm.org / Debian convention),
# then fall back to plain `clang-format` IF it reports the pinned major (Arch and
# some other distros don't suffix the binary). Fails fast with an install hint
# instead of silently using a mismatched version.
CLANG_FORMAT_VERSION := $(shell cat .clang-format-version 2>/dev/null)
CLANG_FORMAT := $(shell \
	if command -v clang-format-$(CLANG_FORMAT_VERSION) >/dev/null 2>&1; then \
		echo clang-format-$(CLANG_FORMAT_VERSION); \
	elif command -v clang-format >/dev/null 2>&1 \
	     && clang-format --version | grep -qE 'version $(CLANG_FORMAT_VERSION)\.'; then \
		echo clang-format; \
	fi)

format: $(TARGET)
	@if [ -z "$(CLANG_FORMAT_VERSION)" ]; then \
		echo "error: .clang-format-version missing"; exit 1; \
	fi
	@if [ -z "$(CLANG_FORMAT)" ]; then \
		echo "error: no clang-format matching pinned version $(CLANG_FORMAT_VERSION) on PATH"; \
		echo "  expected 'clang-format-$(CLANG_FORMAT_VERSION)' or 'clang-format' reporting major $(CLANG_FORMAT_VERSION)"; \
		echo "  install from apt.llvm.org (Debian/Ubuntu):"; \
		echo "    wget -qO- https://apt.llvm.org/llvm.sh | sudo bash -s -- $(CLANG_FORMAT_VERSION)"; \
		echo "    sudo apt-get install clang-format-$(CLANG_FORMAT_VERSION)"; \
		echo "  or via your distro's package manager (Arch: clang)"; \
		exit 1; \
	fi
	for f in $$(find . -name "*.arche" -type f \
	             -not -path "*/.venv/*" \
	             -not -path "*/site-packages/*" \
	             -not -path "*/__pycache__/*"); do \
		tmp=$$(mktemp --suffix=.arche); \
		if timeout 5 ./$(TARGET) fmt "$$f" > "$$tmp" 2>/dev/null \
		   && timeout 5 ./$(TARGET) fmt "$$tmp" > /dev/null 2>&1; then \
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
		$(CLANG_FORMAT) -i "$$f"; \
		echo "✓ $$f"; \
	done

.PHONY: build

# Verify the syntax tree is lossless: every .arche file reconstructs byte-for-byte from the syntax tree.
CORPUS = $(shell find . -name "*.arche" -type f \
	-not -path "./build/*" -not -path "*/.venv/*" -not -path "*/site-packages/*" \
	-not -path "*/__pycache__/*" -not -path "./tests/known_failures/*")
verify-syntax: $(SYNTAX_ROUNDTRIP_BIN)
	@./$(SYNTAX_ROUNDTRIP_BIN) $(CORPUS) \
		&& echo "verify-syntax: all $(words $(CORPUS)) files round-trip losslessly"

# syntax tree view-layer unit tests
test-syntax-view: $(SYNTAX_VIEW_TEST_BIN)
	@./$(SYNTAX_VIEW_TEST_BIN)

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
		if ! ./$(TARGET) build --emit=llvm-ir -o $$ir $$f >/dev/null 2>&1; then \
			echo "verify-codegen: $$f failed to emit IR"; fail=1; continue; fi; \
		if [ -f $(VERIFY_CG_DIR)/$$base.ll ]; then \
			if ! diff -q $(VERIFY_CG_DIR)/$$base.ll $$ir >/dev/null; then \
				echo "CODEGEN DIFF: $$f"; diff $(VERIFY_CG_DIR)/$$base.ll $$ir | head -20; fail=1; fi; \
		else cp $$ir $(VERIFY_CG_DIR)/$$base.ll; echo "golden captured: $$base"; fi; \
	done; \
	[ $$fail -eq 0 ] && echo "verify-codegen: IR matches golden" || (echo "verify-codegen: FAILED"; exit 1)

# Install a relocatable arche: the binary in $(PREFIX)/bin and its support files in
# $(PREFIX)/lib/arche/{core,stdlib,runtime,explain}. The installed binary discovers those via the
# exe-relative path (<bindir>/../lib/arche/...) in cli/resource.c, so it runs from anywhere.
PREFIX ?= /usr/local
ARCHE_LIBDIR = $(DESTDIR)$(PREFIX)/lib/arche
ARCHE_BINDIR = $(DESTDIR)$(PREFIX)/bin

# Shell-completion directories. Completion only auto-loads from the dirs each shell scans — which are
# system paths (under /usr/share), NOT $(PREFIX)/share — so we install there directly. We ask
# pkg-config where bash-completion / fish keep theirs (distro-correct), falling back to the
# conventions; zsh has no pkg-config var, so we use the standard site-functions dir. Override any of
# these on the command line for non-standard layouts.
BASHCOMP_DIR ?= $(shell pkg-config --variable=completionsdir bash-completion 2>/dev/null || echo /usr/share/bash-completion/completions)
FISHCOMP_DIR ?= $(shell pkg-config --variable=completionsdir fish 2>/dev/null || echo /usr/share/fish/vendor_completions.d)
ZSHCOMP_DIR ?= /usr/share/zsh/site-functions
install: all
	install -d "$(ARCHE_BINDIR)" "$(ARCHE_LIBDIR)/core" "$(ARCHE_LIBDIR)/stdlib" "$(ARCHE_LIBDIR)/runtime" "$(ARCHE_LIBDIR)/explain"
	install -m 0755 $(TARGET) "$(ARCHE_BINDIR)/arche"
	install -m 0755 $(ANALYZER_BIN) "$(ARCHE_BINDIR)/arche-analyzer"
	install -m 0644 core/core.arche "$(ARCHE_LIBDIR)/core/"
	cp -R stdlib/. "$(ARCHE_LIBDIR)/stdlib/"
	install -m 0644 $(BUILD_DIR)/runtime/stack_check.o $(BUILD_DIR)/runtime/io.o $(BUILD_DIR)/runtime/net.o $(BUILD_DIR)/runtime/term.o "$(ARCHE_LIBDIR)/runtime/"
	@[ -d docs/explain ] && cp -R docs/explain/. "$(ARCHE_LIBDIR)/explain/" || true
	@# `cp -R` preserves source-tree modes (and leaves a pre-existing dest file's mode untouched on
	@# re-install), so a stdlib file that happens to be 0600 in the tree lands unreadable for the
	@# user who runs `arche`. Normalize: every installed resource MUST be world-readable (dirs
	@# traversable). a+rX is idempotent and re-asserts this on every install.
	chmod -R a+rX "$(ARCHE_LIBDIR)"
	@# Install shell completions into the dirs each installed shell auto-scans, so they "just work"
	@# in a new shell with no sourcing. Gated on the shell being present so we don't litter.
	@if command -v bash >/dev/null 2>&1; then \
		d="$(DESTDIR)$(BASHCOMP_DIR)"; install -d "$$d" && $(TARGET) completion bash > "$$d/arche" && echo "  bash  -> $$d/arche"; \
	fi
	@if command -v zsh >/dev/null 2>&1; then \
		d="$(DESTDIR)$(ZSHCOMP_DIR)"; install -d "$$d" && $(TARGET) completion zsh > "$$d/_arche" && echo "  zsh   -> $$d/_arche"; \
	fi
	@if command -v fish >/dev/null 2>&1; then \
		d="$(DESTDIR)$(FISHCOMP_DIR)"; install -d "$$d" && $(TARGET) completion fish > "$$d/arche.fish" && echo "  fish  -> $$d/arche.fish"; \
	fi
	@echo "installed arche $(ARCHE_VERSION) to $(DESTDIR)$(PREFIX) — open a new shell for completion"

# Smoke-test relocatability: install to a throwaway prefix and compile+run from an unrelated cwd,
# so the binary must resolve core/stdlib/runtime via the exe-relative layout (no in-tree paths).
test-install: all
	@root=$$(mktemp -d); $(MAKE) -s install PREFIX=$$root BASHCOMP_DIR=$$root/bashcomp ZSHCOMP_DIR=$$root/zshcomp FISHCOMP_DIR=$$root/fishcomp >/dev/null; \
	printf 'proc main() { printf("install-ok\\n"); }\n' > $$root/t.arche; \
	out=$$(cd /tmp && $$root/bin/arche run $$root/t.arche); \
	rm -rf $$root; \
	[ "$$out" = "install-ok" ] && echo "test-install: PASS" || { echo "test-install: FAIL (got '$$out')"; exit 1; }

# Phony targets
.PHONY: all run run-lexer test test-doc test-lexer test-semantic test-codegen test-codegen-unit test-lit test-lower test-asan memcheck clean clean-data bench-physics bench-strings bench-lifecycle bench-mixed format verify-syntax verify-codegen install test-install

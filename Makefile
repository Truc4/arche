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
HOTRELOAD_TEST_BIN = $(BUILD_DIR)/hotreload-test
INSPECT_TEST_BIN = $(BUILD_DIR)/inspect-test
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
       compile/module_resolve.c \
       compile/variant_select.c \
       doctest/doctest_extract.c \
       doctest/doctest_run.c \
       semantic/semantic.c \
       semantic/sem_model.c \
       semantic/sem_hints.c \
       semantic/sem_diagnostics.c \
       semantic/sem_types.c \
       semantic/tycheck.c \
       codegen/codegen.c \
       codegen/gpu_glsl.c \
       codegen/gpu_embed.c

# `--gpu` builds dispatch @gpu maps on the GPU via Vulkan. Detect the Vulkan SDK once — the probe both
# includes <vulkan/vulkan.h> AND links -lvulkan against a real symbol, so headers-without-library does NOT
# falsely enable it (which would make every `arche build --gpu` fail to link). When present, define
# ARCHE_HAVE_VULKAN; this MUST reach both compile.o (the -lvulkan #ifdef) and runtime/gpu_runtime.o (the
# real-vs-stub #if) — it does, via the single generic %.o rule's CFLAGS. Absent → the compiler still
# builds and `--gpu` degrades to a CPU-fallback stub that links no library.
HAVE_VULKAN := $(shell printf '#include <vulkan/vulkan.h>\nint main(void){vkEnumerateInstanceVersion(0);return 0;}\n' | $(CC) -x c - -lvulkan -o /dev/null >/dev/null 2>&1 && echo 1)
ifeq ($(HAVE_VULKAN),1)
CFLAGS += -DARCHE_HAVE_VULKAN
endif

RUNTIME_SRCS = runtime/stack_check.c runtime/io.c runtime/net.c runtime/term.c runtime/inspect.c
RUNTIME_OBJS = $(RUNTIME_SRCS:.c=.o)
# Position-independent copies of the runtime, linked into `--emit=shared` (.so) builds. Kept SEPARATE
# from the non-PIC `.o` set so the executable link (-no-pie -mcmodel=large) stays byte-identical.
RUNTIME_PIC_OBJS = $(BUILD_DIR)/runtime/stack_check.pic.o $(BUILD_DIR)/runtime/io.pic.o $(BUILD_DIR)/runtime/net.pic.o $(BUILD_DIR)/runtime/term.pic.o

OBJS = $(SRCS:.c=.o)
# CLI multitool: dispatch + table-driven arg parser + one object per subcommand.
CLI_OBJS = $(BUILD_DIR)/cli/args.o $(BUILD_DIR)/cli/cli.o $(BUILD_DIR)/cli/resource.o $(BUILD_DIR)/cli/cmd_build.o $(BUILD_DIR)/cli/cmd_run.o $(BUILD_DIR)/cli/cmd_check.o $(BUILD_DIR)/cli/cmd_test.o $(BUILD_DIR)/cli/cmd_fmt.o $(BUILD_DIR)/cli/cmd_explain.o $(BUILD_DIR)/cli/cmd_analyze.o $(BUILD_DIR)/cli/cmd_completion.o $(BUILD_DIR)/cli/cmd_version.o $(BUILD_DIR)/cli/cmd_init.o $(BUILD_DIR)/cli/cmd_fill.o $(BUILD_DIR)/cli/cmd_inspect.o
# Satellite tools folded into the `arche` binary as subcommands (fmt, analyze): their objects join
# the main link. The standalone arche-fmt / arche-analyzer binaries still build during the migration.
FOLD_OBJS = $(BUILD_DIR)/syntax/format_syntax.o $(BUILD_DIR)/syntax/token_category.o $(BUILD_DIR)/arche_analyzer.o
COMPILER_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/syntax/type_ref.o $(BUILD_DIR)/syntax/syntax_tree.o $(BUILD_DIR)/syntax/syntax_view.o $(BUILD_DIR)/hir/hir.o $(BUILD_DIR)/lower/lower.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/compile/compile.o $(BUILD_DIR)/compile/module_resolve.o $(BUILD_DIR)/compile/variant_select.o $(BUILD_DIR)/doctest/doctest_extract.o $(BUILD_DIR)/doctest/doctest_run.o $(BUILD_DIR)/semantic/semantic.o $(BUILD_DIR)/semantic/sem_model.o $(BUILD_DIR)/semantic/sem_hints.o $(BUILD_DIR)/semantic/sem_diagnostics.o $(BUILD_DIR)/semantic/sem_types.o $(BUILD_DIR)/semantic/tycheck.o $(BUILD_DIR)/codegen/codegen.o $(BUILD_DIR)/codegen/gpu_glsl.o $(BUILD_DIR)/codegen/gpu_embed.o $(CLI_OBJS) $(FOLD_OBJS) $(BUILD_DIR)/main.o
LEXER_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/lexer/lexer_main.o
FMT_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/syntax/type_ref.o $(BUILD_DIR)/syntax/syntax_tree.o $(BUILD_DIR)/syntax/syntax_view.o $(BUILD_DIR)/syntax/format_syntax.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/arche_fmt.o
SYNTAX_TOKENS_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/syntax/type_ref.o $(BUILD_DIR)/syntax/syntax_tree.o $(BUILD_DIR)/syntax/syntax_view.o $(BUILD_DIR)/syntax/token_category.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/arche_syntax_tokens.o
# Standalone arche-analyzer = the analyzer object + a thin shim main (analyze_main lives in
# arche_analyzer.o, shared with the folded `arche analyze` subcommand).
ANALYZER_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/syntax/type_ref.o $(BUILD_DIR)/syntax/syntax_tree.o $(BUILD_DIR)/syntax/syntax_view.o $(BUILD_DIR)/syntax/token_category.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/semantic/semantic.o $(BUILD_DIR)/semantic/sem_model.o $(BUILD_DIR)/semantic/sem_hints.o $(BUILD_DIR)/semantic/sem_diagnostics.o $(BUILD_DIR)/semantic/sem_types.o $(BUILD_DIR)/semantic/tycheck.o $(BUILD_DIR)/compile/module_resolve.o $(BUILD_DIR)/compile/variant_select.o $(BUILD_DIR)/cli/resource.o $(BUILD_DIR)/arche_analyzer.o $(BUILD_DIR)/arche_analyzer_main.o
SYNTAX_ROUNDTRIP_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/syntax/type_ref.o $(BUILD_DIR)/syntax/syntax_tree.o $(BUILD_DIR)/syntax/syntax_view.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/arche_syntax_roundtrip.o
SYNTAX_VIEW_TEST_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/syntax/type_ref.o $(BUILD_DIR)/syntax/syntax_tree.o $(BUILD_DIR)/syntax/syntax_view.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/unit/compiler/syntax_view_tests.o
SEMANTIC_TEST_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/syntax/type_ref.o $(BUILD_DIR)/syntax/syntax_tree.o $(BUILD_DIR)/syntax/syntax_view.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/semantic/semantic.o $(BUILD_DIR)/semantic/sem_model.o $(BUILD_DIR)/semantic/sem_hints.o $(BUILD_DIR)/semantic/sem_diagnostics.o $(BUILD_DIR)/semantic/sem_types.o $(BUILD_DIR)/semantic/tycheck.o $(BUILD_DIR)/unit/compiler/semantic_tests.o
CODEGEN_TEST_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/syntax/type_ref.o $(BUILD_DIR)/syntax/syntax_tree.o $(BUILD_DIR)/syntax/syntax_view.o $(BUILD_DIR)/hir/hir.o $(BUILD_DIR)/lower/lower.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/semantic/semantic.o $(BUILD_DIR)/semantic/sem_model.o $(BUILD_DIR)/semantic/sem_hints.o $(BUILD_DIR)/semantic/sem_diagnostics.o $(BUILD_DIR)/semantic/sem_types.o $(BUILD_DIR)/semantic/tycheck.o $(BUILD_DIR)/codegen/codegen.o $(BUILD_DIR)/unit/compiler/codegen_tests.o
LOWER_TEST_OBJS = $(BUILD_DIR)/lexer/lexer.o $(BUILD_DIR)/syntax/type_ref.o $(BUILD_DIR)/syntax/syntax_tree.o $(BUILD_DIR)/syntax/syntax_view.o $(BUILD_DIR)/hir/hir.o $(BUILD_DIR)/parser/parser.o $(BUILD_DIR)/semantic/semantic.o $(BUILD_DIR)/semantic/sem_model.o $(BUILD_DIR)/semantic/sem_hints.o $(BUILD_DIR)/semantic/sem_diagnostics.o $(BUILD_DIR)/semantic/sem_types.o $(BUILD_DIR)/semantic/tycheck.o $(BUILD_DIR)/lower/lower.o $(BUILD_DIR)/unit/compiler/lower_tests.o
# Reload-runtime unit test: just the runtime object + the test driver (links libdl for dlopen/dlsym).
HOTRELOAD_TEST_OBJS = $(BUILD_DIR)/runtime/hotreload.o $(BUILD_DIR)/unit/runtime/hotreload_tests.o
# State-inspector unit test: the runtime object + the test driver (drives arche_inspect_handle directly).
INSPECT_TEST_OBJS = $(BUILD_DIR)/runtime/inspect.o $(BUILD_DIR)/unit/runtime/inspect_tests.o

# Default target
# `arche fmt` replaces the standalone arche-fmt (its target is still defined, buildable on demand).
# arche-analyzer (LSP) + arche-syntax-tokens stay for editor integration.
all: $(BUILD_DIR) $(TARGET) $(LEXER_BIN) $(SYNTAX_TOKENS_BIN) $(ANALYZER_BIN) $(SEMANTIC_TEST_BIN) $(CODEGEN_TEST_BIN) $(LOWER_TEST_BIN) $(SYNTAX_VIEW_TEST_BIN) $(HOTRELOAD_TEST_BIN) $(INSPECT_TEST_BIN) $(LIBARCH) $(BUILD_DIR)/runtime/stack_check.o $(BUILD_DIR)/runtime/io.o $(BUILD_DIR)/runtime/net.o $(BUILD_DIR)/runtime/term.o $(RUNTIME_PIC_OBJS) $(BUILD_DIR)/runtime/hotreload.o $(BUILD_DIR)/runtime/inspect.o $(BUILD_DIR)/runtime/gpu_runtime.o

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

# Build hot-reload runtime unit test (needs libdl for the runtime's dlopen/dlsym)
$(HOTRELOAD_TEST_BIN): $(HOTRELOAD_TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -ldl

# Build state-inspector unit test (plain libc; no sockets opened in the test)
$(INSPECT_TEST_BIN): $(INSPECT_TEST_OBJS)
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

# PIC runtime objects for `--emit=shared` (.so) builds.
$(BUILD_DIR)/%.pic.o: %.c | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -MMD -MP -c -o $@ $<

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

# Run semantic tests
test-semantic: $(SEMANTIC_TEST_BIN)
	./$(SEMANTIC_TEST_BIN)

# Run codegen unit tests
test-codegen-unit: $(CODEGEN_TEST_BIN)
	./$(CODEGEN_TEST_BIN)

# Run lower tests
test-lower: $(LOWER_TEST_BIN)
	./$(LOWER_TEST_BIN)

# Run hot-reload runtime unit tests
test-hotreload: $(HOTRELOAD_TEST_BIN)
	./$(HOTRELOAD_TEST_BIN)

# Run state-inspector unit tests
test-inspect: $(INSPECT_TEST_BIN)
	./$(INSPECT_TEST_BIN)

# OFF-GATE live hot-reload smoke (real `arche run` + a mid-run device edit). Kept out of `make test`
# (timing/process-bound by nature); fixture files are formatter-checked so they can't silently rot.
test-e2e: $(TARGET) $(BUILD_DIR)/runtime/hotreload.o
	python3 tests/integration/e2e_hot_reload/run_e2e.py

# Run all tests with LIT
test: $(TARGET) $(ANALYZER_BIN) $(SYNTAX_TOKENS_BIN) $(SEMANTIC_TEST_BIN) $(CODEGEN_TEST_BIN) $(SYNTAX_VIEW_TEST_BIN) $(HOTRELOAD_TEST_BIN) $(INSPECT_TEST_BIN) $(BUILD_DIR)/runtime/stack_check.o $(BUILD_DIR)/runtime/io.o $(BUILD_DIR)/runtime/net.o $(BUILD_DIR)/runtime/term.o
	lit -v tests/ extras/
	$(MAKE) test-doc
	$(MAKE) verify-fmt

# Per-unit codegen check: runs the full lit suite emitting one LLVM module per compilation unit
# (unit 0 = driver, unit N = the Nth imported device; mangled/external symbols + linkonce_odr shared
# defs), with the ODR folding verifier ALWAYS on. This is the foundation of arche's INCREMENTAL
# separate-compilation mode: a `--emit=link` build opt/llc/cc's each unit to its own content-hashed
# object (ARCHE_CACHE_DIR), reusing unchanged devices verbatim. Now fully passing and gated in
# `make test` for the key paths via tests/unit/compiler/per_unit/ (smoke, policy-no-cross-declare,
# incremental_cache). This full-suite run is kept out of CI only to avoid ~2x suite time; run it by
# hand to re-validate the whole language under per-unit. Whole-program (no inlining loss) stays the
# default build.
test-per-unit: $(TARGET) $(ANALYZER_BIN) $(SYNTAX_TOKENS_BIN) $(SEMANTIC_TEST_BIN) $(CODEGEN_TEST_BIN) $(SYNTAX_VIEW_TEST_BIN) $(HOTRELOAD_TEST_BIN) $(INSPECT_TEST_BIN) $(BUILD_DIR)/runtime/stack_check.o $(BUILD_DIR)/runtime/io.o $(BUILD_DIR)/runtime/net.o $(BUILD_DIR)/runtime/term.o
	ARCHE_PER_UNIT=1 lit -v tests/ extras/

# Run doctests over the real source tree: ```arche examples in /// doc comments (.arche) AND in
# prose docs (.md). The synthetic runner fixtures in tests/unit/doctest/ + tests/unit/mddoc/ are
# exercised by lit above (some are intentional failures wrapped in `not`); this sweeps the production
# sources so any documented example is CI-gated. Skips files with no examples.
# docs/*.md are listed explicitly (not docs/...): docs/devices.md is deliberately excluded — its
# examples are inherently multi-file (datasheet + impl + driver) and cannot run as standalone
# .md doctests (see docs/DOCTESTS.md "Markdown doctests").
test-doc: $(TARGET) $(BUILD_DIR)/runtime/stack_check.o $(BUILD_DIR)/runtime/io.o $(BUILD_DIR)/runtime/net.o $(BUILD_DIR)/runtime/term.o
	./$(TARGET) test core/... stdlib/... examples/... README.md docs/language.md docs/patterns.md docs/DOCTESTS.md

# Corpus lint gate: compile every stdlib + extras module IN CONTEXT with `-Werror`, failing on any
# warning or error. Unlike `make test` (which only catches errors in library code a test happens to
# compile, and never gates warnings), this imports each module like a real program does and promotes
# every lint to an error — so a stray warning or an unexercised module can't rot. See scripts/check_corpus.sh.
check-corpus: $(TARGET)
	@ARCHE=$(TARGET) bash scripts/check_corpus.sh

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
		build-asan/semantic-test build-asan/lower-test build-asan/hotreload-test
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ./build-asan/semantic-test
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ./build-asan/lower-test
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ./build-asan/hotreload-test

# GPU shader gate (opt-in; NOT part of `make test` so the core suite needs no GPU toolchain). For each
# `@gpu` map fixture it emits the GLSL compute shader and proves it is real, valid GPU code: glslc
# compiles it to SPIR-V and spirv-val validates the module. Skips cleanly if the toolchain is absent.
test-gpu: $(TARGET)
	@command -v glslc >/dev/null 2>&1 && command -v spirv-val >/dev/null 2>&1 || { \
		echo "test-gpu: SKIP (glslc/spirv-val not found)"; exit 0; }
	@rm -rf $(BUILD_DIR)/gpu && mkdir -p $(BUILD_DIR)/gpu
	@fail=0; n=0; \
	for f in tests/unit/gpu/*.arche; do \
		./$(TARGET) build -o $(BUILD_DIR)/gpu/exe --emit-gpu=$(BUILD_DIR)/gpu $$f >/dev/null 2>&1 || { echo "FAIL build $$f"; fail=1; continue; }; \
	done; \
	for c in $(BUILD_DIR)/gpu/*.comp; do \
		[ -e "$$c" ] || { echo "test-gpu: no shaders emitted"; exit 1; }; \
		glslc -fshader-stage=compute "$$c" -o "$$c.spv" 2>/tmp/glslc.err || { echo "FAIL glslc $$c"; cat /tmp/glslc.err; fail=1; continue; }; \
		spirv-val "$$c.spv" 2>/tmp/spv.err || { echo "FAIL spirv-val $$c"; cat /tmp/spv.err; fail=1; continue; }; \
		n=$$((n+1)); echo "PASS gpu-shader $$(basename $$c) (glslc + spirv-val)"; \
	done; \
	if [ $$fail -ne 0 ]; then echo "test-gpu: FAILED"; exit 1; fi; \
	echo "test-gpu: $$n shader(s) validated"

# GPU EXECUTION gate (opt-in). Proves an arche `@gpu` map actually runs on the GPU and matches the CPU:
# emit the shader, glslc → SPIR-V, dispatch via a tiny Vulkan runner, assert the readback == the CPU
# result. Skips cleanly if the toolchain or a Vulkan device is absent (so it never breaks GPU-less CI).
test-gpu-run: $(TARGET)
	@command -v glslc >/dev/null 2>&1 || { echo "test-gpu-run: SKIP (glslc not found)"; exit 0; }
	@cc -O2 -o $(BUILD_DIR)/gpu/vk_run tests/gpu/vk_run.c -lvulkan 2>/dev/null || { echo "test-gpu-run: SKIP (libvulkan not found)"; exit 0; }
	@mkdir -p $(BUILD_DIR)/gpu
	@./$(TARGET) build -o $(BUILD_DIR)/gpu/x --emit-gpu=$(BUILD_DIR)/gpu tests/unit/gpu/scale.arche >/dev/null 2>&1
	@./$(TARGET) build -o $(BUILD_DIR)/gpu/x --emit-gpu=$(BUILD_DIR)/gpu tests/unit/gpu/physics_step.arche >/dev/null 2>&1
	@glslc -fshader-stage=compute $(BUILD_DIR)/gpu/scale__P.comp -o $(BUILD_DIR)/gpu/scale.spv
	@glslc -fshader-stage=compute $(BUILD_DIR)/gpu/step__Body.comp -o $(BUILD_DIR)/gpu/step.spv
	@# single-column arithmetic: x = x*10 over [0,1,2,3]
	@o1=$$($(BUILD_DIR)/gpu/vk_run $(BUILD_DIR)/gpu/scale.spv 4 1 0 1 2 3); rc=$$?; \
	if [ $$rc -eq 2 ]; then echo "test-gpu-run: SKIP (no usable Vulkan device)"; exit 0; fi; \
	if [ $$rc -ne 0 ]; then echo "test-gpu-run: FAIL (runner error)"; exit 1; fi; \
	if [ "$$o1" != "0 10 20 30" ]; then echo "test-gpu-run: FAIL scale — GPU [$$o1] != [0 10 20 30]"; exit 1; fi; \
	echo "test-gpu-run: PASS scale — GPU [$$o1] == CPU"; \
	o2=$$($(BUILD_DIR)/gpu/vk_run $(BUILD_DIR)/gpu/step.spv 8 2 0 1 2 3 4 5 6 7 1 1 1 1 1 1 1 1); \
	if [ "$$o2" != "1 2 3 4 5 6 7 8 | 1 1 1 1 1 1 1 1" ]; then echo "test-gpu-run: FAIL step — GPU [$$o2]"; exit 1; fi; \
	echo "test-gpu-run: PASS step (2-col + select) — GPU [$$o2] == CPU"

# GPU EXECUTABLE gate (opt-in). The end-to-end Phase 3 path: `arche build --gpu` produces a normal
# executable that embeds the @gpu maps' SPIR-V and dispatches them on the GPU at runtime (CPU fallback).
# Asserts the program's own output is correct AND — via ARCHE_GPU_DEBUG — that the GPU path actually ran
# when a device is present. With no Vulkan device the CPU fallback still produces the right answer (SKIP).
test-gpu-exe: $(TARGET) $(BUILD_DIR)/runtime/gpu_runtime.o
	@command -v glslc >/dev/null 2>&1 || { echo "test-gpu-exe: SKIP (glslc not found)"; exit 0; }
	@mkdir -p $(BUILD_DIR)/gpu
	@for t in scale:"x0=0 x3=30" physics_step:"p0=1 p7=8"; do \
		name=$${t%%:*}; want=$${t#*:}; \
		./$(TARGET) build --gpu -o $(BUILD_DIR)/gpu/$$name.exe tests/unit/gpu/$$name.arche >/dev/null 2>&1 \
			|| { echo "test-gpu-exe: SKIP ($$name --gpu build failed; likely no libvulkan at link)"; exit 0; }; \
		out=$$(ARCHE_GPU_DEBUG=1 $(BUILD_DIR)/gpu/$$name.exe 2>$(BUILD_DIR)/gpu/$$name.err); \
		echo "$$out" | grep -qF "$$want" || { echo "test-gpu-exe: FAIL $$name — output [$$out] != [$$want]"; exit 1; }; \
		if grep -q "gpu dispatch" $(BUILD_DIR)/gpu/$$name.err; then \
			echo "test-gpu-exe: PASS $$name — ran on GPU, output [$$want]"; \
		else \
			echo "test-gpu-exe: SKIP $$name (no Vulkan device; CPU fallback output [$$want] correct)"; \
		fi; \
	done

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
	# Format `.arche` files FIRST — this uses the arche binary, NOT clang-format, so it must not be
	# gated behind clang-format availability (a missing/mismatched clang-format would otherwise block
	# .arche formatting too, across the WHOLE tree including extras/).
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
	@if [ -z "$(CLANG_FORMAT)" ]; then \
		echo "error: no clang-format matching pinned version $(CLANG_FORMAT_VERSION) on PATH (C/H files NOT formatted)"; \
		echo "  expected 'clang-format-$(CLANG_FORMAT_VERSION)' or 'clang-format' reporting major $(CLANG_FORMAT_VERSION)"; \
		echo "  install from apt.llvm.org (Debian/Ubuntu):"; \
		echo "    wget -qO- https://apt.llvm.org/llvm.sh | sudo bash -s -- $(CLANG_FORMAT_VERSION)"; \
		echo "    sudo apt-get install clang-format-$(CLANG_FORMAT_VERSION)"; \
		echo "  or via your distro's package manager (Arch: clang)"; \
		exit 1; \
	fi
	# Format C/H with the pinned clang-format. Skip wayland-scanner-GENERATED protocol code
	# (`*-protocol.c` / `*-client-protocol.h`) — machine-emitted, not held to the style (matches ci.yml).
	for f in $$(find . \( -name "*.c" -o -name "*.h" \) -type f \
	             -not -path "./build/*" \
	             -not -path "*/.venv/*" \
	             -not -path "*/site-packages/*" \
	             -not -path "*/__pycache__/*" \
	             -not -name "*-protocol.c" \
	             -not -name "*-protocol.h" \
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

# Verify `arche fmt` never corrupts a file and is idempotent, across the whole corpus. The net that
# was missing while the formatter silently produced NUL/heap garbage for some files (data loss).
verify-fmt: $(TARGET)
	@ARCHE=$(TARGET) bash scripts/verify_fmt.sh $(CORPUS)

# syntax tree view-layer unit tests
test-syntax-view: $(SYNTAX_VIEW_TEST_BIN)
	@./$(SYNTAX_VIEW_TEST_BIN)

# MANUAL dev tool (NOT in `make test`, NOT in CI): emit LLVM IR for representative programs and diff
# against checked-in goldens (first run captures them). VERIFY_CG_PROGRAMS is the fixed representative
# set; see tests/codegen_golden/. Catches silent IR drift that behavioral tests can't see (same output,
# different IR). Run it to review a change's IR impact or chase a suspected codegen regression; on an
# INTENDED codegen change regenerate the goldens: `rm tests/codegen_golden/*.ll && make verify-codegen`,
# then review the diff before committing. Goldens are RAW (pre-opt) codegen on purpose — that is pure
# arche output (hardcoded triple/datalayout), hence machine-independent and reproducible; post-opt IR
# would be LLVM-version-dependent. It is manual rather than CI-gated because raw IR churns on cosmetic
# codegen changes, which would tax every codegen PR for protection the behavioral suite mostly provides.
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
	tests/unit/language/tuples/tuple_compound_assign.arche \
	tests/unit/language/callbacks/proc_callback.arche \
	tests/unit/language/systems/sys_minimal.arche \
	tests/unit/language/strings/string_literals.arche
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
	install -m 0644 $(BUILD_DIR)/runtime/stack_check.o $(BUILD_DIR)/runtime/io.o $(BUILD_DIR)/runtime/net.o $(BUILD_DIR)/runtime/term.o $(BUILD_DIR)/runtime/gpu_runtime.o $(BUILD_DIR)/runtime/hotreload.o $(BUILD_DIR)/runtime/inspect.o "$(ARCHE_LIBDIR)/runtime/"
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
.PHONY: all run run-lexer test test-per-unit test-doc check-corpus test-semantic test-codegen test-codegen-unit test-lit test-lower test-asan test-gpu test-gpu-run test-gpu-exe memcheck clean clean-data bench-physics bench-strings bench-lifecycle bench-mixed format verify-syntax verify-fmt verify-codegen install test-install

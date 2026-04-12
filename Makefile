CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -DARCHE_CORE_DIR=\"$(abspath core)\"
TARGET = arche
LEXER_BIN = lexer-bin
PARSER_TEST_BIN = parser-test
FMT_BIN = arche-fmt
SEMANTIC_TEST_BIN = semantic-test

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

# Default target
all: $(TARGET) $(LEXER_BIN) $(PARSER_TEST_BIN) $(FMT_BIN) $(SEMANTIC_TEST_BIN)

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

# Run all tests
test: test-lexer test-parser test-semantic test-codegen

# Test code generation
test-codegen: $(TARGET)
	rm -f examples/hello_world/hello_world
	./$(TARGET) examples/hello_world/hello_world.arche
	@test -x examples/hello_world/hello_world && ./examples/hello_world/hello_world > /tmp/test_output.txt && grep -q "Hello, World!" /tmp/test_output.txt && echo "✓ Codegen test passed (hello_world)" || echo "✗ Codegen test failed"

# Clean all generated artifacts
clean:
	find . -name "*.o" -delete
	find . -name "*.a" -delete
	rm -f $(TARGET) $(LEXER_BIN) $(PARSER_TEST_BIN) $(FMT_BIN) $(SEMANTIC_TEST_BIN)
	find examples/ -type f ! -name "*.c" ! -name "*.arche" ! -name "*.sh" -delete
	find tests/ -type f ! -name "*.c" ! -name "*.h" ! -name "*.sh" ! -name "*.arche" -delete
	rm -f *.txt test_*.sh run_*.sh 2>/dev/null || true

# Phony targets
.PHONY: all run run-lexer test test-lexer test-parser test-semantic clean

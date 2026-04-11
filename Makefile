CC = gcc
CFLAGS = -Wall -Wextra -std=c99
TARGET = arche
LEXER_BIN = lexer-bin

# Source files
SRCS = lexer/lexer.c \
       ast/ast.c \
       parser/parser.c

OBJS = $(SRCS:.c=.o)
LEXER_OBJS = lexer/lexer.o lexer/lexer_main.o

# Default target
all: $(TARGET) $(LEXER_BIN)

# Build compiler executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Build lexer-only executable
$(LEXER_BIN): $(LEXER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Compile object files
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Run the compiler
run: $(TARGET)
	./$(TARGET) examples/hello.arc

# Run the lexer
run-lexer: $(LEXER_BIN)
	./$(LEXER_BIN) examples/hello.arc

# Clean build artifacts
clean:
	rm -f $(OBJS) $(LEXER_OBJS) $(TARGET) $(LEXER_BIN)

# Phony targets
.PHONY: all run run-lexer clean

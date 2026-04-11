#!/bin/bash

# Test print statement equivalence
# Compiles both Arche and C versions, runs them, and compares output

set -e

ARCHE_COMPILER="./arche"
GCC="gcc -Wall -Wextra"

TEST_DIR="tests"
ARCHE_SRC="$TEST_DIR/data/test_print_simple.arche"
C_SRC="$TEST_DIR/test_print_simple_equiv.c"
ARCHE_BIN="$TEST_DIR/test_print_simple_arche"
C_BIN="$TEST_DIR/test_print_simple_c"
C_OUT="$TEST_DIR/c_output.txt"
ARCHE_OUT="$TEST_DIR/arche_output.txt"
DIFF_FILE="$TEST_DIR/print_test.diff"

echo "╔════════════════════════════════════════════════════════════════╗"
echo "║          PRINT STATEMENT EQUIVALENCE TEST                      ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""

# Compile Arche
echo "[1/4] Compiling Arche version..."
if $ARCHE_COMPILER "$ARCHE_SRC" -o "$ARCHE_BIN" 2>&1 | grep -q "Successfully"; then
    echo "✓ Arche compiled"
else
    echo "✗ Arche compilation failed"
    exit 1
fi

# Compile C
echo "[2/4] Compiling C version..."
if $GCC -o "$C_BIN" "$C_SRC" 2>&1; then
    echo "✓ C compiled"
else
    echo "✗ C compilation failed"
    exit 1
fi

# Run C version
echo "[3/4] Running C version..."
if "$C_BIN" > "$C_OUT" 2>&1; then
    echo "✓ C executed successfully"
    echo "C Output:"
    cat "$C_OUT" | sed 's/^/  /'
else
    echo "✗ C execution failed"
    exit 1
fi

# Run Arche version
echo "[4/4] Running Arche version..."
if "$ARCHE_BIN" > "$ARCHE_OUT" 2>&1; then
    echo "✓ Arche executed successfully"
    echo "Arche Output:"
    cat "$ARCHE_OUT" | sed 's/^/  /'
else
    echo "✗ Arche execution failed"
    exit 1
fi

# Compare outputs
echo ""
echo "╔════════════════════════════════════════════════════════════════╗"
echo "║                       RESULT                                   ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""

if diff -u "$C_OUT" "$ARCHE_OUT" > "$DIFF_FILE" 2>&1; then
    echo "✓ OUTPUTS MATCH - PRINT EQUIVALENCE VERIFIED!"
    echo ""
    echo "Both programs produce identical output:"
    cat "$C_OUT"
    exit 0
else
    echo "✗ OUTPUTS DIFFER"
    echo ""
    echo "Diff:"
    cat "$DIFF_FILE" | sed 's/^/  /'
    exit 1
fi

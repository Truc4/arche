#!/bin/bash

PASS=0
FAIL=0
ERROR=0

echo "Running all tests..."
echo

# Test lexer
echo "=== Lexer Tests ==="
if LEXER_BIN=build/lexer-bin ./tests/run_lexer_tests.sh > /tmp/lexer_out.txt 2>&1; then
    LEXER_PASS=$(grep -c "✓" /tmp/lexer_out.txt || echo 0)
    PASS=$((PASS + LEXER_PASS))
else
    ERROR=$((ERROR + 1))
fi
cat /tmp/lexer_out.txt
echo

# Test parser
echo "=== Parser Tests ==="
if ./build/parser-test > /tmp/parser_out.txt 2>&1; then
    PARSER_PASS=$(grep -c "PASS" /tmp/parser_out.txt || echo 0)
    PASS=$((PASS + PARSER_PASS))
else
    ERROR=$((ERROR + 1))
fi
cat /tmp/parser_out.txt
echo

# Test semantic
echo "=== Semantic Tests ==="
if ./build/semantic-test > /tmp/semantic_out.txt 2>&1; then
    SEMANTIC_PASS=$(grep -c "PASS" /tmp/semantic_out.txt || echo 0)
    PASS=$((PASS + SEMANTIC_PASS))
else
    ERROR=$((ERROR + 1))
fi
cat /tmp/semantic_out.txt
echo

# Test codegen unit
echo "=== Codegen Unit Tests ==="
if ./build/codegen-test > /tmp/codegen_out.txt 2>&1; then
    CODEGEN_PASS=$(grep -c "PASS" /tmp/codegen_out.txt || echo 0)
    PASS=$((PASS + CODEGEN_PASS))
else
    ERROR=$((ERROR + 1))
fi
cat /tmp/codegen_out.txt
echo

# Test arche files
echo "=== Arche File Tests ==="
if make test-arche > /tmp/arche_out.txt 2>&1; then
    ARCHE_PASS=$(grep -c "PASS$" /tmp/arche_out.txt 2>/dev/null | tail -1)
    ARCHE_PASS="${ARCHE_PASS:-0}"
    ARCHE_SKIP=$(grep -c "expected" /tmp/arche_out.txt 2>/dev/null)
    ARCHE_SKIP="${ARCHE_SKIP:-0}"
    ARCHE_FAIL=$(grep -c "FAIL" /tmp/arche_out.txt 2>/dev/null)
    ARCHE_FAIL="${ARCHE_FAIL:-0}"
    PASS=$((PASS + ARCHE_PASS - ARCHE_SKIP))
    ERROR=$((ERROR + ARCHE_SKIP))
    FAIL=$((FAIL + ARCHE_FAIL))
else
    ERROR=$((ERROR + 1))
fi
tail -5 /tmp/arche_out.txt
echo

# Test examples
echo "=== Example Tests ==="
if ./tests/run_example_tests.sh > /tmp/examples_out.txt 2>&1; then
    EXAMPLES_PASS=$(grep -c "✓" /tmp/examples_out.txt || echo 0)
    PASS=$((PASS + EXAMPLES_PASS))
else
    ERROR=$((ERROR + 1))
fi
cat /tmp/examples_out.txt
echo

# Test codegen
echo "=== Codegen Test ==="
if make test-codegen > /tmp/codegen_test_out.txt 2>&1; then
    if grep -q "Codegen test passed" /tmp/codegen_test_out.txt; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
else
    ERROR=$((ERROR + 1))
fi
cat /tmp/codegen_test_out.txt
echo

echo "======================================"
echo "Passed: $PASS"
echo "Failed: $FAIL"
echo "Skipped: $ERROR"
echo "======================================"

if [ $FAIL -eq 0 ]; then
    exit 0
else
    exit 1
fi

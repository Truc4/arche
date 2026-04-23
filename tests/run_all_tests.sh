#!/bin/bash

PASS=0
FAIL=0
ERROR=0

# Colors
RED='\033[0;31m'      # Failed
GREEN='\033[0;32m'    # Passed
YELLOW='\033[1;33m'   # Error
BLUE='\033[0;34m'     # Headers
NC='\033[0m'          # No Color

# Test lexer
echo -e "${BLUE}=== Lexer Tests ===${NC}"
if LEXER_BIN=build/lexer-bin ./tests/run_lexer_tests.sh > /tmp/lexer_out.txt 2>&1; then
    LEXER_PASS=$(grep -c "✓" /tmp/lexer_out.txt 2>/dev/null || true)
    LEXER_PASS=${LEXER_PASS:-0}
    LEXER_FAIL=$(grep -c "✗" /tmp/lexer_out.txt 2>/dev/null || true)
    LEXER_FAIL=${LEXER_FAIL:-0}
    if [ $LEXER_FAIL -gt 0 ]; then
        grep "✗" /tmp/lexer_out.txt
    fi
    echo -e "${GREEN}✓ Passed: $LEXER_PASS${NC}, ${RED}Failed: $LEXER_FAIL${NC}"
    PASS=$((PASS + LEXER_PASS))
    FAIL=$((FAIL + LEXER_FAIL))
else
    echo -e "${YELLOW}⚠ ERROR${NC} running tests"
    ERROR=$((ERROR + 1))
fi
echo

# Test parser
echo -e "${BLUE}=== Parser Tests ===${NC}"
if ./build/parser-test > /tmp/parser_out.txt 2>&1; then
    PARSER_PASS=$(grep "Results:" /tmp/parser_out.txt 2>/dev/null | grep -oP '\d+(?=/)')
    PARSER_PASS=${PARSER_PASS:-0}
    PARSER_FAIL=$(grep "Results:" /tmp/parser_out.txt 2>/dev/null | tail -1 | grep -oP '\d+(?= failed)' || echo 0)
    PARSER_FAIL=${PARSER_FAIL:-0}
    if [ $PARSER_FAIL -gt 0 ]; then
        grep "✗" /tmp/parser_out.txt
    fi
    echo -e "${GREEN}✓ Passed: $PARSER_PASS${NC}, ${RED}Failed: $PARSER_FAIL${NC}"
    PASS=$((PASS + PARSER_PASS))
    FAIL=$((FAIL + PARSER_FAIL))
else
    echo -e "${YELLOW}⚠ ERROR${NC} running tests"
    ERROR=$((ERROR + 1))
fi
echo

# Test semantic
echo -e "${BLUE}=== Semantic Tests ===${NC}"
if ./build/semantic-test > /tmp/semantic_out.txt 2>&1; then
    SEMANTIC_PASS=$(grep "Results:" /tmp/semantic_out.txt 2>/dev/null | grep -oP '\d+(?=/)')
    SEMANTIC_PASS=${SEMANTIC_PASS:-0}
    SEMANTIC_FAIL=$(grep "Results:" /tmp/semantic_out.txt 2>/dev/null | tail -1 | grep -oP '\d+(?= failed)' || echo 0)
    SEMANTIC_FAIL=${SEMANTIC_FAIL:-0}
    if [ $SEMANTIC_FAIL -gt 0 ]; then
        grep "✗" /tmp/semantic_out.txt
    fi
    echo -e "${GREEN}✓ Passed: $SEMANTIC_PASS${NC}, ${RED}Failed: $SEMANTIC_FAIL${NC}"
    PASS=$((PASS + SEMANTIC_PASS))
    FAIL=$((FAIL + SEMANTIC_FAIL))
else
    echo -e "${YELLOW}⚠ ERROR${NC} running tests"
    ERROR=$((ERROR + 1))
fi
echo

# Test codegen unit
echo -e "${BLUE}=== Codegen Unit Tests ===${NC}"
if ./build/codegen-test > /tmp/codegen_out.txt 2>&1; then
    CODEGEN_PASS=$(grep "Results:" /tmp/codegen_out.txt 2>/dev/null | grep -oP '\d+(?=/)')
    CODEGEN_PASS=${CODEGEN_PASS:-0}
    CODEGEN_FAIL=$(grep "Results:" /tmp/codegen_out.txt 2>/dev/null | tail -1 | grep -oP '\d+(?= failed)' || echo 0)
    CODEGEN_FAIL=${CODEGEN_FAIL:-0}
    if [ $CODEGEN_FAIL -gt 0 ]; then
        grep "✗" /tmp/codegen_out.txt
    fi
    echo -e "${GREEN}✓ Passed: $CODEGEN_PASS${NC}, ${RED}Failed: $CODEGEN_FAIL${NC}"
    PASS=$((PASS + CODEGEN_PASS))
    FAIL=$((FAIL + CODEGEN_FAIL))
else
    echo -e "${YELLOW}⚠ ERROR${NC} running tests"
    ERROR=$((ERROR + 1))
fi
echo

# Test arche files
echo -e "${BLUE}=== Arche File Tests ===${NC}"
make test-arche > /tmp/arche_out.txt 2>&1 || true
ARCHE_PASS=$(grep -c "✓ PASS" /tmp/arche_out.txt 2>/dev/null || true)
ARCHE_PASS=${ARCHE_PASS:-0}
ARCHE_FAIL=$(grep -c "✗ FAIL" /tmp/arche_out.txt 2>/dev/null || true)
ARCHE_FAIL=${ARCHE_FAIL:-0}
ARCHE_ERROR=$(grep -c "⚠ ERROR" /tmp/arche_out.txt 2>/dev/null || true)
ARCHE_ERROR=${ARCHE_ERROR:-0}
if [ $ARCHE_FAIL -gt 0 ]; then
    echo -e "${RED}✗ FAILURES:${NC}"
    grep "✗ FAIL" /tmp/arche_out.txt
fi
if [ $ARCHE_ERROR -gt 0 ]; then
    echo -e "${YELLOW}⚠ ERRORS:${NC}"
    grep "⚠ ERROR" /tmp/arche_out.txt
fi
echo -e "${GREEN}✓ Passed: $ARCHE_PASS${NC}, ${RED}Failed: $ARCHE_FAIL${NC}, ${YELLOW}Error: $ARCHE_ERROR${NC}"
PASS=$((PASS + ARCHE_PASS))
FAIL=$((FAIL + ARCHE_FAIL))
ERROR=$((ERROR + ARCHE_ERROR))
echo

# Test examples
echo -e "${BLUE}=== Example Tests ===${NC}"
if ./tests/run_example_tests.sh > /tmp/examples_out.txt 2>&1; then
    EXAMPLES_PASS=$(sed 's/\x1b\[[0-9;]*m//g' /tmp/examples_out.txt 2>/dev/null | grep -c "PASS" || true)
    EXAMPLES_PASS=${EXAMPLES_PASS:-0}
    EXAMPLES_FAIL=$(sed 's/\x1b\[[0-9;]*m//g' /tmp/examples_out.txt 2>/dev/null | grep -c "FAIL" || true)
    EXAMPLES_FAIL=${EXAMPLES_FAIL:-0}
    if [ $EXAMPLES_FAIL -gt 0 ]; then
        echo -e "${RED}✗ FAILURES:${NC}"
        sed 's/\x1b\[[0-9;]*m//g' /tmp/examples_out.txt 2>/dev/null | grep "FAIL"
    fi
    echo -e "${GREEN}✓ Passed: $EXAMPLES_PASS${NC}, ${RED}Failed: $EXAMPLES_FAIL${NC}"
    PASS=$((PASS + EXAMPLES_PASS))
    FAIL=$((FAIL + EXAMPLES_FAIL))
else
    echo -e "${YELLOW}⚠ ERROR${NC} running tests"
    ERROR=$((ERROR + 1))
fi
echo

# Test codegen
echo -e "${BLUE}=== Codegen Test ===${NC}"
if make test-codegen > /tmp/codegen_test_out.txt 2>&1; then
    if grep -q "Codegen test passed" /tmp/codegen_test_out.txt; then
        echo -e "${GREEN}✓ Passed: 1${NC}"
        PASS=$((PASS + 1))
    else
        echo -e "${RED}✗ FAILED:${NC} hello_world codegen"
        FAIL=$((FAIL + 1))
    fi
else
    echo -e "${YELLOW}⚠ ERROR${NC} running tests"
    ERROR=$((ERROR + 1))
fi
echo

echo -e "${BLUE}======================================${NC}"
echo -e "TOTAL: ${GREEN}✓ Passed: $PASS${NC}, ${RED}✗ Failed: $FAIL${NC}, ${YELLOW}⚠ Error: $ERROR${NC}"
echo -e "${BLUE}======================================${NC}"

if [ $FAIL -eq 0 ]; then
    exit 0
else
    exit 1
fi

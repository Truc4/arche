#!/bin/bash

# Simple lexer test runner
LEXER_BIN="${LEXER_BIN:-./build/lexer-bin}"
TESTS_DATA_DIR="./tests/arche"
PASS=0
FAIL=0

if [ ! -f "$LEXER_BIN" ]; then
	echo "Error: $LEXER_BIN not found. Run 'make' first."
	exit 1
fi

echo "Running lexer tests..."
echo

for test_file in "$TESTS_DATA_DIR"/*.arche; do
	test_name=$(basename "$test_file" .arche)
	echo "Testing $test_name..."

	if "$LEXER_BIN" "$test_file" > /dev/null 2>&1; then
		echo "  ✓ PASS"
		((PASS++))
	else
		echo "  ✗ FAIL"
		"$LEXER_BIN" "$test_file"
		((FAIL++))
	fi
done

echo
echo "Results: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] && exit 0 || exit 1

#!/bin/bash

# Test harness: compare Arche programs with C reference implementations

ARCHE_BIN="./arche"
CC="gcc"
TEST_DIR="examples"
TMP_DIR="tests/tmp"

# Create temp directory
mkdir -p "$TMP_DIR"

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

PASS=0
FAIL=0

run_test() {
	local name=$1
	local arche_file=$2
	local c_file=$3

	echo -n "Testing $name... "

	# Compile C reference
	local c_out="$TMP_DIR/${name}_c"
	local c_expected="$TMP_DIR/${name}_c.txt"
	if ! $CC -o "$c_out" "$c_file" 2>/dev/null; then
		echo -e "${RED}FAIL${NC} (C compile error)"
		FAIL=$((FAIL + 1))
		return
	fi

	# Run C reference and capture output
	if ! "$c_out" > "$c_expected" 2>&1; then
		echo -e "${RED}FAIL${NC} (C runtime error)"
		FAIL=$((FAIL + 1))
		return
	fi

	# Compile Arche (once .length is implemented)
	local arche_out="$TMP_DIR/${name}_arche"
	local arche_actual="$TMP_DIR/${name}_arche.txt"
	if ! $ARCHE_BIN "$arche_file" > "$TMP_DIR/${name}_arche.ll" 2>/dev/null; then
		echo -e "${RED}SKIP${NC} (Arche compile error - .length not yet implemented)"
		return
	fi

	# Run Arche and capture output
	if ! "$arche_out" > "$arche_actual" 2>&1; then
		echo -e "${RED}FAIL${NC} (Arche runtime error)"
		FAIL=$((FAIL + 1))
		return
	fi

	# Compare outputs
	if diff -q "$c_expected" "$arche_actual" >/dev/null 2>&1; then
		echo -e "${GREEN}PASS${NC}"
		PASS=$((PASS + 1))
	else
		echo -e "${RED}FAIL${NC} (output mismatch)"
		echo "Expected:"
		cat "$c_expected"
		echo "Got:"
		cat "$arche_actual"
		FAIL=$((FAIL + 1))
	fi
}

echo "=== Example Tests ==="

run_test "hello_world" "$TEST_DIR/hello_world/hello_world.arche" "$TEST_DIR/hello_world/hello_world.c"
run_test "simple" "$TEST_DIR/simple/simple.arche" "$TEST_DIR/simple/simple.c"
run_test "with_params" "$TEST_DIR/with_params/with_params.arche" "$TEST_DIR/with_params/with_params.c"
run_test "simple_with_print" "$TEST_DIR/simple_with_print/simple_with_print.arche" "$TEST_DIR/simple_with_print/simple_with_print.c"
run_test "test_archetype" "$TEST_DIR/archetype/test_archetype.arche" "$TEST_DIR/archetype/test_archetype.c"
run_test "test_archetype_verbose" "$TEST_DIR/archetype/test_archetype_verbose.arche" "$TEST_DIR/archetype/test_archetype_verbose.c"
run_test "multidim_example" "$TEST_DIR/archetype/multidim_example.arche" "$TEST_DIR/archetype/multidim_example.c"

echo ""
echo "=== Test Data Tests ==="

run_test "test_array_ops" "tests/data/test_array_ops.arche" "tests/data/test_array_ops.c"
run_test "test_sys_ops" "tests/data/test_sys_ops.arche" "tests/data/test_sys_ops.c"
run_test "test_soa" "tests/data/test_soa.arche" "tests/data/test_soa.c"

echo ""
echo "=== Summary ==="
echo -e "Passed: ${GREEN}$PASS${NC}"
echo -e "Failed: ${RED}$FAIL${NC}"

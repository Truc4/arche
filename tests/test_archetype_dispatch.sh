#!/bin/bash
ARCHE="/home/curt/Code/arche/arche"
TESTS_DIR="/home/curt/Code/arche/tests/arche"

tests=(
  "test_arch_single_sys_single_arch.arche"
  "test_arch_single_sys_two_archs.arche"
  "test_arch_two_sys_two_archs.arche"
  "test_arch_partial_field_match.arche"
  "test_arch_insert_then_run_both.arche"
)

passed=0
failed=0

for test in "${tests[@]}"; do
  echo "Running $test..."
  $ARCHE "$TESTS_DIR/$test" -o /tmp/test_prog 2>&1
  if [ $? -eq 0 ]; then
    /tmp/test_prog > /tmp/output.txt 2>&1
    if [ $? -eq 0 ]; then
      echo "  ✓ PASS"
      ((passed++))
    else
      echo "  ✗ FAIL (execution error)"
      cat /tmp/output.txt
      ((failed++))
    fi
  else
    echo "  ✗ FAIL (compilation error)"
    ((failed++))
  fi
done

echo ""
echo "Results: $passed passed, $failed failed out of ${#tests[@]} tests"

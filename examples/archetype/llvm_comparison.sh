#!/bin/bash

# Generate and compare LLVM IR for Arche vs hand-written C equivalent

echo "========================================"
echo "LLVM IR Comparison: Archetype Example"
echo "========================================"

# Generate LLVM from Arche
echo ""
echo "1. GENERATING ARCHE LLVM IR..."
./arche examples/archetype/test_archetype.arche -emit-llvm -o /tmp/arche_generated.ll 2>&1 | grep -E "Generated|Error" || true

# Compile C to LLVM for comparison
echo ""
echo "2. GENERATING C LLVM IR..."
clang -S -emit-llvm -o /tmp/c_generated.ll examples/archetype/archetype_equiv.c 2>&1 || echo "Note: clang may not be available, skipping C LLVM generation"

# Compare struct definitions
echo ""
echo "======== ARCHE GENERATED STRUCTS ========"
grep -A 5 "%struct\." /tmp/arche_generated.ll | head -30

echo ""
echo "======== ARCHE GENERATED FUNCTIONS ========"
grep -A 10 "^define void @" /tmp/arche_generated.ll | head -40

echo ""
echo "========================================"
echo "Verification: Arche systems are compiled as LLVM functions"
echo "========================================"

if grep -q "define void @move" /tmp/arche_generated.ll; then
    echo "✓ Found 'move' system compiled to LLVM function"
fi

if grep -q "define void @dampen" /tmp/arche_generated.ll; then
    echo "✓ Found 'dampen' system compiled to LLVM function"
fi

if grep -q "define void @initialize" /tmp/arche_generated.ll; then
    echo "✓ Found 'initialize' procedure compiled to LLVM function"
fi

echo ""
echo "LLVM files saved to:"
echo "  - Arche: /tmp/arche_generated.ll"
echo "  - C:     /tmp/c_generated.ll (if clang available)"

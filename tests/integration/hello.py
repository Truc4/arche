#!/usr/bin/env python3
"""
Integration test: Compile and run hello_world.arche program.
RUN: python3 %s
"""

import subprocess
import os
import sys

# Get paths
test_dir = os.path.dirname(os.path.abspath(__file__))
repo_root = os.path.join(test_dir, '..', '..')
arche_bin = os.path.join(repo_root, 'build', 'arche')
test_prog = os.path.join(repo_root, 'examples', 'hello_world', 'hello_world.arche')
test_out = os.path.join(repo_root, 'build', 'test-integration-hello')

# Compile
result = subprocess.run([arche_bin, '-o', test_out, test_prog],
                       capture_output=True, text=True)
if result.returncode != 0:
    print(f"FAIL: Compilation failed")
    print(result.stderr)
    sys.exit(1)

# Run
result = subprocess.run([test_out], capture_output=True, text=True)
if result.returncode != 0:
    print(f"FAIL: Program execution failed")
    sys.exit(1)

# Check output
if "Hello, World!" not in result.stdout:
    print(f"FAIL: Expected 'Hello, World!' in output, got: {result.stdout}")
    sys.exit(1)

print("PASS: hello_world integration test")

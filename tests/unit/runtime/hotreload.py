#!/usr/bin/env python3
"""
Unit test: hot-reload runtime (runtime/hotreload.c) — runs the deterministic C unit binary.
RUN: python3 %s
"""

import os
import subprocess
import sys


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.join(script_dir, '../../..')
    binary = os.path.join(repo_root, 'build', 'hotreload-test')

    if not os.path.exists(binary):
        print(f"FAIL: Binary not found: {binary}")
        return 1

    result = subprocess.run([binary], cwd=repo_root, capture_output=True, text=True)
    print(result.stdout, end='')
    if result.stderr:
        print(result.stderr, end='', file=sys.stderr)
    return result.returncode


if __name__ == '__main__':
    sys.exit(main())

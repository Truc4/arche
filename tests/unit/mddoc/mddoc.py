#!/usr/bin/env python3
"""
Unit test: markdown (`.md`) doctests.

`arche test` extracts every ```arche fenced block from a `.md` file, compiles it as a
STANDALONE program (fmt auto-imported, loose statements wrapped in main), and runs it.
A `.md` block must compile + exit 0 — there are no opt-out flags in our docs.

`.md` files can't carry an inline lit run directive and aren't a lit suffix, so this
Python harness drives the fixtures and asserts behavior.

RUN: python3 %s %arche
"""

import subprocess
import sys
import os


def run(arche, md):
    here = os.path.dirname(os.path.abspath(__file__))
    return subprocess.run([arche, "test", "-v", os.path.join(here, md)],
                          capture_output=True, text=True)


def main():
    arche = sys.argv[1]
    fails = []

    # Passing fixtures: every ```arche block runs and exits 0.
    for md in ("passing.md", "standalone.md", "sections.md"):
        r = run(arche, md)
        if r.returncode != 0:
            fails.append(f"{md}: expected pass, got exit {r.returncode}\n{r.stdout}\n{r.stderr}")

    # sections.md reuses the name `helper` and `#module` in two sections; section isolation
    # must keep them from colliding, and the two statement blocks both run.
    r = run(arche, "sections.md")
    if r.stdout.count("--- PASS") != 2:
        fails.append(f"sections.md: expected 2 runnable blocks across isolated sections:\n{r.stdout}")

    # passing.md has exactly one extractable block (plain ``` and ```python are skipped).
    r = run(arche, "passing.md")
    if r.stdout.count("--- PASS") != 1:
        fails.append(f"passing.md: expected 1 extracted block, output:\n{r.stdout}")

    # standalone.md has two blocks (own-main, own-import-with-fmt-merge).
    r = run(arche, "standalone.md")
    if r.stdout.count("--- PASS") != 2:
        fails.append(f"standalone.md: expected 2 blocks, output:\n{r.stdout}")

    # Failing fixture: a runtime-failing block must make `arche test` exit non-zero and
    # report the block by source line.
    r = run(arche, "failing.md")
    if r.returncode == 0:
        fails.append("failing.md: expected non-zero exit, got 0")
    if "FAIL" not in r.stdout or "failing.md" not in r.stdout:
        fails.append(f"failing.md: expected a FAIL line naming the file:\n{r.stdout}")
    if "line 5" not in r.stdout:
        fails.append(f"failing.md: expected the failing block's fence line (5):\n{r.stdout}")

    if fails:
        for f in fails:
            print("FAIL:", f)
        return 1
    print("ok: markdown doctests")
    return 0


if __name__ == "__main__":
    sys.exit(main())

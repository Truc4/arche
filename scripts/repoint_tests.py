#!/usr/bin/env python3
"""Repoint test call sites at the new stdlib modules after draining core.arche.

Rewrites bare calls to relocated symbols into qualified `mod.sym(` form and adds a single merged
`#import { ... }` per file. Optimistic: writes the qualified spelling even if the compiler does
not yet support it. Conservative about collisions:
  - skips pure-comment lines (lstrip startswith //)
  - only matches a bare identifier NOT preceded by '.', word-char, and immediately followed by '('
  - does NOT touch type refs `file`/`socket` (comment-collision prone — handled manually)
"""
import re, sys, pathlib

# symbol -> module
CALLS = {
    "printf": "fmt", "sprintf": "fmt", "fflush": "fmt",
    "print": "fmt", "print_float": "fmt", "assert": "fmt",
    "arche_file_map": "io", "arche_file_size": "io", "arche_file_unmap": "io",
    "syscall": "sys", "open": "sys", "read": "sys", "write": "sys",
    "close": "sys", "lseek": "sys", "exit": "sys",
    "streq": "str",
}
# longest-first so print_float / arche_file_* match before print / shorter prefixes
SYMS = sorted(CALLS, key=len, reverse=True)
PAT = {s: re.compile(r'(?<![\w.])' + re.escape(s) + r'(\s*\()') for s in SYMS}

root = pathlib.Path("tests")
changed = []
mod_hits = {}

for path in sorted(root.rglob("*.arche")):
    lines = path.read_text().splitlines(keepends=True)
    needed = set()
    out = []
    for line in lines:
        if line.lstrip().startswith("//"):
            out.append(line); continue
        for s in SYMS:
            new, n = PAT[s].subn(CALLS[s] + "." + s + r"\1", line)
            if n:
                needed.add(CALLS[s]); line = new
                mod_hits[CALLS[s]] = mod_hits.get(CALLS[s], 0) + n
        out.append(line)
    if not needed:
        continue
    text = "".join(out)

    # merge into an existing `#import { ... }` if present, else insert a fresh line after the
    # leading comment/blank block.
    m = re.search(r'#import\s*\{([^}]*)\}', text)
    if m:
        present = set(m.group(1).split())
        merged = present | needed
        # preserve a stable order: existing first, then new
        ordered = list(dict.fromkeys(m.group(1).split() + sorted(needed - present)))
        text = text[:m.start()] + "#import { " + " ".join(ordered) + " }" + text[m.end():]
    else:
        src_lines = text.splitlines(keepends=True)
        i = 0
        while i < len(src_lines) and (src_lines[i].lstrip().startswith("//") or src_lines[i].strip() == ""):
            i += 1
        imp = "#import { " + " ".join(sorted(needed)) + " }\n"
        src_lines.insert(i, imp)
        text = "".join(src_lines)

    path.write_text(text)
    changed.append(str(path))

print(f"files changed: {len(changed)}")
print("substitutions per module:", mod_hits)

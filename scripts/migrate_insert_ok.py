#!/usr/bin/env python3
"""Codemod: migrate insert/delete call sites to the mandatory-ok out-list form.

  insert(P, a);            -> insert(P, a)(_:, _:);     (bare statement, discard)
  h := insert(P, a);       -> insert(P, a)(h:, _:);     (bind: handle kept, ok discarded)
  delete(h);               -> delete(h)(_:);            (bare statement)

Nested calls (i32(insert(...)), insert(P2, insert(...)), := i32(insert ...)) and any call that
already has an out-list are left untouched and reported for manual handling. delete sites are also
reported because the enclosing proc must be marked `proc!`.
"""
import re, sys

def find_call_end(s, open_paren):
    depth = 0
    i = open_paren
    while i < len(s):
        if s[i] == '(':
            depth += 1
        elif s[i] == ')':
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1

def migrate(text, fname):
    out = []
    i = 0
    nested = []
    deletes = []
    for m in re.finditer(r'\b(insert|delete)\s*\(', text):
        pass
    # process line-aware to keep it simple/safe
    lines = text.split('\n')
    new_lines = []
    for ln, line in enumerate(lines, 1):
        m = re.search(r'\b(insert|delete)\s*\(', line)
        if not m:
            new_lines.append(line); continue
        kind = m.group(1)
        op = line.index('(', m.start())
        end = find_call_end(line, op)
        if end < 0:
            new_lines.append(line); nested.append((ln, line.strip(), "multiline/unbalanced")); continue
        after = line[end+1:].lstrip()
        if after.startswith('('):
            new_lines.append(line); continue  # already has out-list
        # must be a statement: trailing `;`
        if not after.startswith(';'):
            new_lines.append(line); nested.append((ln, line.strip(), "nested/non-stmt")); continue
        before = line[:m.start()]
        bm = re.match(r'^(\s*)([A-Za-z_][A-Za-z0-9_]*)\s*:=\s*$', before)
        call = line[m.start():end+1]
        tail = line[end+1:]  # starts with ;
        if kind == 'insert':
            if bm:
                indent, name = bm.group(1), bm.group(2)
                new_lines.append(f"{indent}{call}({name}:, _:){tail}")
            elif before.strip() == '':
                new_lines.append(f"{before}{call}(_:, _:){tail}")
            else:
                new_lines.append(line); nested.append((ln, line.strip(), "insert-with-prefix")); continue
        else:  # delete
            deletes.append((ln, line.strip()))
            if bm:
                new_lines.append(line); nested.append((ln, line.strip(), "delete-bind?")); continue
            if before.strip() == '':
                new_lines.append(f"{before}{call}(_:){tail}")
            else:
                new_lines.append(line); nested.append((ln, line.strip(), "delete-with-prefix")); continue
    return '\n'.join(new_lines), nested, deletes

if __name__ == '__main__':
    any_nested = False
    for fname in sys.argv[1:]:
        with open(fname) as f:
            text = f.read()
        new, nested, deletes = migrate(text, fname)
        if new != text:
            with open(fname, 'w') as f:
                f.write(new)
        for (ln, s, why) in nested:
            any_nested = True
            print(f"NESTED  {fname}:{ln}: [{why}] {s}")
        for (ln, s) in deletes:
            print(f"DELETE  {fname}:{ln}: {s}")
    sys.exit(0)

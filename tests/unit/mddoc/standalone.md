# Shared-context markdown doctests

A `.md` page shares ONE declaration scope across all its ```arche blocks. Declare helpers
once:

```arche
add :: func(a: int, b: int) -> int {
  return a + b;
}

triple :: func(n: int) -> int {
  return add(n, add(n, n));
}
```

A later block reuses them with no re-setup — the declarations above are in scope:

```arche
fmt.assert(add(2, 3) == 5, "add broken\n")();
fmt.assert(triple(4) == 12, "triple broken\n")();
```

Imports are pooled too (declared once, available everywhere); `fmt` is always present:

```arche
#import { str }
```

```arche
fmt.assert(str.strlen("hello") == 5, "strlen broken\n")();
```

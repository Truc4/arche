# A deliberately failing block

Used by the harness to confirm `arche test` reports `.md` failures with `file:line`.

```arche
fmt.assert(1 == 2, "this block fails at runtime\n");
```

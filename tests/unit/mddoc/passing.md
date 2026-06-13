# Passing markdown doctests

A loose-statement block (wrapped in `main`, `fmt` auto-imported):

```arche
x := 2 + 3;
fmt.assert(x == 5, "math broken\n");
```

Plain fences without the `arche` info string are NOT extracted:

```
this is illustration, never compiled
```

```python
print("ignored too")
```

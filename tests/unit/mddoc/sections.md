# Section isolation

Each markdown heading starts a fresh shared-context scope. The two sections below both
define `helper` — and both `#module` — yet do NOT collide, because a section is isolated
from the others.

## Section A

```arche
#module
helper :: func() -> int { return 1; }
```

```arche
fmt.assert(helper() == 1, "section A broken\n");
```

## Section B

```arche
#module
helper :: func() -> int { return 2; }
```

```arche
fmt.assert(helper() == 2, "section B broken\n");
```

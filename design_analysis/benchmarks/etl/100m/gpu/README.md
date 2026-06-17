# GPU backend (placeholder)

Intended: a GPU **tree-fold reduce** for the aggregate/sum tasks — reusing the `@gpu` map dispatch
(GLSL workgroup reduction → per-workgroup partials → CPU finish). NOT built yet: arche's `@gpu` attaches
only to `run map`; collectives (`reduce`/`scan`/`sort`) have no GPU path (see docs/OPEN_ITEMS.md).

Note: over CPU-resident data a one-shot GPU reduce is dominated by the host→device transfer (column
residency is unbuilt), so it is expected to be *slower* than the CPU SIMD reduce until residency lands.

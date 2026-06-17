# Multicore category

Multi-threaded engines — a **different performance category** from the single-threaded peers
(arche, C, pandas). They use all cores + hand-tuned SIMD CSV readers, so comparing them head-to-head
with single-threaded arche is apples-to-oranges (see ../../../README.md).

- `polars/`, `duckdb/`, `datafusion/` — third-party multicore baselines.
- A future arche **multicore reduce** (split the column across cores, fold each chunk, combine partials
  with the monoid — associativity makes this exact) belongs here too. Not built yet: arche is
  single-threaded by design (no threading primitive; see docs/performance.md).

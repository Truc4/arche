#!/usr/bin/env python3
"""Task 1 (DuckDB, transform-only): revenue = price * quantity, repeated N times.

Table is loaded into an in-memory DuckDB DB once; each iter recomputes the result
table via CREATE OR REPLACE — entirely server-side, no row transfer to Python.
"""

import os
import sys
import time
import duckdb

DEFAULT_CSV = "design_analysis/benchmarks/etl/data/data_10k.csv"
N_ITERS = 216_000
OUT_DIR = "design_analysis/benchmarks/transform/out/duckdb"


def main(csv_path):
    con = duckdb.connect(":memory:")
    con.execute(f"CREATE TABLE t AS SELECT price, quantity FROM read_csv('{csv_path}')")

    start = time.perf_counter()
    for _ in range(N_ITERS):
        con.execute("CREATE OR REPLACE TABLE result AS SELECT price * quantity AS revenue FROM t")
    elapsed = time.perf_counter() - start

    checksum = con.execute("SELECT SUM(revenue) FROM result").fetchone()[0]

    os.makedirs(OUT_DIR, exist_ok=True)
    con.execute(f"COPY (SELECT revenue FROM result) TO '{OUT_DIR}/task_1.csv' (HEADER, DELIMITER ',')")

    print(f"task1_checksum: {checksum}")
    print(f"task1_time: {elapsed / N_ITERS}")


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    main(csv_path)

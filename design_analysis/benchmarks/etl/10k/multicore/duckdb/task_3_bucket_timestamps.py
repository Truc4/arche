#!/usr/bin/env python3
"""Task 3 (DuckDB, transform-only): price_bucket = price / 10.0, repeated N times. In-memory."""

import os
import sys
import time
import duckdb

DEFAULT_CSV = "design_analysis/benchmarks/etl/data/data_10k.csv"
N_ITERS = 216_000
OUT_DIR = "design_analysis/benchmarks/etl/out/duckdb"


def main(csv_path):
    con = duckdb.connect(":memory:")
    con.execute(f"CREATE TABLE t AS SELECT price FROM read_csv('{csv_path}')")

    start = time.perf_counter()
    for _ in range(N_ITERS):
        con.execute("CREATE OR REPLACE TABLE result AS SELECT price / 10.0 AS price_bucket FROM t")
    elapsed = time.perf_counter() - start

    checksum = con.execute("SELECT SUM(price_bucket) FROM result").fetchone()[0]

    os.makedirs(OUT_DIR, exist_ok=True)
    con.execute(f"COPY (SELECT price_bucket FROM result) TO '{OUT_DIR}/task_3.csv' (HEADER, DELIMITER ',')")

    print(f"task3_checksum: {checksum}")
    print(f"task3_time: {elapsed / N_ITERS}")


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    main(csv_path)

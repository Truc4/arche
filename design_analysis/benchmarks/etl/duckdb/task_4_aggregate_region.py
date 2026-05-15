#!/usr/bin/env python3
"""Task 4 (DuckDB): sum(price * quantity). Matches arche_scale/task_4 checksum semantics."""

import sys
import time
import duckdb

DEFAULT_CSV = "design_analysis/benchmarks/etl/data/data_100m.csv"


def main(csv_path):
    start = time.perf_counter()
    checksum = duckdb.sql(
        f"SELECT SUM(price * quantity) FROM read_csv('{csv_path}')"
    ).fetchone()[0]
    elapsed = time.perf_counter() - start
    print(f"task4_checksum: {checksum}")
    print(f"task4_time: {elapsed}")


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    main(csv_path)

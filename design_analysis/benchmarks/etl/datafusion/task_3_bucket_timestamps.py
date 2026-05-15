#!/usr/bin/env python3
"""Task 3 (DataFusion): sum(price / 10.0). Matches arche_scale/task_3 checksum semantics."""

import sys
import time
from datafusion import SessionContext

DEFAULT_CSV = "design_analysis/benchmarks/etl/data/data_100m.csv"


def main(csv_path):
    ctx = SessionContext()
    start = time.perf_counter()
    ctx.register_csv("t", csv_path)
    batches = ctx.sql("SELECT SUM(price / 10.0) FROM t").collect()
    elapsed = time.perf_counter() - start
    checksum = batches[0].column(0)[0].as_py()
    print(f"task3_checksum: {checksum}")
    print(f"task3_time: {elapsed}")


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    main(csv_path)

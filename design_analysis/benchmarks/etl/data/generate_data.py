#!/usr/bin/env python3
"""Generate large ETL benchmark dataset."""

import sys
import random
from datetime import datetime, timedelta

def generate_data(num_rows, output_file):
    """Generate CSV with num_rows of transaction data."""
    print(f"Generating {num_rows:,} rows...")

    base_date = datetime(2023, 12, 19, 0, 0, 0)
    regions = ["US", "EU", "APAC", "LATAM", "EMEA"]

    with open(output_file, 'w') as f:
        f.write("timestamp,price,quantity,region,flags\n")

        for i in range(num_rows):
            if (i + 1) % 10000000 == 0:
                print(f"  {i + 1:,} rows written...")

            ts = base_date + timedelta(seconds=i % 86400)
            price = round(5.0 + random.random() * 15.0, 1)
            # ~10% of rows have non-positive quantity (returns / invalid records)
            # so `quantity > 0` is a meaningful filter, not a no-op.
            quantity = random.randint(-50, 500)
            region = regions[random.randint(0, len(regions) - 1)]
            flags = random.randint(0, 1)

            f.write(f"{ts.isoformat()},{price},{quantity},{region},{flags}\n")

    print(f"Done! Created {output_file}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 generate_data.py <num_rows> [output_file]")
        sys.exit(1)

    num_rows = int(sys.argv[1])
    output_file = sys.argv[2] if len(sys.argv) > 2 else "data.csv"

    generate_data(num_rows, output_file)

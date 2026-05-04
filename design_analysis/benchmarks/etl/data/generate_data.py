#!/usr/bin/env python3
"""Generate benchmark dataset: millions of rows with price, quantity, timestamp, region, flags."""

import csv
import random
from datetime import datetime, timedelta
import sys

def generate_dataset(num_rows, output_file):
    regions = ["US-East", "US-West", "EU", "APAC", "LATAM"]

    start_date = datetime(2023, 1, 1)

    with open(output_file, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(["timestamp", "price", "quantity", "region", "flags"])

        for i in range(num_rows):
            timestamp = start_date + timedelta(seconds=random.randint(0, 365*86400))
            price = round(random.uniform(1.0, 1000.0), 2)
            quantity = random.randint(-100, 10000)  # include some invalid (negative)
            region = random.choice(regions)
            flags = random.randint(0, 255)

            writer.writerow([timestamp.isoformat(), price, quantity, region, flags])

            if (i + 1) % 100000 == 0:
                print(f"Generated {i + 1} rows...", file=sys.stderr)

    print(f"Dataset written to {output_file}", file=sys.stderr)

if __name__ == "__main__":
    num_rows = int(sys.argv[1]) if len(sys.argv) > 1 else 1_000_000
    output = sys.argv[2] if len(sys.argv) > 2 else "data.csv"
    generate_dataset(num_rows, output)

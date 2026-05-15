#!/usr/bin/env python3
"""Generate a 'dirty' / real-world CSV variant.

The clean generator (generate_data.py) produces uniform rows with no quoting,
no nulls, and identical line widths. Real-world CSVs are messier. This variant
adds:

  - quoted region values when the region name contains punctuation (e.g. "EU, Central")
  - occasional NULL/empty fields for quantity (≈0.5%)
  - mixed line endings (≈1% of rows use CRLF instead of LF)
  - irregular numeric widths for price (≈10% of rows get extra decimal places)
  - occasional embedded commas inside quoted region (≈0.5%)

The schema (columns, types, range) is otherwise compatible with the clean
dataset, so the same parsers can in principle handle both — that is the point.
Engines that silently accept and skip malformed rows vs. crash vs. coerce are
the real signal here.

NOT used by the default benchmark runner. Generate explicitly:

    python3 generate_data_dirty.py 1000000 design_analysis/benchmarks/etl/data/data_dirty.csv

Then point compare_scale.py at it via --csv.
"""

import random
import sys
from datetime import datetime, timedelta

# Regions: a mix of clean and dirty (commas, spaces) to force quoting.
REGIONS = [
    "US",
    "EU",
    "APAC",
    "LATAM",
    "EMEA",
    "EU, Central",
    "APAC, North",
    "US-West",
]


def maybe_quote(region):
    """Quote the region if it contains a comma. Always quote if the field
    has any punctuation that confuses naive split-on-comma parsers."""
    if "," in region or '"' in region or " " in region:
        return '"' + region.replace('"', '""') + '"'
    return region


def format_price(rng):
    """Most rows: one decimal place. ~10% of rows: extra precision."""
    base = 5.0 + rng.random() * 15.0
    if rng.random() < 0.10:
        return f"{base:.4f}"
    return f"{base:.1f}"


def format_quantity(rng):
    """~0.5% of rows have empty quantity to simulate missing data."""
    if rng.random() < 0.005:
        return ""
    return str(rng.randint(1, 500))


def main(num_rows, output_file, seed=None):
    rng = random.Random(seed)
    print(f"Generating {num_rows:,} dirty rows -> {output_file}")

    base_date = datetime(2023, 12, 19, 0, 0, 0)

    with open(output_file, "w", newline="") as f:
        f.write("timestamp,price,quantity,region,flags\n")

        for i in range(num_rows):
            if (i + 1) % 1_000_000 == 0:
                print(f"  {i + 1:,} rows...")

            ts = (base_date + timedelta(seconds=i % 86400)).isoformat()
            price = format_price(rng)
            quantity = format_quantity(rng)
            region = maybe_quote(REGIONS[rng.randint(0, len(REGIONS) - 1)])
            flags = rng.randint(0, 1)

            eol = "\r\n" if rng.random() < 0.01 else "\n"
            f.write(f"{ts},{price},{quantity},{region},{flags}{eol}")

    print(f"Done! Created {output_file}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 generate_data_dirty.py <num_rows> [output_file] [seed]")
        sys.exit(1)
    rows = int(sys.argv[1])
    out = sys.argv[2] if len(sys.argv) > 2 else "data_dirty.csv"
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else None
    main(rows, out, seed)

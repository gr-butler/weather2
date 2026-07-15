#!/usr/bin/env python3
"""Backfill weather_observations from the Prometheus long-term store.

The original PostgreSQL history was lost in the server's OS/PG upgrade, but
Prometheus retained ~2.2 years of the same weather metrics. This script exports
that history to a CSV on the weather_observations 15-minute grid, which you then
load into Postgres (see the load procedure printed at the end and in
weather_schema.sql).

Design notes:
  * Standard library only (urllib + csv) — no pip installs, runs anywhere that
    can reach Prometheus.
  * Uses the Prometheus range API (/api/v1/query_range) at a 15-minute step to
    match the ongoing Telegraf cadence, chunking the time range to stay under
    Prometheus's 11 000-points-per-query limit.
  * Instantaneous gauge value at each grid point (same as a Telegraf scrape).

Usage:
  PROM_URL=http://localhost:9090 \
  python3 prometheus_backfill.py --out weather_history.csv
  # optional: --start 2024-05-02 --end 2026-07-15 --step 900

Then load into Postgres (idempotent):
  CREATE TEMP TABLE _wo_stage (LIKE weather_observations INCLUDING DEFAULTS);
  \\copy _wo_stage (time,temperature_c,humidity_pct,pressure_hpa,rain_rate_mm_min,rain_day_mm,wind_speed_mph,wind_gust_mph,wind_direction_deg,river_level_m) FROM 'weather_history.csv' WITH (FORMAT csv, HEADER true)
  INSERT INTO weather_observations SELECT * FROM _wo_stage ON CONFLICT (time) DO NOTHING;
"""

import argparse
import csv
import datetime as dt
import json
import os
import sys
import urllib.parse
import urllib.request

# Prometheus metric name -> weather_observations column name.
# Order here defines the CSV column order (after `time`).
# weather_observations column -> Prometheus metric name(s). When several names
# are given they are tried in order and merged, covering metrics that were
# renamed over the years (e.g. river level). Order defines the CSV columns.
COLUMN_TO_METRICS = [
    ("temperature_c", ["temperature"]),
    ("humidity_pct", ["relative_humidity"]),
    ("pressure_hpa", ["atmospheric_pressure"]),
    ("rain_rate_mm_min", ["rain_min_rate"]),
    ("rain_day_mm", ["rain_day"]),
    ("wind_speed_mph", ["windspeed"]),
    ("wind_gust_mph", ["windgust"]),
    ("wind_direction_deg", ["winddirection"]),
    ("river_level_m", ["riverlevel", "river_level"]),
]

COLUMNS = [col for col, _ in COLUMN_TO_METRICS]

# Prometheus limits a single query_range to 11 000 points; stay well under it.
MAX_POINTS_PER_QUERY = 10000


def prom_query_range(base_url, query, start, end, step):
    """Return list of (unix_ts_int, value_str) for a metric over [start, end]."""
    params = urllib.parse.urlencode(
        {"query": query, "start": start, "end": end, "step": step}
    )
    url = f"{base_url}/api/v1/query_range?{params}"
    with urllib.request.urlopen(url, timeout=120) as resp:
        payload = json.load(resp)
    if payload.get("status") != "success":
        raise RuntimeError(f"Prometheus error for {query!r}: {payload}")
    # Flatten every returned series. A metric may span more than one series if
    # the instance/job labels changed over the years (old Go app vs ESP32);
    # merging them keeps the whole history. fetch_column dedups per time bucket.
    points = []
    for series in payload["data"]["result"]:
        for ts, val in series["values"]:
            points.append((int(ts), val))
    return points


def fetch_column(base_url, metric_names, start, end, step, grid, column):
    """Fill grid[bucket][column] from the given metric name(s), chunked."""
    window = step * MAX_POINTS_PER_QUERY
    count = 0
    for name in metric_names:
        cursor = start
        while cursor <= end:
            chunk_end = min(cursor + window, end)
            for ts, val in prom_query_range(base_url, name, cursor, chunk_end, step):
                bucket = ts - (ts % step)
                slot = grid.setdefault(bucket, {})
                if column not in slot:  # first name / first sample wins
                    slot[column] = val
                    count += 1
            cursor = chunk_end + step
    return count


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--prom-url", default=os.environ.get("PROM_URL", "http://localhost:9090"))
    ap.add_argument("--out", default="weather_history.csv")
    ap.add_argument("--start", help="ISO date/time (UTC). Default: 800 days ago.")
    ap.add_argument("--end", help="ISO date/time (UTC). Default: now.")
    ap.add_argument("--step", type=int, default=900, help="Grid seconds (default 900 = 15 min).")
    args = ap.parse_args()

    def parse_iso(s):
        return int(dt.datetime.fromisoformat(s).replace(tzinfo=dt.timezone.utc).timestamp())

    now = int(dt.datetime.now(dt.timezone.utc).timestamp())
    end = parse_iso(args.end) if args.end else now
    start = parse_iso(args.start) if args.start else end - 800 * 86400
    start -= start % args.step
    end -= end % args.step

    def iso(ts):
        return dt.datetime.fromtimestamp(ts, dt.timezone.utc).strftime("%Y-%m-%d %H:%M:%S")

    print(f"Exporting {args.prom_url} -> {args.out}", file=sys.stderr)
    print(f"  range {iso(start)}Z .. {iso(end)}Z  step {args.step}s", file=sys.stderr)

    grid = {}
    for column, metric_names in COLUMN_TO_METRICS:
        n = fetch_column(args.prom_url, metric_names, start, end, args.step, grid, column)
        print(f"  {column}: {n} points ({'/'.join(metric_names)})", file=sys.stderr)

    rows = sorted(grid)
    with open(args.out, "w", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(["time"] + COLUMNS)
        for ts in rows:
            vals = grid[ts]
            writer.writerow([iso(ts) + "+00"] + [vals.get(col, "") for col in COLUMNS])

    print(f"Wrote {len(rows)} rows to {args.out}", file=sys.stderr)
    print("\nLoad into Postgres (idempotent):", file=sys.stderr)
    print("  CREATE TEMP TABLE _wo_stage (LIKE weather_observations INCLUDING DEFAULTS);", file=sys.stderr)
    print(f"  \\copy _wo_stage (time,{','.join(COLUMNS)}) FROM '{args.out}' WITH (FORMAT csv, HEADER true)", file=sys.stderr)
    print("  INSERT INTO weather_observations SELECT * FROM _wo_stage ON CONFLICT (time) DO NOTHING;", file=sys.stderr)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Plot the output of run_bench.sh.

Reads results/summary.csv and writes two charts to results/:
  throughput.png  requests per second against connection count, per server
  latency.png     p50 and p99 latency against connection count, per server

Usage: python3 bench/plot_results.py [results_dir]
"""

import csv
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RESULTS = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "results"


def load(path):
    rows = defaultdict(list)
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            rows[row["server"]].append(
                {
                    "connections": int(row["connections"]),
                    "throughput": float(row["throughput_rps"]),
                    "p50": float(row["p50_us"]),
                    "p99": float(row["p99_us"]),
                    "errors": int(row["errors"]),
                }
            )
    for server in rows:
        rows[server].sort(key=lambda r: r["connections"])
    return rows


def plot_throughput(data, out):
    fig, ax = plt.subplots(figsize=(9, 5.5))
    for server, points in sorted(data.items()):
        ax.plot(
            [p["connections"] for p in points],
            [p["throughput"] for p in points],
            marker="o",
            label=server.replace("_server", ""),
        )
    ax.set_xscale("log", base=2)
    ax.set_xlabel("concurrent connections")
    ax.set_ylabel("throughput (requests/s)")
    ax.set_title("Throughput against concurrency")
    ax.grid(True, which="both", alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(out, dpi=140)
    print(f"wrote {out}")


def plot_latency(data, out):
    fig, ax = plt.subplots(figsize=(9, 5.5))
    for server, points in sorted(data.items()):
        label = server.replace("_server", "")
        line, = ax.plot(
            [p["connections"] for p in points],
            [p["p50"] for p in points],
            marker="o",
            label=f"{label} p50",
        )
        ax.plot(
            [p["connections"] for p in points],
            [p["p99"] for p in points],
            marker="^",
            linestyle="--",
            color=line.get_color(),
            alpha=0.6,
            label=f"{label} p99",
        )
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xlabel("concurrent connections")
    ax.set_ylabel("latency (microseconds, log scale)")
    ax.set_title("Median and tail latency against concurrency")
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=8, ncol=2)
    fig.tight_layout()
    fig.savefig(out, dpi=140)
    print(f"wrote {out}")


def main():
    summary = RESULTS / "summary.csv"
    if not summary.exists():
        sys.exit(f"{summary} not found. Run bench/run_bench.sh first.")

    data = load(summary)
    if not data:
        sys.exit("summary.csv has no rows")

    bad = [(s, p) for s, ps in data.items() for p in ps if p["errors"]]
    for server, point in bad:
        print(f"warning: {server} @ {point['connections']} conns had errors")

    plot_throughput(data, RESULTS / "throughput.png")
    plot_latency(data, RESULTS / "latency.png")


if __name__ == "__main__":
    main()

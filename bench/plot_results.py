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
RESULTS = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "results" / "latest"


def load(path):
    rows = defaultdict(list)
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            requested = int(row["connections"])
            established = int(row.get("established", requested) or requested)
            rows[row["server"]].append(
                {
                    "connections": requested,
                    "established": established,
                    "throughput": float(row["throughput_rps"]),
                    "p50": float(row["p50_us"]),
                    "p99": float(row["p99_us"]),
                    "errors": int(row["errors"]),
                    "connect_max": float(row.get("connect_max_us", 0) or 0),
                }
            )
    for server in rows:
        rows[server].sort(key=lambda r: r["connections"])
    return rows


def _mark_failures(ax, points, colour):
    """Ring every point where requests failed.

    Without this the charts flatter whichever server refuses load fastest: a
    server that accepts 134 of 1024 connections and serves those 134 quickly
    posts the best throughput and the flattest latency on the page.
    """
    bad = [p for p in points if p["errors"] > 0]
    if not bad:
        return
    ax.scatter(
        [p["connections"] for p in bad],
        [p["_y"] for p in bad],
        s=170,
        facecolors="none",
        edgecolors=colour,
        linewidths=2.0,
        zorder=5,
    )


def plot_throughput(data, out):
    fig, ax = plt.subplots(figsize=(9, 5.5))
    for server, points in sorted(data.items()):
        (line,) = ax.plot(
            [p["connections"] for p in points],
            [p["throughput"] for p in points],
            marker="o",
            label=server.replace("_server", ""),
        )
        for p in points:
            p["_y"] = p["throughput"]
        _mark_failures(ax, points, line.get_color())

    ax.set_xscale("log", base=2)
    ax.set_xlabel("concurrent connections")
    ax.set_ylabel("throughput (requests/s)")
    ax.set_title("Throughput against concurrency")
    ax.grid(True, which="both", alpha=0.3)
    ax.plot([], [], "o", mfc="none", mec="black", ms=13, label="had failed requests")
    ax.legend(fontsize=9)
    fig.tight_layout()
    fig.savefig(out, dpi=140)
    print(f"wrote {out}")


def plot_established(data, out):
    """How many of the requested connections the server actually served.

    The diagonal is the ideal. Anything below it is load the server refused,
    and refused load is why a throughput curve can look good while most clients
    got nothing.
    """
    fig, ax = plt.subplots(figsize=(9, 5.5))
    all_conns = sorted({p["connections"] for ps in data.values() for p in ps})
    ax.plot(all_conns, all_conns, color="black", linestyle=":", alpha=0.5,
            label="all connections served")
    for server, points in sorted(data.items()):
        ax.plot(
            [p["connections"] for p in points],
            [p["established"] for p in points],
            marker="o",
            label=server.replace("_server", ""),
        )
    ax.set_xscale("log", base=2)
    ax.set_yscale("log", base=2)
    ax.set_xlabel("connections requested")
    ax.set_ylabel("connections established")
    ax.set_title("Connections served against connections offered")
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=9)
    fig.tight_layout()
    fig.savefig(out, dpi=140)
    print(f"wrote {out}")


def plot_latency(data, out):
    fig, ax = plt.subplots(figsize=(9, 5.5))
    for server, points in sorted(data.items()):
        label = server.replace("_server", "")
        (line,) = ax.plot(
            [p["connections"] for p in points],
            [p["p50"] for p in points],
            marker="o",
            label=f"{label} p50",
        )
        for p in points:
            p["_y"] = p["p50"]
        _mark_failures(ax, points, line.get_color())
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
    plot_established(data, RESULTS / "established.png")


if __name__ == "__main__":
    main()

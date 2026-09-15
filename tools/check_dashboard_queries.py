#!/usr/bin/env python3
"""Extract every PromQL expression from the Grafana dashboard and emit a
Prometheus rule file so promtool can syntax check them.

Grafana template variables are substituted with a wildcard before checking,
since promtool has no knowledge of them.

    tools/check_dashboard_queries.py deploy/grafana/dashboards/*.json -o /tmp/dq.yml
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

VARIABLE = re.compile(r"\$(\w+)|\$\{(\w+)[^}]*\}")


def substitute_variables(expr: str) -> str:
    return VARIABLE.sub(".*", expr)


def extract(path: Path) -> list[dict[str, str]]:
    dashboard = json.loads(path.read_text(encoding="utf-8"))
    uid = dashboard.get("uid", path.stem)
    rules = []

    for panel in dashboard.get("panels", []):
        for target in panel.get("targets", []):
            expr = target.get("expr")
            if not expr:
                continue
            rules.append(
                {
                    "record": "check:{}:{}{}".format(uid.replace("-", "_"), panel["id"], target["refId"]),
                    "expr": substitute_variables(expr),
                }
            )

    if not rules:
        raise SystemExit(f"{path}: no PromQL targets found")

    return rules


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dashboards", nargs="+", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    args = parser.parse_args()

    rules: list[dict[str, str]] = []
    for dashboard in args.dashboards:
        rules.extend(extract(dashboard))

    lines = ["groups:", "  - name: dashboard-queries", "    rules:"]
    for rule in rules:
        lines.append(f"      - record: {rule['record']}")
        lines.append(f"        expr: {json.dumps(rule['expr'])}")
    lines.append("")

    args.output.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {len(rules)} queries to {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

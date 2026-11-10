#!/usr/bin/env python3
"""Per-thread CPU bottlenecks from an Instruments "CPU Counters" trace.

Record the browser's renderer (it hosts the CPU worker and the guest
cores) while a game runs, export the per-thread metric table, and run:

    xcrun xctrace record --template "CPU Counters" --attach <renderer pid> \
        --time-limit 8s --output run.trace
    xcrun xctrace export --input run.trace --xpath \
        '/trace-toc/run[@number="1"]/data/table[@schema="MetricAggregationForThread"]' > run.xml
    tools/profiling/topdown.py run.xml

Prints, for the busiest threads, the share of their cycles that were
useful (retiring), lost to instruction delivery (the front end: i-cache,
iTLB, fetch), to processing (the back end: execution and memory) and
discarded (mis-speculation). The renderer's pid: the busiest
"Chrome ... Helper (Renderer)" process while the game runs.
"""
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict

TOP_THREADS = 8
CATEGORIES = ("useful", "delivery", "processing", "discarded")


def main(path):
    ids = {}
    cycles = defaultdict(float)
    fractions = defaultdict(dict)
    for _, row in ET.iterparse(path, events=("end",)):
        if row.tag != "row":
            continue
        values = {}
        for cell in row:
            if "ref" in cell.attrib:
                value = ids.get(cell.attrib["ref"])
            else:
                for inner in cell.iter():
                    if "id" in inner.attrib and inner is not cell:
                        ids[inner.attrib["id"]] = inner.attrib.get("fmt", inner.text)
                if cell.tag == "thread":
                    value = cell.attrib.get("fmt", "")
                elif cell.tag in ("uint64", "fixed-decimal", "start-time"):
                    value = float(cell.text or 0)
                else:
                    value = cell.text or cell.attrib.get("fmt", "")
                if "id" in cell.attrib:
                    ids[cell.attrib["id"]] = value
            values.setdefault(cell.tag, value)
        key = (values.get("start-time"), values.get("thread"))
        metric = values.get("string")
        if metric == "cycle":
            cycles[key] += values.get("uint64") or 0
        else:
            fractions[key][metric] = values.get("fixed-decimal") or 0.0
        row.clear()
    weighted = defaultdict(lambda: defaultdict(float))
    total = defaultdict(float)
    for key, count in cycles.items():
        thread = key[1]
        total[thread] += count
        for metric, fraction in fractions.get(key, {}).items():
            weighted[thread][metric] += fraction * count
    for thread in sorted(total, key=lambda t: -total[t])[:TOP_THREADS]:
        shares = [weighted[thread][c] for c in CATEGORIES]
        whole = sum(shares) or 1.0
        print("%8.2f Gcycles  " % (total[thread] / 1e9) +
              "  ".join("%s %3.0f%%" % (c, 100 * s / whole) for c, s in zip(CATEGORIES, shares)) +
              "  " + str(thread)[:60])


if __name__ == "__main__":
    main(sys.argv[1])

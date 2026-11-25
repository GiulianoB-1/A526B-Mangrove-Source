#!/usr/bin/env python3
"""Offline rate calculation from unfiltered timestamped dmesg, no device writes."""
import argparse
import re
from pathlib import Path
p = argparse.ArgumentParser()
p.add_argument("capture", type=Path)
p.add_argument("--start", type=float, required=True)
p.add_argument("--end", type=float, required=True)
a = p.parse_args()
if a.end <= a.start:
    p.error("end must be after start")
rx = re.compile(r"^\s*\[\s*(\d+(?:\.\d+)?)\]")
times = []
for line in a.capture.read_text(errors="replace").splitlines():
    m = rx.match(line)
    if m:
        times.append(float(m.group(1)))
if not times or min(times) > a.start or max(times) < a.end:
    p.error("capture does not bracket requested interval; cannot measure")
n = sum(a.start <= t < a.end for t in times)
print(f"window={a.start:g}..{a.end:g}s entries={n} rate={n/(a.end-a.start):.3f} timestamped_lines/s")
print("Valid only for complete, unfiltered capture without dropped/overwritten records.")

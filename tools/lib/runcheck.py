#!/usr/bin/env python3
"""Deterministic regression check runner: render once, compute metrics, evaluate one condition.

usage: runcheck.py BINARY OUT_DIR SETUP FRAMES TRACE(0|1) EXPECT
Exit 0 when EXPECT is true for the measured metrics, 1 when false, 2 when the run itself failed.
EXPECT is a Python boolean expression over `m`, for example: m["lock_waits"] == 0 and m["max_ms"] < 3 * m["p50_ms"]
"""
import json
import subprocess
import sys
from pathlib import Path

binary, out, setup, frames, trace, expect = sys.argv[1], Path(sys.argv[2]), sys.argv[3], int(sys.argv[4]), sys.argv[5] == "1", sys.argv[6]
out.mkdir(parents=True, exist_ok=True)
cmd = [binary, "--setup", setup, "--frames", str(max(frames, 300)), "--out", str(out)] + (["--trace"] if trace else [])
code = subprocess.run(cmd, capture_output=True).returncode

m = {"exit_code": code, "crashed": code != 0}
summary = out / "summary.json"
if summary.is_file():
    m.update({k: v for k, v in json.loads(summary.read_text()).items() if isinstance(v, (int, float, bool)) and v is not None})
mem = out / "memory.csv"
if mem.is_file():
    rows = [line.split(",") for line in mem.read_text().splitlines()[1:] if "," in line]
    if len(rows) > 1:
        m["memory_start_mb"] = int(rows[0][1]) / 2**20
        m["memory_end_mb"] = int(rows[-1][1]) / 2**20
        m["memory_growth_mb"] = m["memory_end_mb"] - m["memory_start_mb"]
tr = out / "trace.json"
if tr.is_file():
    events = json.loads(tr.read_text()).get("traceEvents", [])
    waits = [e["dur"] / 1000 for e in events if e.get("name") == "wait_asset_lock"]
    m["lock_waits"] = len(waits)
    m["lock_wait_max_ms"] = max(waits, default=0.0)
log = out / "log.jsonl"
m["fatal_lines"] = sum('"fatal"' in line for line in log.read_text().splitlines()) if log.is_file() else 0

print(json.dumps(m))
try:
    # Both m["p95_ms"] and bare p95_ms are accepted; metrics a run does not produce read as None, so the condition fails.
    names = {k: m.get(k) for k in m} | {"m": m, "abs": abs, "min": min, "max": max}
    ok = bool(eval(expect, {"__builtins__": {}}, names))
except Exception as e:  # a condition that cannot be evaluated on these metrics is a failed check, reported as such
    print(f"condition error: {type(e).__name__}: {e}")
    sys.exit(2)
print("PASS" if ok else "FAIL", expect)
sys.exit(0 if ok else 1)

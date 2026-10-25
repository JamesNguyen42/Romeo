"""Always-on telemetry monitor: runs the current release on a rotating rig, stores the telemetry in the local log store,
evaluates fixed alert rules (no model) and opens a case when a rule trips. Runs only while no case job is rendering,
so case measurements stay clean. At most one open alert case per (rig, rule)."""
import json
import os
import subprocess
import time
import urllib.request
from pathlib import Path

API = os.environ.get("ROMEO_API", "http://127.0.0.1:8790")
RELEASE = os.environ.get("ROMEO_CHAT_REV", "release/current")
TOOLS = Path(os.environ.get("ROMEO_TOOLS", "/opt/hack/Romeo/tools"))
STORE = Path(os.environ.get("TELEMETRY_STORE", "/opt/hack/telemetry"))
RIGS = os.environ.get("TELEMETRY_RIGS", "B,D,E,I").split(",")
PERIOD_S = int(os.environ.get("TELEMETRY_PERIOD_S", "180"))
RULES = [  # (name, description, predicate over the run's metrics)
    ("crash", "the renderer exited with an error", lambda m: m["exit_code"] != 0),
    ("frame_spikes", "p99 frame time above 2x the median", lambda m: m.get("p99_ms", 0) > 2 * m.get("p50_ms", 1e9)),
    ("memory_growth", "resident memory grew more than 100 MiB in one run", lambda m: m.get("memory_growth_mb", 0) > 100),
    ("motion_vectors", "motion-vector error above 0.5 px", lambda m: (m.get("mv_error_px") or 0) > 0.5),
]
RIG_NAMES = {"B": "4-camera rig", "D": "8-camera 1440p rig", "E": "12-camera rig", "I": "4-camera rig with pedestrians"}


def api(method, path, body=None):
    req = urllib.request.Request(API + path, method=method, data=json.dumps(body).encode() if body else None, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read() or b"{}")


def renderer_busy():
    try:
        return any("romeo-render" in p["cmd"] for p in api("GET", "/api/system").get("processes", []))
    except Exception:
        return True


def open_alerts():
    return {c["title"] for c in api("GET", "/api/cases") if c["status"] not in ("done", "stopped")}


def sample(rig):
    binary = subprocess.run([str(TOOLS / "build_rev"), RELEASE], capture_output=True, text=True).stdout.strip().splitlines()[-1]
    out = STORE / f"{time.strftime('%Y%m%d-%H%M%S')}-rig-{rig}"
    out.mkdir(parents=True, exist_ok=True)
    code = subprocess.run([binary, "--setup", rig, "--frames", "600", "--out", str(out)], capture_output=True).returncode
    m = {"exit_code": code}
    if (out / "summary.json").is_file():
        m.update(json.loads((out / "summary.json").read_text()))
    rows = [l.split(",") for l in (out / "memory.csv").read_text().splitlines()[1:]] if (out / "memory.csv").is_file() else []
    if len(rows) > 1:
        m["memory_growth_mb"] = (int(rows[-1][1]) - int(rows[0][1])) / 2**20
    (out / "metrics.json").write_text(json.dumps(m))
    return out, m


def main():
    i = 0
    while True:
        time.sleep(PERIOD_S)
        if renderer_busy():
            continue
        rig = RIGS[i % len(RIGS)]
        i += 1
        try:
            out, m = sample(rig)
            tripped = [(n, d) for n, d, pred in RULES if pred(m)]
            print(json.dumps({"rig": rig, "run": out.name, "tripped": [n for n, _ in tripped]}), flush=True)
            if not tripped:
                continue
            title = f"Alert: {', '.join(n.replace('_', ' ') for n, _ in tripped)} on rig {rig}"
            if title in open_alerts():
                continue
            numbers = ", ".join(f"{k} {m[k]:.1f}" for k in ("p50_ms", "p99_ms", "max_ms", "memory_growth_mb", "mv_error_px") if isinstance(m.get(k), (int, float)))
            report = (f"Production telemetry alert from the always-on monitor. Run {out.name} of the current release on the "
                      f"{RIG_NAMES.get(rig, 'rig ' + rig)} (setup {rig}) tripped: {'; '.join(d for _, d in tripped)}. "
                      f"Measured: {numbers or 'no summary (exit code ' + str(m['exit_code']) + ')'}.")
            api("POST", "/api/cases", {"title": title, "setup": rig, "report": report, "channel": "monitor", "user": "telemetry"})
        except Exception as e:
            print("monitor sample failed:", type(e).__name__, str(e)[:200], flush=True)


if __name__ == "__main__":
    main()

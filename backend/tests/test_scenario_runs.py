#!/usr/bin/env python3
"""HTTP/WAL regression tests using an explicitly synthetic renderer process.

Run on Linux after building the service:
  ROMEO_TEST_BINARY=/path/to/romeo python3 backend/tests/test_scenario_runs.py
These fixtures exercise orchestration and failure handling, not renderer performance.
"""

import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen
import zlib


BINARY = os.environ.get("ROMEO_TEST_BINARY")
METRICS = dict(p50_ms=12, p95_ms=16, p99_ms=22, max_ms=30, total_ms=600,
               spike_frames=1, views=1, frames=50, resident_end_bytes=1048576,
               tmp_bytes=0, cache_entries=2)

RENDERER = r'''#!/usr/bin/env python3
import json, os, pathlib, sys, time
args = sys.argv[1:]
out = pathlib.Path(args[args.index('--out') + 1])
out.mkdir(parents=True, exist_ok=True)
lock = out.parent / 'renderer.lock'
try:
    fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
except FileExistsError:
    sys.exit(97)
try:
    (out / 'args.json').write_text(json.dumps(args))
    time.sleep(float(os.environ.get('TEST_RENDER_DELAY', '.05')))
    bug = args[args.index('--bug') + 1] if '--bug' in args else ''
    if bug == 'exit_failure':
        sys.exit(7)
    metrics = METRICS
    (out / 'summary.json').write_text(json.dumps({} if bug == 'bad_summary' else metrics))
    (out / 'frames.csv').write_text('frame,ms\n0,12\n1,22\n')
    (out / 'memory.csv').write_text('frame,resident_bytes\n0,1048576\n')
    (out / 'image.png').write_bytes(b'test-only-not-an-image')
    seed = int(args[args.index('--view-seed') + 1]) if '--view-seed' in args else 0
    (out / 'preview.json').write_text(json.dumps({'view_seed':seed,'frames':[]}))
finally:
    os.close(fd)
    lock.unlink(missing_ok=True)
'''.replace("metrics = METRICS", "metrics = " + repr(METRICS))


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


@unittest.skipUnless(BINARY, "set ROMEO_TEST_BINARY to the built Linux service")
class ScenarioRuns(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="romeo-api-test-")
        self.root = Path(self.tmp.name)
        self.data = self.root / "data"
        self.data.mkdir()
        self.scenarios = self.root / "cases.json"
        self.defs = [dict(id="test-scene", title="Test renderer scenario", report="Measure a seeded view.",
                         setup="A", bugs="loader_lock", rev="missing/revision", rev_good="missing/baseline",
                         withhold=["capture_trace"]),
                     dict(id="test-failure", title="Failing run", report="A process fails.", setup="A", bugs="exit_failure"),
                     dict(id="test-malformed", title="Malformed output", report="Missing measurements.", setup="A", bugs="bad_summary")]
        self.scenarios.write_text(json.dumps(self.defs))
        self.renderer = self.root / "renderer"
        self.renderer.write_text(RENDERER)
        self.renderer.chmod(0o755)
        self.tools = self.root / "tools"
        self.tools.mkdir()
        self.port = free_port()
        self.base = f"http://127.0.0.1:{self.port}"
        self.proc = None
        self.log = (self.root / "server.log").open("ab")
        self.env = dict(os.environ, ROMEO_PORT=str(self.port), ROMEO_DATA=str(self.data),
                        ROMEO_CASES=str(self.scenarios), ROMEO_RENDER=str(self.renderer),
                        ROMEO_TOOLS=str(self.tools), ROMEO_UI=str(self.root), ROMEO_DASHBOARD=str(self.root),
                        ROMEO_REPO_DIR=str(self.root), ROMEO_FRAMES="50", ROMEO_THINK="0",
                        CLEF_URL="http://127.0.0.1:1/absent", ORNITH_URL="http://127.0.0.1:1/absent",
                        ORNITH_METRICS="http://127.0.0.1:1/absent")

    def tearDown(self):
        self.stop()
        self.log.close()
        # A detached test renderer can briefly outlive a stopped service.
        deadline = time.monotonic() + 4
        while (self.data / "jobs" / "renderer.lock").exists() and time.monotonic() < deadline:
            time.sleep(.05)
        self.tmp.cleanup()

    def request(self, path, body=None):
        req = Request(self.base + path, data=None if body is None else json.dumps(body).encode(),
                      headers={"Content-Type": "application/json"})
        with urlopen(req, timeout=5) as response:
            return json.load(response)

    def start(self):
        self.proc = subprocess.Popen([str(Path(BINARY).resolve())], env=self.env, cwd=self.root,
                                     stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                self.fail((self.root / "server.log").read_text())
            try:
                self.request("/api/scenarios")
                return
            except OSError:
                time.sleep(.04)
        self.fail("service did not start")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=4)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.proc = None

    def run_scenario(self, scenario="test-scene", **body):
        return self.request(f"/api/scenarios/{scenario}/runs", dict(mode="comparison", **body))

    def wait_case(self, case_id, predicate=lambda case: case["status"] in ("done", "stopped")):
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            case = self.request("/api/cases/" + case_id)
            if predicate(case):
                return case
            time.sleep(.05)
        self.fail("case did not reach expected state: " + json.dumps(case))

    def write_wal(self, events):
        with (self.data / "wal.log").open("wb") as file:
            for lsn, event in enumerate(events, 1):
                payload = json.dumps(event).encode()
                file.write(struct.pack("<IIIQ", 0x4C415752, len(payload), zlib.crc32(payload), lsn))
                file.write(payload)

    def test_real_process_comparisons_are_unique_serial_and_seeded(self):
        self.start()
        catalog = self.request("/api/scenarios")
        self.assertEqual(catalog["capacity"], {"used": 0, "max": 64})
        self.assertNotIn("bugs", catalog["scenarios"][0])
        first = self.run_scenario(view_seed=42)
        second = self.run_scenario(view_seed=43)
        self.assertNotEqual(first["id"], second["id"])
        for run, seed in ((first, 42), (second, 43)):
            case = self.wait_case(run["id"])
            self.assertEqual(case["status"], "done")
            self.assertEqual(case["scenario_id"], "test-scene")
            self.assertFalse(any(e["type"] in ("decision", "thought", "conclusion") for e in case["events"]))
            results = [e["data"] for e in case["events"] if e["type"] == "action_result"]
            self.assertEqual(len(results), 2)
            self.assertTrue(all(r["exit_code"] == 0 and r["metrics"] == METRICS for r in results))
            self.assertEqual(case["events"][-1]["type"], "comparison_complete")
            for index, result in enumerate(results):
                args = json.loads((self.data / "jobs" / result["action_id"] / "args.json").read_text())
                self.assertEqual(args[args.index("--view-seed") + 1], str(seed))
                self.assertEqual(args[args.index("--preview-frames") + 1], "40")
                self.assertEqual("--bug" in args, index == 0)
                if index == 0:
                    self.assertEqual(args[args.index("--bug") + 1], "loader_lock")
                self.assertIn(result["action_id"] + "/preview.json", result["artifacts"])
                manifest = self.request(f'/api/artifacts/{run["id"]}/{result["action_id"]}/preview.json')
                self.assertEqual(manifest["view_seed"], seed)

    def test_recovery_preserves_execution_metadata_when_scenario_changes(self):
        self.env["TEST_RENDER_DELAY"] = ".7"
        self.start()
        run = self.run_scenario(view_seed=123)
        self.wait_case(run["id"], lambda c: any(e["type"] == "action_requested" for e in c["events"]))
        self.stop()
        self.scenarios.write_text("[]")
        self.start()
        case = self.wait_case(run["id"])
        self.assertEqual(case["status"], "done")
        self.assertEqual((case["scenario_id"], case["mode"], case["view_seed"]), ("test-scene", "comparison", 123))
        execution = case["events"][0]["data"]["execution"]
        self.assertEqual(execution["bugs"], "loader_lock")
        self.assertEqual(execution["rev"], "missing/revision")
        self.assertEqual(execution["rev_good"], "missing/baseline")
        self.assertEqual(execution["withhold"], ["capture_trace"])
        self.assertEqual(len([e for e in case["events"] if e["type"] == "action_requested"]), 2)

    def test_queued_comparison_keeps_preview_seed_after_restart(self):
        self.env["TEST_RENDER_DELAY"] = "1"
        self.start()
        first = self.run_scenario(view_seed=101)
        second = self.run_scenario(view_seed=202)
        queued = self.wait_case(second["id"], lambda c: any(e["type"] == "action_requested" for e in c["events"]))
        request = next(e["data"] for e in queued["events"] if e["type"] == "action_requested")
        self.assertFalse((self.data / "outbox" / (request["action_id"] + ".json")).exists())
        self.stop()
        self.start()
        self.assertEqual(self.wait_case(first["id"])["status"], "done")
        case = self.wait_case(second["id"])
        self.assertEqual(case["status"], "done")
        results = [e["data"] for e in case["events"] if e["type"] == "action_result"]
        self.assertEqual(len(results), 2)
        for result in results:
            args = json.loads((self.data / "jobs" / result["action_id"] / "args.json").read_text())
            self.assertEqual(args[args.index("--view-seed") + 1], "202")
            self.assertEqual(args[args.index("--preview-frames") + 1], "40")

    def test_legacy_revision_case_keeps_delivery_pipeline_after_restart(self):
        # These are inert local tool fixtures: no source edit, network request, or pull request.
        catalog = [{"name": name, "criteria": "Test fixture", "label": name}
                   for name in ("write_test", "write_fix", "open_pr")]
        (self.tools / "catalog.json").write_text(json.dumps(catalog))
        for name, data in (("write_fix", {"fix_rev": "fix/test-fixture"}), ("open_pr", {})):
            script = self.tools / name
            script.write_text("#!/usr/bin/python3\nimport json, pathlib, sys\n"
                              "out = pathlib.Path(sys.argv[1])\n"
                              + "(out / 'result.json').write_text(json.dumps(" + repr({"summary": name + " local fixture", "data": data}) + "))\n")
            script.chmod(0o755)
        build = self.tools / "build_rev"
        build.write_text("#!/usr/bin/python3\nimport pathlib, sys\n"
                         + "pathlib.Path(" + repr(str(self.root / "built-revision")) + ").write_text(sys.argv[1])\n"
                         + "print(" + repr(str(self.renderer)) + ")\n")
        build.chmod(0o755)
        job = self.data / "jobs" / "delivery-test"
        job.mkdir(parents=True)
        (job / "exit").write_text("0")
        (job / "result.json").write_text(json.dumps({"summary": "Regression fixture completed"}))
        (self.data / "outbox").mkdir()
        (self.data / "outbox" / "delivery-test.json").write_text("{}")
        events = [
            {"type": "case_opened", "data": {"title": "Legacy revision case", "report": "Fixture", "setup": "A", "rev": "release/current", "rev_good": "main"}},
            {"type": "conclusion", "data": {"diagnosis": "Fixture cause", "fix": "Fixture fix", "supported": True}},
            {"type": "action_requested", "data": {"action_id": "delivery-test", "kind": "write_test", "label": "Regression check", "job": True, "runner": "script", "dir": str(job)}},
        ]
        self.write_wal([dict(event, seq=i, case="delivery-case", t_ms=int(time.time() * 1000)) for i, event in enumerate(events, 1)])
        self.start()
        case = self.wait_case("delivery-case")
        self.assertEqual(case["status"], "done")
        requested = [e["data"] for e in case["events"] if e["type"] == "action_requested"]
        self.assertEqual([action["kind"] for action in requested], ["write_test", "write_fix", "open_pr", "verify_fix"])
        fix_input = json.loads((self.data / "jobs" / requested[1]["action_id"] / "input.json").read_text())
        self.assertEqual((fix_input["rev"], fix_input["rev_good"]), ("release/current", "main"))
        self.assertEqual((self.root / "built-revision").read_text(), "fix/test-fixture")
        done = next(e["data"] for e in case["events"] if e["type"] == "done")
        self.assertIn("fix/test-fixture", done["verified_with"])

    def test_failed_and_incomplete_runs_do_not_complete(self):
        self.start()
        for scenario in ("test-failure", "test-malformed"):
            case = self.wait_case(self.run_scenario(scenario)["id"])
            self.assertEqual(case["status"], "stopped")
            self.assertFalse(any(e["type"] in ("done", "comparison_complete") for e in case["events"]))

    def verification_fixture(self, exit_code, summary):
        job = self.data / "jobs" / "verify-job"
        job.mkdir(parents=True)
        (self.data / "outbox").mkdir()
        (self.data / "outbox" / "verify-job.json").write_text("{}")
        (job / "exit").write_text(str(exit_code))
        (job / "summary.json").write_text(json.dumps(summary))
        data = [{"type": "case_opened", "data": {"title": "Verification", "report": "Test", "setup": "A"}},
                {"type": "conclusion", "data": {"diagnosis": "Fixture", "fix": "Fixture"}},
                {"type": "action_requested", "data": {"action_id": "verify-job", "kind": "verify_fix", "label": "Verify", "job": True, "dir": str(job)}}]
        self.write_wal([dict(e, seq=i, case="verify-case", t_ms=int(time.time() * 1000)) for i, e in enumerate(data, 1)])

    def test_failed_verification_never_reports_done(self):
        self.verification_fixture(7, METRICS)
        self.start()
        case = self.wait_case("verify-case")
        self.assertEqual(case["status"], "stopped")
        self.assertFalse(any(e["type"] == "done" for e in case["events"]))

    def test_incomplete_verification_never_reports_done(self):
        self.verification_fixture(0, {})
        self.start()
        case = self.wait_case("verify-case")
        self.assertEqual(case["status"], "stopped")
        self.assertFalse(any(e["type"] == "done" for e in case["events"]))

    def test_invalid_parameters_and_capacity_preserve_existing_cases(self):
        events = []
        for index in range(64):
            events.extend([dict(seq=1, case=f"saved-{index}", t_ms=1, type="case_opened", data={"title": "Saved", "setup": "A"}),
                           dict(seq=2, case=f"saved-{index}", t_ms=2, type="stopped", data={"reason": "Complete fixture"})])
        self.write_wal(events)
        self.start()
        for body in ({"mode": "unknown"}, {"mode": 7}, {"view_seed": -1}, {"view_seed": 4294967296}):
            with self.assertRaises(HTTPError) as error:
                self.request("/api/scenarios/test-scene/runs", body)
            self.assertEqual(error.exception.code, 400)
        with self.assertRaises(HTTPError) as error:
            self.run_scenario()
        self.assertEqual(error.exception.code, 503)
        self.assertEqual(len(self.request("/api/cases")), 64)

    def test_named_system_event_uses_existing_stream(self):
        self.start()
        with urlopen(self.base + "/api/stream", timeout=4) as stream:
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                if stream.readline().decode().strip() == "event: system":
                    snapshot = json.loads(stream.readline().decode().removeprefix("data: "))
                    self.assertIn("gpu_available", snapshot)
                    self.assertIn("cases", snapshot)
                    return
        self.fail("no system snapshot received")

    def test_unavailable_model_stops_investigation_without_retrying(self):
        self.start()
        with self.assertRaises(HTTPError) as error:
            self.request("/api/scenarios/test-scene/runs", {"mode": "investigation", "view_seed": 42})
        self.assertEqual(error.exception.code, 400)
        run = self.request("/api/scenarios/test-scene/runs", {"mode": "investigation"})
        self.assertEqual(run["view_seed"], 0)
        case = self.wait_case(run["id"])
        self.assertEqual(case["status"], "stopped")
        self.assertIn("Clef", case["events"][-1]["data"]["reason"])
        self.assertFalse(any(e["type"] in ("decision", "action_requested") for e in case["events"]))
        time.sleep(.7)
        self.assertEqual(len(self.request("/api/cases/" + run["id"])["events"]), len(case["events"]))

    def test_stale_gpu_sample_is_not_reported_as_live(self):
        fake_smi = self.root / "nvidia-smi"
        fake_smi.write_text("#!/usr/bin/python3\nfrom pathlib import Path\nimport time\n"
                            "marker = Path(__file__).with_suffix('.sampled')\n"
                            "if marker.exists(): time.sleep(10)\n"
                            "else:\n marker.touch()\n print('42, 90, 60, 1500')\n")
        fake_smi.chmod(0o755)
        self.env["PATH"] = str(self.root) + os.pathsep + self.env["PATH"]
        self.start()
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            first = self.request("/api/system")
            if first["gpu_available"]:
                break
            time.sleep(.05)
        self.assertTrue(first["gpu_available"])
        self.assertEqual(first["gpu_util_pct"], 42)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            stale = self.request("/api/system")
            if stale["t_ms"] - first["gpu_sample_ms"] > 5000:
                break
            time.sleep(.1)
        self.assertGreater(stale["t_ms"] - first["gpu_sample_ms"], 5000)
        self.assertFalse(stale["gpu_available"])
        self.assertEqual(stale["gpu_sample_ms"], first["gpu_sample_ms"])

    def test_malformed_model_response_does_not_create_fake_probabilities(self):
        class EmptyDecision(BaseHTTPRequestHandler):
            def do_POST(self):
                self.rfile.read(int(self.headers.get("Content-Length", 0)))
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", "2")
                self.end_headers()
                self.wfile.write(b"{}")

            def log_message(self, *_):
                pass

        with ThreadingHTTPServer(("127.0.0.1", 0), EmptyDecision) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                self.env["CLEF_URL"] = f"http://127.0.0.1:{server.server_port}/decision"
                self.start()
                run = self.request("/api/scenarios/test-scene/runs", {"mode": "investigation"})
                case = self.wait_case(run["id"])
                self.assertEqual(case["status"], "stopped")
                self.assertFalse(any(e["type"] == "decision" for e in case["events"]))
                self.assertIn("invalid decision", case["events"][-1]["data"]["reason"])
            finally:
                server.shutdown()
                thread.join(timeout=2)


if __name__ == "__main__":
    unittest.main(verbosity=2)

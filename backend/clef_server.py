"""Clef-flash decision server: POST /v1/systemone, GET /health.

One process owns the model and answers one request at a time, so GPU memory stays at one model plus one
batch. Requests above MAX_BODY bytes are refused with 413; malformed requests get 400; nothing is retried.
"""
import json
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

MODEL_DIR = os.environ.get("CLEF_DIR", "/models/clef")
PORT = int(os.environ.get("CLEF_PORT", "8001"))
MAX_BODY = 256 * 1024

sys.path.insert(0, MODEL_DIR)
import torch  # noqa: E402
from joint_schema_model import load_release_model, systemone  # noqa: E402

started = time.perf_counter()
model, processor = load_release_model(MODEL_DIR, device="cuda")
load_ms = round((time.perf_counter() - started) * 1000)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, body):
        raw = json.dumps(body, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        if self.path == "/health":
            self.reply(200, {"status": "ready", "model": "clef-flash", "load_ms": load_ms,
                             "gpu_bytes": torch.cuda.memory_allocated()})
        else:
            self.reply(404, {"error": "not_found"})

    def do_POST(self):
        if self.path != "/v1/systemone":
            self.reply(404, {"error": "not_found"})
            return
        length = int(self.headers.get("Content-Length") or 0)
        if length < 1 or length > MAX_BODY:
            self.reply(413, {"error": "body_size", "limit": MAX_BODY})
            return
        try:
            request = json.loads(self.rfile.read(length))
            if not isinstance(request.get("questions"), dict) or not request["questions"]:
                raise ValueError("questions must be a non-empty object")
            started = time.perf_counter()
            with torch.inference_mode():
                response = systemone(model, processor, request)
            response["timings"] = {"inference_ms": round((time.perf_counter() - started) * 1000, 2)}
            self.reply(200, response)
        except (ValueError, KeyError, TypeError) as error:
            self.reply(400, {"error": str(error)[:300]})
        except Exception as error:  # model or CUDA failure: report it, never retry
            self.reply(500, {"error": type(error).__name__, "message": str(error)[:300]})


if __name__ == "__main__":
    print(json.dumps({"event": "ready", "port": PORT, "load_ms": load_ms}), flush=True)
    HTTPServer(("0.0.0.0", PORT), Handler).serve_forever()

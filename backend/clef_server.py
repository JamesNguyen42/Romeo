"""Clef-flash decision server (FastAPI): POST /v1/systemone, GET /health.

Threading model:
- Request handlers validate the body and encode it on a CPU thread pool (tokenization runs in parallel; on a
  free-threaded Python build it is truly parallel).
- One GPU thread owns the model. It takes every encoded request that is waiting (up to MAX_BATCH, or whatever
  arrives within BATCH_WINDOW_MS), scores them in one batched forward pass, and resolves each request.
Limits: bodies over MAX_BODY bytes get 413, malformed requests 400, model failures 500. Nothing is retried.
"""
import asyncio
import os
import queue
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import JSONResponse

MODEL_DIR = os.environ.get("CLEF_DIR", "/models/clef")
MAX_BODY = 256 * 1024
MAX_BATCH = int(os.environ.get("CLEF_MAX_BATCH", "8"))
BATCH_WINDOW_MS = float(os.environ.get("CLEF_BATCH_WINDOW_MS", "4"))
ENCODE_THREADS = int(os.environ.get("CLEF_ENCODE_THREADS", "8"))

sys.path.insert(0, MODEL_DIR)
import torch  # noqa: E402
from joint_schema_model import QUESTION_TYPES, collate_records, encode_record, load_release_model, systemone_answer  # noqa: E402

started = time.perf_counter()
model, processor = load_release_model(MODEL_DIR, device="cuda")
LOAD_MS = round((time.perf_counter() - started) * 1000)
DEVICE = next(model.parameters()).device
PAD_ID = processor.tokenizer.pad_token_id

encode_pool = ThreadPoolExecutor(max_workers=ENCODE_THREADS, thread_name_prefix="clef-encode")
work: "queue.Queue[tuple]" = queue.Queue(maxsize=256)
stats = {"requests": 0, "batches": 0, "max_batch": 0, "gpu_ms": 0.0}


def validate(request: dict) -> None:
    questions = request.get("questions")
    if not isinstance(request.get("model"), str) or "state" not in request:
        raise ValueError("model and state are required")
    if not isinstance(questions, dict) or not questions:
        raise ValueError("at least one question is required")
    for qid, q in questions.items():
        if q.get("type") not in QUESTION_TYPES:
            raise ValueError(f"{qid}: type must be noul, choice, or score")
        if q["type"] != "noul" and not q.get("criteria"):
            raise ValueError(f"{qid}: criteria must not be empty")


def encode(request: dict):
    validate(request)
    return encode_record(processor.tokenizer, request, max_length=16384, processor=processor)


def gpu_loop() -> None:
    """Owns the model. Batches whatever is waiting and scores it in one forward pass."""
    while True:
        batch = [work.get()]
        deadline = time.perf_counter() + BATCH_WINDOW_MS / 1000.0
        while len(batch) < MAX_BATCH:
            remaining = deadline - time.perf_counter()
            if remaining <= 0:
                break
            try:
                batch.append(work.get(timeout=remaining))
            except queue.Empty:
                break
        t0 = time.perf_counter()
        try:
            with torch.inference_mode():
                logits = model(collate_records([item[1] for item in batch], PAD_ID, DEVICE))
            gpu_ms = (time.perf_counter() - t0) * 1000.0
            for (request, encoded, loop, future), record_logits in zip(batch, logits):
                answers = {
                    q.question_id: systemone_answer(request["questions"][q.question_id], dict(zip(q.option_ids, ql.float().softmax(-1).tolist())))
                    for q, ql in zip(encoded.questions, record_logits)
                }
                body = {"model": request["model"], "answers": answers,
                        "usage": {"input_tokens": len(encoded.input_ids), "output_tokens": 0},
                        "timings": {"inference_ms": round(gpu_ms, 2), "batch_size": len(batch)}}
                loop.call_soon_threadsafe(future.set_result, body)
            stats["batches"] += 1
            stats["requests"] += len(batch)
            stats["max_batch"] = max(stats["max_batch"], len(batch))
            stats["gpu_ms"] += gpu_ms
        except Exception as error:  # model or CUDA failure: report it to every waiting request, never retry
            for _, _, loop, future in batch:
                loop.call_soon_threadsafe(future.set_exception, RuntimeError(f"{type(error).__name__}: {str(error)[:300]}"))


threading.Thread(target=gpu_loop, name="clef-gpu", daemon=True).start()
app = FastAPI(title="clef-flash")


@app.get("/health")
def health():
    free_threaded = hasattr(sys, "_is_gil_enabled") and not sys._is_gil_enabled()
    return {"status": "ready", "model": "clef-flash", "load_ms": LOAD_MS, "gpu_bytes": torch.cuda.memory_allocated(),
            "python": sys.version.split()[0], "free_threaded": free_threaded, **stats}


@app.post("/v1/systemone")
async def systemone(raw: Request):
    body = await raw.body()
    if not body or len(body) > MAX_BODY:
        raise HTTPException(status_code=413, detail=f"body must be 1..{MAX_BODY} bytes")
    try:
        import json
        request = json.loads(body)
        loop = asyncio.get_running_loop()
        encoded = await loop.run_in_executor(encode_pool, encode, request)
    except (ValueError, KeyError, TypeError) as error:
        return JSONResponse(status_code=400, content={"error": str(error)[:300]})
    future = loop.create_future()
    try:
        work.put_nowait((request, encoded, loop, future))
    except queue.Full:
        return JSONResponse(status_code=503, content={"error": "decision queue full"})
    try:
        return await future
    except RuntimeError as error:
        return JSONResponse(status_code=500, content={"error": str(error)})


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=int(os.environ.get("CLEF_PORT", "8001")), log_level="warning")

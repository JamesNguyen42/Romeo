"""Matched LLM benchmark: prompts of about 256, 1k, 4k and 8k input tokens, 256 output tokens, temperature 0,
thinking off, one request at a time. Streams to measure time to first token; decode rate = output tokens /
(total - TTFT). usage: bench_llm.py URL MODEL LABEL OUT.json"""
import json
import sys
import time
import urllib.request

url, model, label, out = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
LOG = ("frame 1042 render 11.2 ms wait_asset_lock 0.0 ms; frame 1043 render 11.4 ms; asset loader read 64 MiB in 72 ms holding asset_lock; "
       "frame 1044 render 83.1 ms wait_asset_lock 71.9 ms; worker tiles idle; ")


def run(target_tokens, max_out=256):
    context = (LOG * (target_tokens // 60 + 1))
    body = {"model": model, "temperature": 0, "max_tokens": max_out, "min_tokens": max_out, "stream": True,
            "stream_options": {"include_usage": True}, "chat_template_kwargs": {"enable_thinking": False},
            "messages": [{"role": "user", "content": f"Here is a render log:\n{context}\nExplain what causes the slow frames and how to fix it."}]}
    req = urllib.request.Request(url, data=json.dumps(body).encode(), headers={"Content-Type": "application/json"})
    t0 = time.perf_counter(); ttft = None; usage = {}
    with urllib.request.urlopen(req, timeout=600) as r:
        for raw in r:
            line = raw.decode().strip()
            if not line.startswith("data: ") or line == "data: [DONE]":
                continue
            chunk = json.loads(line[6:])
            if chunk.get("usage"):
                usage = chunk["usage"]
            if ttft is None and chunk.get("choices") and (chunk["choices"][0].get("delta") or {}).get("content"):
                ttft = time.perf_counter() - t0
    total = time.perf_counter() - t0
    n = usage.get("completion_tokens", 0)
    return {"input_tokens": usage.get("prompt_tokens"), "output_tokens": n, "ttft_s": round(ttft or 0, 3), "total_s": round(total, 3),
            "decode_tok_s": round(n / max(total - (ttft or 0), 1e-6), 1)}


results = {"model": label, "runs": []}
run(256)  # warm-up, not recorded
for target, max_out in ((256, 256), (1024, 256), (4096, 256), (8192, 256), (1024, 8192)):
    r = run(target, max_out)
    results["runs"].append(r)
    print(label, json.dumps(r), flush=True)
json.dump(results, open(out, "w"), indent=1)

#!/bin/sh
# Matched comparison: Ornith (now) vs Qwen3.8 Flash-Next hot-swapped into Ornith's slot. Clef keeps running.
# Waits for running agent tool work to finish, benchmarks Ornith, swaps, benchmarks Qwen, restores Ornith.
set -u
log() { echo "$(date +%H:%M:%S) $*"; }
B=/opt/hack/bench; mkdir -p $B
while pgrep -f "tools/write_fix|tools/write_test" >/dev/null; do sleep 10; done
sudo -n systemctl stop romeo-monitor
log "benchmark ornith"
python3 /opt/hack/Romeo/deploy/bench_llm.py http://127.0.0.1:8000/v1/chat/completions ornith-35b "Ornith 1.5 35B-A3B NVFP4" $B/ornith.json
sudo -n systemctl stop romeo-slack romeo
docker stop ornith >/dev/null
docker rm -f qwen38 >/dev/null 2>&1
log "starting qwen38 next to clef"
docker run -d --name qwen38 --gpus all --shm-size 8g --network host -e HF_HUB_OFFLINE=1 -e TRANSFORMERS_OFFLINE=1 \
  -v /opt/hack/models/qwen38-flash-next:/models/qwen38:ro vllm-b12x:c50aca /models/qwen38 \
  --served-model-name qwen38 --max-model-len 16384 --max-num-seqs 2 --gpu-memory-utilization "${QWEN_MEM:-0.82}" --enforce-eager \
  --host 127.0.0.1 --port 8010 >/dev/null
for i in $(seq 1 120); do
  curl -s -m 3 localhost:8010/v1/models | grep -q qwen38 && break
  [ "$(docker inspect -f {{.State.Running}} qwen38 2>/dev/null)" = true ] || { log "qwen38 exited"; docker logs --tail 60 qwen38 2>&1 | grep -iE "error|exception|unsupported|not supported|memory" | tail -10; break; }
  sleep 10
done
if curl -s -m 3 localhost:8010/v1/models | grep -q qwen38; then
  log "qwen38 ready"; docker logs qwen38 2>&1 | grep -E "Loading weights took|Model loading took|KV cache size|Maximum concurrency" | tail -4 | cut -c1-200
  python3 /opt/hack/Romeo/deploy/bench_llm.py http://127.0.0.1:8010/v1/chat/completions qwen38 "Qwen3.8 Flash-Next FP4" $B/qwen38.json
  curl -s -m 300 localhost:8010/v1/chat/completions -H "Content-Type: application/json" -d '{"model":"qwen38","messages":[{"role":"user","content":"A renderer hitches every ~3 s; a trace shows the render thread waiting 72 ms on asset_lock while the asset loader reads a 64 MiB file under that lock. In 3 sentences: cause and fix."}],"max_tokens":200,"temperature":0,"chat_template_kwargs":{"enable_thinking":false}}' | python3 -c "import json,sys;print('qwen sample:', json.load(sys.stdin)['choices'][0]['message']['content'][:600])"
fi
log "restoring ornith"
docker rm -f qwen38 >/dev/null 2>&1
docker start ornith >/dev/null
for i in $(seq 1 60); do curl -s -m 3 localhost:8000/v1/models | grep -q ornith && break; sleep 10; done
sudo -n systemctl start romeo romeo-slack romeo-monitor
log "restored: $(systemctl is-active romeo romeo-slack romeo-monitor | tr '\n' ' ')"

#!/bin/sh
# One-off comparison: Qwen3.8 Flash-Next in the same vLLM image, same benchmark as Ornith. Stops Ornith and Clef
# for the duration (the 102 GB weights do not fit next to them) and restores everything at the end.
set -u
log() { echo "$(date +%H:%M:%S) $*"; }
sudo -n systemctl stop romeo-monitor romeo-slack romeo
docker stop ornith clef >/dev/null
docker rm -f qwen38 >/dev/null 2>&1
log "starting qwen38"
docker run -d --name qwen38 --gpus all --shm-size 8g --network host -e HF_HUB_OFFLINE=1 -e TRANSFORMERS_OFFLINE=1 \
  -v /opt/hack/models/qwen38-flash-next:/models/qwen38:ro vllm-b12x:c50aca /models/qwen38 \
  --served-model-name qwen38 --max-model-len 32768 --max-num-seqs 4 --gpu-memory-utilization 0.92 --enforce-eager \
  --host 127.0.0.1 --port 8010 >/dev/null
for i in $(seq 1 90); do
  curl -s -m 3 localhost:8010/v1/models | grep -q qwen38 && break
  [ "$(docker inspect -f {{.State.Running}} qwen38)" = true ] || { log "qwen38 exited"; docker logs --tail 40 qwen38 2>&1 | grep -iE "error|exception|unsupported|not supported" | tail -8; break; }
  sleep 10
done
if curl -s -m 3 localhost:8010/v1/models | grep -q qwen38; then
  log "qwen38 ready"; docker logs qwen38 2>&1 | grep -E "Loading weights took|Model loading took|KV cache size" | tail -3
  for i in 1 2 3; do
    s=$(date +%s.%N)
    curl -s -m 300 localhost:8010/v1/chat/completions -H "Content-Type: application/json" -d '{"model":"qwen38","messages":[{"role":"user","content":"Explain step by step how a mutex held during a 64 MB file read can stall a render loop, and list three ways to fix it."}],"max_tokens":256,"min_tokens":256,"temperature":0,"chat_template_kwargs":{"enable_thinking":false}}' > /tmp/q.json
    e=$(date +%s.%N)
    python3 -c "import json;d=json.load(open('/tmp/q.json'));n=d['usage']['completion_tokens'];t=$e-$s;print(f'run $i: {n} tokens in {t:.2f} s = {n/t:.1f} tok/s (wall, includes prefill)')" 2>&1 | tail -1
  done
  python3 -c "import json;print('sample:', json.load(open('/tmp/q.json'))['choices'][0]['message']['content'][:700])"
fi
log "restoring ornith and clef"
docker rm -f qwen38 >/dev/null 2>&1
docker start ornith clef >/dev/null
for i in $(seq 1 60); do curl -s -m 3 localhost:8000/v1/models | grep -q ornith && curl -s -m 3 localhost:8001/health | grep -q ready && break; sleep 10; done
sudo -n systemctl start romeo romeo-slack romeo-monitor
log "restored: $(systemctl is-active romeo romeo-slack romeo-monitor | tr '\n' ' ')"

#!/bin/sh
# CUDA graphs are captured only for batch sizes 1, 2, 4 and 8 (max-num-seqs is 8): eager decode measured 12-13 tok/s
# against about 48 tok/s with graphs. Graphs crashed the engine under real case load on 2026-10-25,
# so the service runs ORNITH_EAGER=1 until that is understood. With the default capture list and several concurrent requests the b12x engine
# stalled and died once on the GB10; ORNITH_EAGER=1 restores eager mode if that recurs.
# MTP is off by default: vLLM rejects the b12x MoE backend for the unquantized MTP draft layer.
# Ornith 1.5 35B-A3B (NVFP4) on :8000. The memory share is explicit because the GB10 GPU uses system memory.
set -euf  # -f: the JSON capture list must not be glob-expanded
if [ -n "${ORNITH_EAGER:-}" ]; then MODE=--enforce-eager; else MODE='--compilation-config {"cudagraph_capture_sizes":[1,2,4,8]}'; fi
docker rm -f ornith >/dev/null 2>&1 || true
exec docker run -d --name ornith --restart unless-stopped --gpus all --shm-size 8g --network host \
  -e HF_HUB_OFFLINE=1 -e TRANSFORMERS_OFFLINE=1 -e VLLM_B12X_MOE_FP4_FORCE_A16=1 \
  -v /opt/hack/models/Ornith-1.5-35B-A3B-NVFP4:/models/ornith35:ro vllm-b12x:c50aca /models/ornith35 \
  --served-model-name ornith-35b --linear-backend b12x --moe-backend b12x --tensor-parallel-size 1 \
  --max-model-len "${ORNITH_CTX:-131072}" --max-num-seqs "${ORNITH_SEQS:-8}" --gpu-memory-utilization "${ORNITH_MEM:-0.45}" \
  --reasoning-parser qwen3 \
  $MODE \
  ${ORNITH_SPEC:+--speculative-config "$ORNITH_SPEC"} \
  --host 0.0.0.0 --port 8000

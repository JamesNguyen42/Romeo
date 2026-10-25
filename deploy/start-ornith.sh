#!/bin/sh
# MTP is off by default: vLLM rejects the b12x MoE backend for the unquantized MTP draft layer.
# Ornith 1.5 35B-A3B (NVFP4) on :8000. The memory share is explicit because the GB10 GPU uses system memory.
set -eu
docker rm -f ornith >/dev/null 2>&1 || true
exec docker run -d --name ornith --restart unless-stopped --gpus all --shm-size 8g --network host \
  -e HF_HUB_OFFLINE=1 -e TRANSFORMERS_OFFLINE=1 -e VLLM_B12X_MOE_FP4_FORCE_A16=1 \
  -v /opt/hack/models/Ornith-1.5-35B-A3B-NVFP4:/models/ornith35:ro vllm-b12x:c50aca /models/ornith35 \
  --served-model-name ornith-35b --linear-backend b12x --moe-backend b12x --tensor-parallel-size 1 \
  --max-model-len 8192 --max-num-seqs 4 --gpu-memory-utilization "${ORNITH_MEM:-0.35}" \
  --reasoning-parser qwen3 \
  ${ORNITH_SPEC:+--speculative-config "$ORNITH_SPEC"} \
  --host 0.0.0.0 --port 8000

#!/bin/sh
# Clef-flash decision server on :8001. It uses the Clef joint-schema head, which vLLM cannot run.
set -eu
docker rm -f clef >/dev/null 2>&1 || true
exec docker run -d --name clef --restart unless-stopped --gpus all --network host --shm-size 4g \
  -e HF_HUB_OFFLINE=1 -e TRANSFORMERS_OFFLINE=1 -e CLEF_DIR=/models/clef -e CLEF_PORT=8001 \
  -v /opt/hack/models/clef-flash:/models/clef:ro -v /opt/hack/Romeo/backend:/app:ro \
  --entrypoint python3 vllm-b12x:c50aca /app/clef_server.py

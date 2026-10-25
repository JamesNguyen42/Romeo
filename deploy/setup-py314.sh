#!/bin/sh
# Free-threaded Python 3.14 environment for the Clef server, in /opt/hack/py314.
# Tries the free-threaded build (3.14t) first; falls back to plain 3.14 if PyTorch has no wheel for it.
set -eu
export PATH="$HOME/.local/bin:$PATH"
command -v uv >/dev/null || curl -LsSf https://astral.sh/uv/install.sh | sh
for py in 3.14t 3.14; do
  rm -rf /opt/hack/py314
  uv python install "$py" && uv venv --python "$py" /opt/hack/py314 || continue
  if uv pip install --python /opt/hack/py314/bin/python --index-url https://download.pytorch.org/whl/cu130 torch \
     && uv pip install --python /opt/hack/py314/bin/python "transformers>=5.10" safetensors pillow fastapi uvicorn; then
    /opt/hack/py314/bin/python -c "import sys, torch; print('python', sys.version.split()[0], 'gil', getattr(sys, '_is_gil_enabled', lambda: True)(), 'torch', torch.__version__, 'cuda', torch.cuda.is_available())"
    echo "PY314-READY $py"
    exit 0
  fi
done
echo "PY314-FAILED"
exit 1

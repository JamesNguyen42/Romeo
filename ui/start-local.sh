#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
if [ ! -f ui/dist/index.html ]; then
  echo 'Build the UI first: cd ui && npm ci && npm run build' >&2
  exit 1
fi
build="${ROMEO_BUILD_DIR:-$root/build}"
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --parallel 2
export ROMEO_PORT="${ROMEO_PORT:-18790}"
export ROMEO_DATA="${ROMEO_DATA:-$root/runs/local-ui}"
export ROMEO_UI="$root/ui/dist"
export ROMEO_RENDER="$build/romeo-render"
export ROMEO_CASES="$root/cases/cases.json"
# The local launcher uses no script catalog. The comparison path needs only the renderer.
export ROMEO_TOOLS="${ROMEO_TOOLS:-$root/runs/no-script-tools}"
mkdir -p "$ROMEO_TOOLS"
echo "Romeo UI: http://localhost:$ROMEO_PORT"
exec "$build/romeo"

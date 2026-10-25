#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

if [[ "${1:-}" == '--ui-built' && $# == 1 ]]; then
  # The Windows launcher has already built the same files using native Node.
  :
elif [[ $# == 0 ]]; then
  command -v npm >/dev/null || { echo 'Install Node.js 22.12+ (with npm) first.' >&2; exit 1; }
  npm --prefix "$root/ui" run build
else
  echo 'Usage: bash ui/start-local.sh (configure with ROMEO_PORT, ROMEO_DATA or ROMEO_BUILD_DIR)' >&2
  exit 2
fi
[[ -f "$root/ui/dist/index.html" ]] || { echo 'The UI build did not produce ui/dist/index.html.' >&2; exit 1; }

for tool in cmake ninja c++ flock; do
  command -v "$tool" >/dev/null || { echo "Missing $tool. Install CMake, Ninja, a C++20 compiler, libcurl development headers and util-linux (flock)." >&2; exit 1; }
done
build="${ROMEO_BUILD_DIR:-$root/build/story-ui}"
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --target romeo romeo-render --parallel 2

export ROMEO_PORT="${ROMEO_PORT:-18790}"
if [[ ! "$ROMEO_PORT" =~ ^[0-9]{1,5}$ ]] || (( 10#$ROMEO_PORT < 1 || 10#$ROMEO_PORT > 65535 )); then
  echo 'ROMEO_PORT must be a number from 1 to 65535.' >&2
  exit 2
fi
# Refuse to replace an existing service. In particular, do not reset its cases or terminate its jobs.
if (exec 3<>"/dev/tcp/127.0.0.1/$ROMEO_PORT") 2>/dev/null; then
  echo "Port $ROMEO_PORT is already in use. The UI and backend have been rebuilt." >&2
  echo "Stop that service in its terminal and run this command again, or choose another ROMEO_PORT." >&2
  exit 1
fi

export ROMEO_DATA="${ROMEO_DATA:-$root/runs/story-ui}"
# The descriptor survives exec, holding this lease for the service's lifetime. Changing only the port must
# never allow two launcher instances to write the same case log. Do not remove or replace the lock file.
mkdir -p "$ROMEO_DATA"
exec 9>"$ROMEO_DATA/.launcher.lock"
if ! flock -n 9; then
  echo "Another Romeo launcher is already using $ROMEO_DATA. Saved cases were not changed." >&2
  echo 'Stop that service, or choose a separate ROMEO_DATA directory for a concurrent instance.' >&2
  exit 1
fi
export ROMEO_UI="$root/ui/dist"
export ROMEO_RENDER="$build/romeo-render"
export ROMEO_CASES="$root/cases/cases.json"
export ROMEO_TOOLS="${ROMEO_TOOLS:-$root/runs/story-ui-tools}"
export ROMEO_THINK="${ROMEO_THINK:-0}"
mkdir -p "$ROMEO_TOOLS"
printf 'Story UI and API: http://localhost:%s\nSaved runs: %s\n' "$ROMEO_PORT" "$ROMEO_DATA"
echo 'Startup creates no cases and runs no tests. Keep this terminal open; Ctrl+C stops the service.'
exec "$build/romeo"

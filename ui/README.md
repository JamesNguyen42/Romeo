# Romeo UI

A live case board and visual story for Romeo's renderer. The page uses real run artifacts and one server-sent event connection. It includes a rotating wall view, canvas timing/memory charts, a 40-image replay for each comparison side, and an evidence drawer.

## Run on Linux / the GB10

Use Node 22.12+ and a C++20 toolchain with CMake 3.25+, Ninja, and libcurl development headers.

```sh
cd ui
npm ci
npm run build
cd ..
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ROMEO_UI=ui/dist ROMEO_RENDER=build/romeo-render ./build/romeo
```

Open `http://localhost:8790`. The backend serves the compiled UI. It is required; opening `index.html` from disk will not run tests. Rebuild the UI after source changes. Windows requires WSL for the C++ service.

For this Windows workstation, `ui/start-local.ps1` builds the frontend and starts the C++ service in `Radiance-Ubuntu` on port 18790. Override `-Distro` or `-Port` as needed. It keeps startup logs in `runs/launcher` and reuses an already-running Romeo service on that port. The WSL distribution needs the build dependencies above. `ui/start-local.sh` is the equivalent service launcher on Linux after building the frontend.

For frontend development against an already-running service:

```sh
cd ui
npm ci
ROMEO_API=http://127.0.0.1:8790 npm run dev
```

In PowerShell, set `$env:ROMEO_API='http://127.0.0.1:8790'` before `npm run dev`. Vite proxies `/api`, including SSE, to that address. No fixtures are loaded automatically.

## What the controls do

- **Run test** creates a new, durable comparison case. It executes the configured renderer with the scenario's intentional bug flags, then without them. Jobs run serially with a matched camera seed. This does not build a historical revision or assert a fix.
- **40 rendered frames** displays either run's sampled camera frames, re-rendered after the benchmark and downsampled to 480×270. Their timing captions come from the original frame measurements across all cameras. PNG generation is excluded from benchmark statistics. Playback is a visual replay, not a camera feed.
- **Test new angles** starts another comparison with a different, recorded camera seed. It creates new scene views, not a claim about any model's training data or a held-out classification dataset.
- **Ask Romeo to investigate** uses the existing Clef/Ornith agent workflow. It needs the configured model services; revision-based cases also need the product repository used by `tools/build_rev`. No decision probabilities are invented when those services are unavailable. This workflow may use the configured script-tool catalog, so use an empty `ROMEO_TOOLS` directory for a measurement-only local environment.
- **Cases** preserves previous runs. Case links use `#case=...` and survive refresh. No reset or deletion button is exposed.
- **Wall** enters full screen and rotates through cases every 14 seconds. GPU data arrives on the same event stream; unavailable GPU readings are omitted.

The 40-frame warm-up is excluded from summary timing statistics and timing charts. The replay includes sampled warm-up frames with their recorded timings. Results reflect the current machine and workload. A failed render or missing model ends with a visible explanation; completing a comparison is not a diagnosis or verification of a code change.

## Checks

```sh
cd ui
npm test
npm run build
cd ..
ROMEO_RENDER=build/romeo-render python3 tests/renderer_preview_test.py -v
```

Backend API/WAL tests live in `backend/tests/`; see their header for the isolated-service command. Keep tests separate from a service containing runs you want to preserve.

The UI preserves the prototype's Archivo typeface and warm grey/red palette. Archivo is included under the SIL Open Font License in `public/fonts/OFL.txt`.

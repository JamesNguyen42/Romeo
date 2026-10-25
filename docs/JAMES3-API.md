# James3 renderer UI API

The UI runs the existing renderer and reads its measurements and images. It has no crosswalk detector, camera feed, or held-out image dataset. A new view seed changes the rendered camera view; it does not establish that a model has never seen an image.

## Build and start

Install Node.js 22.12+ with npm, plus a Linux C++20 toolchain, CMake 3.25+, Ninja, libcurl development files and util-linux (`flock`). Windows uses WSL (`Radiance-Ubuntu` by default). From the repository root, one command builds the original story page and the C++ service, then serves the frontend and API together:

```sh
npm start
```

Open `http://localhost:18790` and keep the terminal open. Startup submits no cases or tests and loads no fixtures. It uses `runs/story-ui` for saved state and `build/story-ui` for binaries. A new data directory starts empty; an existing one keeps its history and previously requested work. The UI initially selects no case. An occupied port produces an error after rebuilding, without replacing that service.

See `ui/README.md` for prerequisites and port/data/build overrides. For a concurrent instance, use both another port and a separate data directory, for example `ROMEO_PORT=18792 ROMEO_DATA=runs/story-ui-18792 npm start` on Linux; PowerShell users can set the corresponding environment variables before `npm start`. The launcher holds an exclusive lock on its data directory and refuses another launcher using it. No saved data is reset. The local launcher uses an empty script-tool directory by default. The service serves `ui/dist` at `/`, so a separate frontend server is unnecessary.

The default model URLs are Clef at `http://127.0.0.1:8001/v1/systemone` and Ornith at `http://127.0.0.1:8000/v1/chat/completions`. Override them with `CLEF_URL` and `ORNITH_URL`. Comparison mode does not call either model. Investigation mode additionally needs the configured model services and, for revision-based scenarios, the product repository and refs used by `tools/build_rev` (`PRODUCT_DIR`, default `/opt/hack/product`).

## Select and start a scenario

`GET /api/scenarios` returns:

```json
{
  "scenarios": [
    {"id":"case-07","title":"4K image looks blurry after an upgrade","report":"...","setup":"C","difficulty":""}
  ],
  "capacity": {"used":0,"max":64}
}
```

`POST /api/scenarios/{id}/runs` accepts:

```json
{"mode":"comparison","view_seed":4242}
```

The body is optional; mode defaults to `comparison`. The seed is an optional unsigned 32-bit integer; omitting it chooses a fresh camera seed. A successful response is a normal case summary:

```json
{
  "id":"run-1792962341548-1",
  "scenario_id":"case-07",
  "mode":"comparison",
  "view_seed":4242,
  "title":"4K image looks blurry after an upgrade",
  "setup":"C",
  "status":"investigating",
  "waiting_for":"",
  "updated_ms":1792962341548,
  "event_count":1
}
```

Every request creates a new case; it does not overwrite a previous run. The UI disables repeated clicks while creation is pending. At 64 stored cases the endpoint returns HTTP 503 and preserves history. Unknown scenarios return 404; invalid modes/seeds or malformed bodies return 400.

Comparison mode runs the local `ROMEO_RENDER` binary twice: once with the scenario's intentional bug flags and once without them. Both runs use the same setup, frame count, and view seed. This is a controlled comparison of the local renderer, not a build of the scenario's historical Git revisions. Renderer jobs run one at a time across cases. Each job receives `--preview-frames 40 --view-seed N`.

Use `{"mode":"investigation"}` for the existing model-directed diagnostic flow. Clef selects tools; Ornith writes explanations. These runs use the scenario's configured revisions and tool catalog, with their original camera (`view_seed:0`); a nonzero seed is rejected. Their decisions are tool-choice probabilities, not image detections or diagnosis confidence. A required model failure stops the investigation with a visible reason; it does not fabricate predictions or retry indefinitely. Optional thought failures do not stop other work.

## Events and artifacts

Use `GET /api/cases`, `GET /api/cases/{id}`, and a single `EventSource('/api/stream')`. Case details include their event history. The stream emits ordinary case events with `{seq,case,t_ms,type,data}`. It has no replay cursor: connect first, fetch snapshots, and reconcile snapshots again on reconnect. Deduplicate events by `(case, seq)`.

Comparison progression:

1. `case_opened`: includes `scenario_id`, `mode`, `view_seed`, and execution metadata persisted in the write-ahead log. Recovery preserves the run's original bug flags, revision refs, and withheld tools even if the scenario file changes.
2. `action_requested`: `kind` is `measure_frames` for the scenario, then `compare_baseline`. Includes `action_id`, `label`, `job`, and `args` with setup, view seed, and preview count.
3. `action_result`: includes `source:"run"`, `exit_code`, `baseline`, `summary`, `artifacts`, and numeric `metrics` when a complete measurement summary exists. Failed runs expose logs/output and do not invent metrics.
4. `comparison_complete`: emitted only when both processes exit successfully and both summaries are valid. Contains `success:true`, `current_action_id`, `baseline_action_id`, `view_seed`, `current`, and `baseline` metric objects. The case status becomes `done`. This means the measurements finished; it is not a diagnosis or a verified fix.

A failed process or incomplete summary ends with `stopped`, with `reason` and `missing`. Investigation verification also stops on a failed process or invalid summary, rather than reporting success.

The metric object contains `p50_ms`, `p95_ms`, `p99_ms`, `max_ms`, `total_ms`, `spike_frames`, `views`, `frames`, `resident_end_bytes`, `tmp_bytes`, and `cache_entries`. Full `summary.json` additionally records renderer dimensions and warm-up/sample metadata. Units are explicit in the property names; bytes are not megabytes.

Artifact entries have the form `<action_id>/<filename>`. Fetch them at `/api/artifacts/{case_id}/{action_id}/{filename}`. Successful comparisons expose `summary.json`, `frames.csv`, `memory.csv`, `image.png`, and `preview.json`, plus any trace/log files produced. The preview manifest identifies its 40 image files, each served through the same artifact route.

`image.png` is the renderer's final full-resolution image. Preview images are re-rendered after the timed benchmark at sampled frame/view positions, then downsampled. Their capture cost is excluded from reported benchmark metrics. `preview.json` explicitly marks `kind:"renderer_replay"` and `capture:"rerendered_after_benchmark"`; entries link each picture to its measured `benchmark_frame_ms`, frame, view, and camera angle. Treat them as a replay of the measured run, not a live camera stream.

The same EventSource also receives a named `system` event once per second, with the `/api/system` JSON shape. Register `addEventListener('system', handler)`. `gpu_available:false` means GPU telemetry is unavailable, not measured zero usage. `gpu_sample_ms` identifies the last successful GPU sample; `t_ms` is the snapshot time.

## Regression checks

```sh
ROMEO_TEST_BINARY="$PWD/build/romeo" python3 backend/tests/test_scenario_runs.py
ROMEO_RENDER="$PWD/build/romeo-render" python3 tests/renderer_preview_test.py
```

The backend suite uses an explicitly synthetic process to test orchestration, WAL recovery, errors, capacity, and model-response handling. It is not performance evidence. The renderer suite runs the actual binary and checks preview/image/measurement correspondence. `tests/messages_test.py` is an unrelated live Slack integration suite requiring its own credentials and dependencies.

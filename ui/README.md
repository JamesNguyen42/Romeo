# Juliet story UI

The original Crosswalk Case Story layout, connected to Juliet's renderer and saved evidence. The frontend is assembled by a dependency-free Node build. Juliet's C++ server serves the page and its API together.

The existing `romeo` and `romeo-render` binary names, `ROMEO_*` settings and repository paths remain compatible with earlier integrations.

## Start both with one command

From the repository root:

```sh
npm start
```

Open `http://localhost:18790`. Keep the terminal open; Ctrl+C stops the service. The command rebuilds the story page and C++ programs before starting. No npm dependency installation is needed.

Startup submits no cases, loads no fixtures and runs no tests. The default data directory is `runs/story-ui`, separate from earlier previews. A new directory starts empty; the story initially has no selected case. Existing directories are preserved. Reusing one restores its saved history and can continue work that was already requested.

If the port is occupied, startup stops with an explanation after rebuilding. It does not kill or replace the existing service. Stop that service in its original terminal, or use another port and a separate data directory for a concurrent instance. The launcher locks its data directory for the service's lifetime; it refuses a second launcher using the same directory. No data is reset.

Windows PowerShell:

```powershell
$env:ROMEO_PORT = '18792'
$env:ROMEO_DATA = 'runs/story-ui-18792'
npm start
```

Linux:

```sh
ROMEO_PORT=18792 ROMEO_DATA=runs/story-ui-18792 npm start
```

## Prerequisites

- Node.js 22.12+ with npm.
- Linux with a C++20 compiler, CMake 3.25+, Ninja, libcurl development headers, and `flock` from util-linux.
- On Windows, WSL with those C++ dependencies. The default distribution is `Radiance-Ubuntu`; set `ROMEO_WSL_DISTRO` to choose another installed distribution. Node and npm run on Windows, so Node is not required inside WSL.

For Ubuntu, install the build tools once:

```sh
sudo apt-get install build-essential cmake ninja-build libcurl4-openssl-dev util-linux
```

## Paths and options

The default C++ build directory is `build/story-ui`. Set `ROMEO_BUILD_DIR` or `ROMEO_DATA` to use other locations. Choose a new data directory for an empty session; no previous directory is deleted or reset. The local launcher uses an empty script-tool directory by default.

The platform launchers can also be called directly from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ui/start-local.ps1 -Port 18792 -DataDirectory runs/story-ui-18792
```

```sh
bash ui/start-local.sh
```

PowerShell also accepts `-Distro`, `-DataDirectory`, and `-BuildDirectory`. Data/build paths can be Windows absolute paths or Linux paths. Linux accepts `ROMEO_PORT`, `ROMEO_DATA`, `ROMEO_BUILD_DIR`, and the backend's normal environment settings.

To rebuild only the page, without starting a service or running a test:

```sh
npm --prefix ui run build
```

Renderer measurements and replay artifacts are documented in [JAMES3-API.md](../docs/JAMES3-API.md). Comparison measurements need no model service. Model-directed investigations additionally need the configured Clef/Ornith services and product revisions.

## Original story source

- `ui/story/template.html` contains the original layout, styles and visual story markup.
- `ui/story/logic.js` binds that layout to the selected case and its live measurements.
- `ui/story/tool-story.js` turns recorded decisions, actions and facts into the v3 tool-choice scene. It uses actual offered options and probabilities, with controls to replay earlier choices. An empty case shows a reference list without scores; a direct comparison clearly says it makes no model call.
- `ui/story/live-data.js` reads events, case records and renderer artifacts. A new run is submitted only by a run control.
- `ui/story/runtime.html` preserves the original bundled runtime and embedded assets.

`ui/build.mjs` combines these sources into `ui/index.html` and `ui/dist/index.html`, with the font license alongside the served page. Edit the source files rather than those generated copies. Building produces page files; it does not start a backend, load case data or execute a renderer test.

Step 3 preserves the v3 case notebook and grouped tool cards. Playback reveals already recorded events; it never executes a tool. A request can be queued or running until a result arrives. Missing probabilities, unavailable evidence and failed results are not presented as successful choices or completed tests. Tool absence is not a recorded explanation of eligibility, so the UI does not invent blocking reasons. The pictured future learning loop is not presented as a current capability.

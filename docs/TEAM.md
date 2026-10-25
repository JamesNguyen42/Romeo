# Romeo: how the pieces fit

Romeo is an always-on engineering agent. It owns open cases (a frame spike, a crash, a blurry render), keeps every
piece of evidence in a durable case log, waits for results, survives a restart, and picks its next action from what
it has actually measured.

## Who owns what

| Folder | Owner | What it is |
| --- | --- | --- |
| `backend/` | Cosmin | Case log, resume after restart, action outbox, result intake, HTTP API |
| `renderer/` | Cosmin | Small C++17 renderer, setups A–E, frame-time / memory / trace / log output |
| `cases/` | Cosmin | The 10 demo cases and how each bug is triggered |
| `ui/` | Teammate A | Case board and visuals |
| `chat/` | Teammate B | OpenClaw connected to Telegram, Slack and Discord |

| `memory/` | Teammate B | Case memory: find similar past cases, spot duplicates |

### Teammate A: UI and story

1. **Case board** for engineers: all cases with status, setup, what each case waits for, live from `/api/stream`.
2. **Story mode** for non-technical viewers. One case at a time, told in plain words with big visuals:
   - "Something went wrong": the report, plus the frame-time or memory chart with the bad part circled.
   - "What it looked at": evidence cards appear one by one as `fact` events arrive.
   - "What it was thinking": the decision bars, with each action label in plain words ("look at the timing trace").
   - "It was interrupted and picked up where it left off": the `restarted` event as its own scene.
   - "What it found": before/after images or charts, and the proposed fix in one sentence.
   - "What it could not prove": the `stopped` reasons. Honesty is part of the pitch.
3. **Wall display**: a full-screen view for the room, cycling through cases, with live GPU use from the GB10.

Keep it fast: one `EventSource` connection, no polling, charts drawn on `<canvas>`, smooth at 60 frames per second
with all 10 cases open.

### Teammate B: chat and memory

1. **Chat**: OpenClaw connected to Telegram, Slack and Discord. Commands: `/cases`, `/case <id>`, `/report <text>`
   (opens a case through `POST /api/cases`). Post a message when a case changes `status`, with the chart or image.
2. **Case memory** (`memory/`): a small service that keeps an embedding for each case's title, report and facts,
   using `all-MiniLM-L6-v2` (90 MB, runs on CPU). Given a new case, it returns the closest past cases with a
   similarity score. The backend uses it to flag duplicates (for example, a second report of the same crash).
   Interface: `POST /similar {"text": "..."}` returns `[{"case": "case-05", "score": 0.91}, ...]` on port 8791.

Work only inside your own folder. Everyone commits straight to `main`; the GB10 pulls `main` every 10 seconds into
`/opt/hack/Romeo`, so a push is live on the device within about 10 seconds.

## Rules for every commit

- No secrets in the repo. Bot tokens and keys go in `.env` (git-ignored).
- No references to AI coding tools in code, comments, docs or commit messages.
- No company names, customer names or data from anyone's employer. Every case, log and number comes from our own
  renderer runs.

## Machines and ports

All services run on the GB10 (`promaxgb10-95c3.local`). Laptops reach it by joining the same phone hotspot; the venue
Wi-Fi blocks laptop-to-laptop traffic.

| Port | Service |
| --- | --- |
| 8790 | Romeo backend: HTTP API, live event stream, and the UI's static files from `ui/dist/` |
| 8000 | Ornith 1.5 35B (vLLM, OpenAI-compatible): writes drafts and summaries |
| 8001 | Clef-flash (vLLM): typed next-action decisions with probabilities |
| 8791 | Case memory: similar-case search |

## Backend API (port 8790)

| Method and path | Returns |
| --- | --- |
| `GET /api/cases` | Array of case summaries (below) |
| `GET /api/cases/{id}` | One case summary plus `events`: the full event list in order |
| `GET /api/stream` | Server-Sent Events. Every new case event, one JSON object per `data:` line |
| `POST /api/cases` | Open a case. Body: `{"title": "...", "setup": "D", "report": "free text"}`. Returns the summary |
| `POST /api/cases/{id}/results` | Deliver a result for a pending action. Body: `{"action_id": "...", "source": "run" \| "fixture", "data": {...}}` |
| `GET /api/artifacts/{id}/{name}` | A file the case produced: PNG image, frame-time CSV, Chrome-trace JSON, log JSONL |

Case summary:

```json
{
  "id": "case-06",
  "title": "Frame spikes every few seconds",
  "setup": "D",
  "status": "waiting",
  "waiting_for": "longer trace capture on setup D",
  "updated_ms": 1793000000000,
  "event_count": 9
}
```

`status` is one of `investigating`, `waiting`, `stopped`, `done`.

## Case events

The backend appends one event per line to the case log and never edits earlier lines. The UI and the chat bots only
read these events; they never need any other state.

```json
{"seq": 4, "case": "case-06", "t_ms": 1793000000000, "type": "decision", "data": {...}}
```

| `type` | `data` fields | Good visual |
| --- | --- | --- |
| `case_opened` | `title`, `setup`, `report` | New card on the board |
| `decision` | `candidates`: `[{"action": "...", "p": 0.71}, ...]`, `chosen` | Probability bars, chosen bar highlighted |
| `action_requested` | `action_id`, `kind`, `args` | "Waiting for …" chip with a timer |
| `action_result` | `action_id`, `source` (`run` or `fixture`), `summary`, `artifacts` | Chart, trace or image from `artifacts` |
| `fact` | `text`, `evidence`: artifact names | Line added to the evidence list |
| `restarted` | `pending`: action IDs still awaited | "Service restarted, still waiting" banner |
| `stopped` | `reason`, `missing`: what evidence would be needed | Grey card with the missing list |
| `done` | `summary`, `fix`: proposed change, `checks`: what passed | Green card, before/after visuals |

`source: "fixture"` marks a result we supplied by hand. Show it with a visible "fixture" label.

## Setups

| Setup | Views | Resolution | Worker threads |
| --- | --- | --- | --- |
| A | 1 | 1080p | 4 |
| B | 4 | 1080p | 8 |
| C | 1 | 4K | 8 |
| D | 8 | 1440p | 12 |
| E | 12 | 1080p | 16 |

## Until the backend is live

Build against `docs/sample-events.jsonl`. It is a hand-written, illustrative event stream for one case, in exactly
the format above.

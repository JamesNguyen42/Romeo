import { useEffect, useRef, useState } from "react";
import {
  artifactUrl,
  parseCsv,
  useRomeo,
  type CaseDetail,
  type CaseEvent,
} from "./data";

type Json = Record<string, any>;
type Run = {
  event: CaseEvent;
  baseline: boolean;
  image?: string;
  files: string[];
  summary?: Json;
  frames?: Json[];
  memory?: Json[];
  preview?: Json;
  error?: string;
};
const setupNames: Record<string, string> = {
  A: "1 camera · 1080p",
  B: "4 cameras · 1080p",
  C: "1 camera · 4K",
  D: "8 cameras · 1440p",
  E: "12 cameras · 1080p",
};
const steps = [
  "The report",
  "Run the test",
  "See the difference",
  "40 rendered frames",
  "The next move",
  "What we know",
];
const shortTitles: Record<string, string> = {
  "case-01": "Memory keeps climbing.",
  "case-02": "A hitch every few seconds.",
  "case-07": "The image looks soft.",
  "case-05": "The render never starts.",
};
const actionLabels: Record<string, string> = {
  reproduce: "Recreate the problem",
  capture_trace: "Look inside the slowdown",
  compare_baseline: "Compare with the baseline",
  measure_memory: "Watch memory use",
  compare_images: "Compare the pictures",
  fetch_logs: "Read the error log",
  inspect_config: "Check the settings",
  try_smaller_setup: "Try fewer cameras",
  check_disk: "Check disk use",
  conclude: "Explain the cause",
  stop_need_more: "Ask for more evidence",
  verify_fix: "Check the proposed fix",
};
const actionName = (name: string) =>
  actionLabels[name] || name.replaceAll("_", " ");
const number = (v: unknown, digits = 1) =>
  typeof v === "number" && Number.isFinite(v)
    ? v.toLocaleString(undefined, { maximumFractionDigits: digits })
    : "—";
const last = (events: CaseEvent[], type: string) =>
  [...events].reverse().find((e) => e.type === type);
const sourceLabel = (run: Run | undefined, label: string) =>
  `${label}${run?.event.data.source === "fixture" ? " · FIXTURE" : ""}`;
const successful = (run?: Run) =>
  !!run?.summary && run.event.data.exit_code === 0;
const statusName = (c?: CaseDetail) =>
  !c
    ? "Ready to test"
    : c.status === "done"
      ? "Run complete"
      : c.status === "stopped"
        ? "Needs attention"
        : c.status === "waiting"
          ? "Running"
          : "Investigating";

function useRuns(c?: CaseDetail) {
  const [runs, setRuns] = useState<Run[]>([]);
  const caseId = useRef<string | undefined>(undefined);
  const results =
    c?.events.filter(
      (e) => e.type === "action_result" && Array.isArray(e.data.artifacts),
    ) || [];
  const key = JSON.stringify(
    results.map((e) => [c?.id, e.seq, e.data.artifacts]),
  );
  useEffect(() => {
    const controller = new AbortController();
    if (caseId.current !== c?.id) {
      setRuns([]);
      caseId.current = c?.id;
    }
    if (!c) return;
    const load = async () => {
      const loaded = await Promise.all(
        results.map(async (event) => {
          const files = (event.data.artifacts as unknown[])
            .map((p) => artifactUrl(c.id, p))
            .filter((p): p is string => !!p);
          const run: Run = {
            event,
            files,
            baseline: event.data.baseline === true,
            summary: event.data.metrics,
            image: files.find((f) => f.endsWith("/image.png")),
          };
          for (const [suffix, field] of [
            ["summary.json", "summary"],
            ["frames.csv", "frames"],
            ["memory.csv", "memory"],
            ["preview.json", "preview"],
          ] as const) {
            const url = files.find((f) => f.endsWith("/" + suffix));
            if (!url) continue;
            try {
              const response = await fetch(url, { signal: controller.signal });
              if (!response.ok) throw new Error(`Could not read ${suffix}`);
              const text = await response.text();
              (run as any)[field] = suffix.endsWith(".csv")
                ? parseCsv(text)
                : JSON.parse(text);
            } catch (error) {
              if (!controller.signal.aborted)
                run.error =
                  error instanceof Error
                    ? error.message
                    : "Evidence unavailable";
            }
          }
          return run;
        }),
      );
      if (!controller.signal.aborted) setRuns(loaded);
    };
    void load();
    return () => controller.abort();
  }, [key]);
  return runs;
}

function RenderImage({
  src,
  label,
  detail = false,
}: {
  src?: string;
  label: string;
  detail?: boolean;
}) {
  const [broken, setBroken] = useState(false);
  useEffect(() => setBroken(false), [src]);
  return (
    <figure className={"render-image " + (detail ? "detail" : "")}>
      {src && !broken ? (
        <img src={src} alt={label} onError={() => setBroken(true)} />
      ) : (
        <div className="image-empty">
          <span className="cube-mark" aria-hidden="true">
            ◇
          </span>
          <span>
            {broken ? "Image unavailable" : "Waiting for renderer output"}
          </span>
        </div>
      )}
      <figcaption>{label}</figcaption>
    </figure>
  );
}

function Metric({
  label,
  value,
  unit,
}: {
  label: string;
  value: unknown;
  unit: string;
}) {
  return (
    <div className="metric">
      <span>{label}</span>
      <strong>
        {number(value)}
        <small>{unit}</small>
      </strong>
    </div>
  );
}

function Chart({ runs, memory = false }: { runs: Run[]; memory?: boolean }) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const series = runs.filter((r) =>
    memory ? r.memory?.length : r.frames?.length,
  );
  useEffect(() => {
    const el = canvas.current;
    if (!el) return;
    const draw = () => {
      const width = el.clientWidth,
        height = el.clientHeight;
      const dpr = window.devicePixelRatio || 1;
      el.width = width * dpr;
      el.height = height * dpr;
      const ctx = el.getContext("2d");
      if (!ctx) return;
      ctx.scale(dpr, dpr);
      ctx.clearRect(0, 0, width, height);
      const points = series.map((r) =>
        (memory ? r.memory! : r.frames!.filter((row) => row.frame >= 40))
          .map((row) => ({
            x: memory ? row.t_ms / 1000 : row.frame,
            y: memory ? row.resident_bytes / 1048576 : row.ms,
          }))
          .filter((p) => Number.isFinite(p.x) && Number.isFinite(p.y)),
      );
      const flat = points.flat();
      if (!flat.length) return;
      const maxX = Math.max(...flat.map((p) => p.x), 1),
        minX = Math.min(...flat.map((p) => p.x));
      const maxY = Math.max(...flat.map((p) => p.y), 1) * 1.12;
      const l = 52,
        r = 18,
        t = 16,
        b = 32;
      const x = (v: number) =>
        l + ((v - minX) / (maxX - minX || 1)) * (width - l - r);
      const y = (v: number) => height - b - (v / maxY) * (height - t - b);
      ctx.font = "11px ui-monospace, monospace";
      ctx.fillStyle = "#69645f";
      ctx.strokeStyle = "#dbd7d2";
      ctx.lineWidth = 1;
      for (let i = 0; i < 5; i++) {
        const v = (maxY * i) / 4;
        ctx.beginPath();
        ctx.moveTo(l, y(v));
        ctx.lineTo(width - r, y(v));
        ctx.stroke();
        ctx.fillText(number(v, 0), 4, y(v) + 4);
      }
      ctx.fillText(number(minX, 0), l, height - 8);
      ctx.textAlign = "right";
      ctx.fillText(
        `${number(maxX, 0)} ${memory ? "seconds" : "frame"}`,
        width - r,
        height - 8,
      );
      ctx.textAlign = "left";
      points.forEach((row, i) => {
        ctx.strokeStyle = series[i].baseline ? "#327363" : "#df361b";
        ctx.lineWidth = 2;
        ctx.beginPath();
        row.forEach((p, j) =>
          j ? ctx.lineTo(x(p.x), y(p.y)) : ctx.moveTo(x(p.x), y(p.y)),
        );
        ctx.stroke();
        if (!series[i].baseline && row.length) {
          const peak = row.reduce((a, b) => (a.y > b.y ? a : b));
          ctx.beginPath();
          ctx.arc(x(peak.x), y(peak.y), 5, 0, Math.PI * 2);
          ctx.fillStyle = "#df361b";
          ctx.fill();
          ctx.textAlign = x(peak.x) > width - 100 ? "right" : "left";
          ctx.fillText(
            `${number(peak.y)} ${memory ? "MiB" : "ms"}`,
            x(peak.x),
            Math.max(12, y(peak.y) - 10),
          );
          ctx.textAlign = "left";
        }
      });
    };
    const observer = new ResizeObserver(draw);
    observer.observe(el);
    draw();
    return () => observer.disconnect();
  }, [runs, memory]);
  return (
    <div className="chart">
      <div className="chart-heading">
        <b>{memory ? "Memory in use" : "Time for each frame"}</b>
        <span>{memory ? "MiB" : "milliseconds · lower is faster"}</span>
      </div>
      {series.length ? (
        <canvas
          ref={canvas}
          role="img"
          aria-label={
            memory
              ? "Measured memory over time; exact data available in evidence downloads"
              : "Measured frame times; exact data available in evidence downloads"
          }
        />
      ) : (
        <div className="chart-empty">
          The chart appears when a run finishes.
        </div>
      )}
      <div className="legend">
        <span>
          <i />
          With the issue
        </span>
        <span>
          <i className="baseline" />
          Baseline
        </span>
      </div>
    </div>
  );
}

function PreviewGallery({ run, caseId }: { run?: Run; caseId?: string }) {
  const [active, setActive] = useState(0);
  const [playing, setPlaying] = useState(false);
  const frames: Json[] = Array.isArray(run?.preview?.frames)
    ? run!.preview!.frames
    : [];
  useEffect(() => {
    setActive(0);
    setPlaying(false);
  }, [run?.event.seq, caseId]);
  useEffect(() => {
    if (!playing || !frames.length) return;
    const timer = setInterval(
      () => setActive((i) => (i + 1) % frames.length),
      180,
    );
    return () => clearInterval(timer);
  }, [playing, frames.length]);
  const src = (f: Json) => {
    const manifest = run?.files.find((f) => f.endsWith("/preview.json"));
    const file = f.file || f.image || f.path;
    if (!caseId || !manifest || typeof file !== "string") return undefined;
    return (
      artifactUrl(
        caseId,
        manifest.slice(0, manifest.lastIndexOf("/") + 1) + file,
      ) || undefined
    );
  };
  return frames.length ? (
    <div className="preview-layout">
      <div className="preview-main">
        <RenderImage
          src={src(frames[active])}
          label={sourceLabel(
            run,
            `Rendered replay · frame ${frames[active].frame}`,
          )}
        />
        <div className="replay-controls">
          <button onClick={() => setPlaying(!playing)}>
            {playing ? "Pause" : "Play"} frames
          </button>
          <span>
            {String(active + 1).padStart(2, "0")} / {frames.length}
          </span>
        </div>
        <p className="small-note">
          Camera {frames[active].view + 1} ·{" "}
          {number(frames[active].benchmark_frame_ms)} ms for the measured frame
          across all cameras
        </p>
      </div>
      <div className="frame-grid">
        {frames.map((f, i) => (
          <button
            key={i}
            className={active === i ? "active" : ""}
            aria-label={`Show frame ${i + 1}`}
            aria-pressed={active === i}
            onClick={() => {
              setActive(i);
              setPlaying(false);
            }}
          >
            <img loading="lazy" src={src(f)} alt="" />
            <span>{String(i + 1).padStart(2, "0")}</span>
          </button>
        ))}
      </div>
    </div>
  ) : (
    <Empty
      icon="▦"
      title="Rendered frames, after the test."
      text="Each completed render can save 40 sampled frames. Select one to look closer."
    />
  );
}

function Empty({
  icon,
  title,
  text,
}: {
  icon: string;
  title: string;
  text: string;
}) {
  return (
    <div className="empty-state">
      <span aria-hidden="true">{icon}</span>
      <h3>{title}</h3>
      <p>{text}</p>
    </div>
  );
}

function Decisions({ events }: { events: CaseEvent[] }) {
  const decision = last(events, "decision");
  if (!decision)
    return (
      <Empty
        icon="↗"
        title="Measurements first."
        text="This comparison runs the renderer directly. Ask Romeo to investigate for live diagnostic choices from Clef."
      />
    );
  const candidates = Array.isArray(decision.data.candidates)
    ? decision.data.candidates
    : [];
  return (
    <div className="decision-scene">
      <div>
        <span className="eyebrow">Next diagnostic step</span>
        <h3>{actionName(decision.data.chosen || "")}</h3>
        <p>These probabilities describe which tool to try next.</p>
        <span className="mono">
          {decision.data.model || "Clef"} · {number(decision.data.inference_ms)}{" "}
          ms
        </span>
      </div>
      <div className="decision-bars">
        {candidates.map((item: Json) => (
          <div
            className={item.action === decision.data.chosen ? "chosen" : ""}
            key={item.action}
          >
            <div>
              <span>{actionName(item.action)}</span>
              <b>
                {typeof item.p === "number"
                  ? number(item.p * 100, 1) + "%"
                  : "Unavailable"}
              </b>
            </div>
            <div className="bar-track">
              <i
                style={{
                  width: `${typeof item.p === "number" ? Math.max(0, Math.min(100, item.p * 100)) : 0}%`,
                }}
              />
            </div>
          </div>
        ))}
      </div>
    </div>
  );
}

function Evidence({ c, runs }: { c?: CaseDetail; runs: Run[] }) {
  const files = [...new Set(runs.flatMap((r) => r.files))];
  return (
    <details className="evidence">
      <summary>
        Open the evidence{" "}
        <span>{c ? `${c.events.length} events` : "No run yet"} ↗</span>
      </summary>
      <div className="evidence-body">
        {c && (
          <div className="download-row">
            <a href={`/api/cases/${encodeURIComponent(c.id)}/export`} download>
              Save case record
            </a>
            <a
              href={`/api/cases/${encodeURIComponent(c.id)}/trace`}
              target="_blank"
              rel="noreferrer"
            >
              Agent trace
            </a>
          </div>
        )}
        {runs.map((r) => (
          <p key={r.event.seq}>
            <b>{r.baseline ? "Baseline" : "With the issue"}</b> ·{" "}
            {r.event.data.source === "fixture" ? "FIXTURE · " : ""}
            {r.event.data.summary}
            {r.error && <em> {r.error}</em>}
          </p>
        ))}
        <div className="file-list">
          {files.map((f) => (
            <a key={f} href={f} target="_blank" rel="noreferrer">
              {f.split("/").slice(-2).join(" / ")}
            </a>
          ))}
        </div>
        {c && (
          <ol className="event-list">
            {c.events
              .filter((e) =>
                [
                  "fact",
                  "restarted",
                  "stopped",
                  "conclusion",
                  "comparison_complete",
                ].includes(e.type),
              )
              .map((e) => (
                <li key={e.seq}>
                  <span className="mono">
                    {new Date(e.t_ms).toLocaleTimeString()}
                  </span>{" "}
                  {e.data.text ||
                    e.data.diagnosis ||
                    e.data.summary ||
                    e.data.missing ||
                    (e.type === "restarted"
                      ? "Restarted and recovered the case record."
                      : e.type.replaceAll("_", " "))}
                </li>
              ))}
          </ol>
        )}
      </div>
    </details>
  );
}

export default function App() {
  const data = useRomeo();
  const [scenarioId, setScenarioId] = useState("case-07");
  const [selectedId, setSelectedId] = useState<string | null>(() =>
    new URLSearchParams(location.hash.slice(1)).get("case"),
  );
  const [step, setStep] = useState(0);
  const [mode, setMode] = useState<"story" | "board" | "wall">("story");
  const [busy, setBusy] = useState(false);
  const [launchError, setLaunchError] = useState("");
  const [detail, setDetail] = useState(true);
  const [memory, setMemory] = useState(false);
  const [gallerySide, setGallerySide] = useState<"current" | "baseline">(
    "current",
  );
  const [follow, setFollow] = useState(true);
  const selected = data.cases.find((c) => c.id === selectedId);
  const scenario =
    data.scenarios.find(
      (s) => s.id === (selected?.scenario_id || scenarioId),
    ) || data.scenarios.find((s) => s.id === scenarioId);
  const events = selected?.events || [];
  const runs = useRuns(selected);
  const affected = [...runs].reverse().find((r) => !r.baseline && r.image);
  const baseline = [...runs]
    .reverse()
    .find(
      (r) =>
        r.baseline &&
        r.image &&
        (!affected || r.event.data.setup === affected.event.data.setup),
    );
  const current = affected || [...runs].reverse().find((r) => !r.baseline);
  const reference = baseline || [...runs].reverse().find((r) => r.baseline);
  const running = !!selected && !["done", "stopped"].includes(selected.status);
  const opened = last(events, "case_opened");
  const stopped = last(events, "stopped");
  const conclusion = last(events, "conclusion");
  const complete = last(events, "comparison_complete");
  const restarted = last(events, "restarted");
  const facts = events.filter((e) => e.type === "fact");
  const heroTitle =
    shortTitles[scenario?.id || ""] || scenario?.title || "See what changed.";
  const connected = data.connection === "live";
  const capacityFull =
    !!data.capacity && data.capacity.used >= data.capacity.max;
  const canRun = connected && !!scenario && !busy && !running && !capacityFull;
  const comparisonMode = opened?.data.mode === "comparison" || !selected;
  useEffect(() => {
    const readHash = () =>
      setSelectedId(new URLSearchParams(location.hash.slice(1)).get("case"));
    window.addEventListener("hashchange", readHash);
    return () => window.removeEventListener("hashchange", readHash);
  }, []);
  useEffect(() => {
    const hash = selectedId ? `#case=${encodeURIComponent(selectedId)}` : "";
    if (location.hash !== hash)
      history.replaceState(
        null,
        "",
        location.pathname + location.search + hash,
      );
  }, [selectedId]);
  useEffect(() => {
    if (selected?.scenario_id) setScenarioId(selected.scenario_id);
  }, [selected?.scenario_id]);
  useEffect(() => {
    if (
      !data.scenarios.some((s) => s.id === scenarioId) &&
      data.scenarios.length
    )
      setScenarioId(data.scenarios[0].id);
  }, [data.scenarios, scenarioId]);
  useEffect(() => {
    if (!follow || !selected) return;
    setStep(
      stopped || complete || selected.status === "done"
        ? 5
        : runs.some((r) => r.image)
          ? 2
          : 1,
    );
  }, [selected?.status, runs.length, follow, !!stopped, !!complete]);
  useEffect(() => {
    if (mode !== "wall" || !data.cases.length) return;
    const timer = setInterval(() => {
      setSelectedId(
        (id) =>
          data.cases[
            (data.cases.findIndex((c) => c.id === id) + 1) % data.cases.length
          ].id,
      );
      setStep(2);
    }, 14000);
    return () => clearInterval(timer);
  }, [
    mode,
    data.cases
      .map((c) => c.id)
      .sort()
      .join(","),
  ]);
  useEffect(() => {
    const f = () => {
      if (!document.fullscreenElement && mode === "wall") setMode("story");
    };
    document.addEventListener("fullscreenchange", f);
    return () => document.removeEventListener("fullscreenchange", f);
  }, [mode]);
  const start = async (investigate = false, fresh = false) => {
    if (!scenario || busy || running) return;
    setBusy(true);
    setLaunchError("");
    try {
      const seed = fresh
        ? (crypto.getRandomValues(new Uint32Array(1))[0] % 1000000) + 1
        : 0;
      const id = await data.runScenario(
        scenario.id,
        investigate
          ? { mode: "investigation" }
          : { mode: "comparison", view_seed: seed },
      );
      setSelectedId(id);
      setFollow(true);
      setStep(1);
      setMode("story");
    } catch (error) {
      setLaunchError(
        error instanceof Error ? error.message : "The run could not start.",
      );
    } finally {
      setBusy(false);
    }
  };
  const showCase = (c: CaseDetail) => {
    setSelectedId(c.id);
    setScenarioId(c.scenario_id || scenarioId);
    setMode("story");
    setStep(2);
    setFollow(false);
  };
  const go = (i: number) => {
    setStep(i);
    setFollow(false);
  };
  const report =
    opened?.data.report ||
    scenario?.report ||
    "Choose a test. Romeo renders the scene and keeps the measurements.";
  const setScenario = (id: string) => {
    setScenarioId(id);
    setSelectedId(null);
    setStep(0);
    setFollow(true);
    setLaunchError("");
  };

  return (
    <div className={"app " + (mode === "wall" ? "wall" : "")}>
      <header className="topbar">
        <a
          className="brand"
          href="#"
          onClick={(e) => {
            e.preventDefault();
            setMode("story");
          }}
        >
          <span>r.</span>ROMEO
        </a>
        <span className="project-name">THE EVIDENCE, AS IT HAPPENS</span>
        <nav aria-label="View">
          <button
            className={mode === "story" ? "selected" : ""}
            onClick={() => setMode("story")}
          >
            Story
          </button>
          <button
            className={mode === "board" ? "selected" : ""}
            onClick={() => setMode("board")}
          >
            Cases <span>{data.cases.length}</span>
          </button>
          <button
            className={mode === "wall" ? "selected" : ""}
            onClick={() => {
              if (mode === "wall") {
                setMode("story");
                if (document.fullscreenElement) void document.exitFullscreen();
              } else {
                setMode("wall");
                if (!selectedId && data.cases.length)
                  setSelectedId(data.cases[0].id);
                void document.documentElement
                  .requestFullscreen?.()
                  .catch(() => {});
              }
            }}
          >
            {mode === "wall" ? "Exit wall" : "Wall ↗"}
          </button>
        </nav>
        <div className={"connection " + (connected ? "online" : "")}>
          <i />
          {connected
            ? "Connected"
            : data.connection === "connecting"
              ? "Connecting"
              : "Reconnecting"}
        </div>
      </header>
      {!connected && (
        <div className="connection-note" role="status">
          Connecting to Romeo.{" "}
          {data.error ||
            "Measurements will appear when the service is available."}{" "}
          <button onClick={() => void data.refresh()}>Retry</button>
        </div>
      )}
      {(launchError || (connected && data.error)) && (
        <div className="error-note" role="alert">
          {launchError || data.error}
        </div>
      )}
      {mode === "board" ? (
        <main className="board">
          <div className="section-heading">
            <div>
              <span className="eyebrow">Every run keeps its own record</span>
              <h1>The case board.</h1>
            </div>
            <button className="primary" onClick={() => setMode("story")}>
              Choose a test ↗
            </button>
          </div>
          <div className="case-grid">
            {data.cases.map((c) => (
              <button
                key={c.id}
                className="case-card"
                onClick={() => showCase(c)}
              >
                <span className="mono">{c.id}</span>
                <h2>{c.title}</h2>
                <p>{setupNames[c.setup] || c.setup}</p>
                <span className={"status " + c.status}>{statusName(c)}</span>
                <p className="waiting">
                  {c.waiting_for ||
                    (c.status === "done"
                      ? "Open the measurements →"
                      : "Open the case →")}
                </p>
              </button>
            ))}
          </div>
          {!data.cases.length && (
            <Empty
              icon="+"
              title="The first run starts here."
              text="Choose a renderer test in Story. Its images and measurements will appear on this board."
            />
          )}
        </main>
      ) : (
        <>
          <section className="intro">
            <div>
              <span className="eyebrow">
                RENDERER CASE STUDY <span className="slash">/</span>{" "}
                {scenario?.id?.replace("case-", "") || "—"}
              </span>
              <h1>{heroTitle}</h1>
              <p>Run it. Compare it. See the evidence.</p>
            </div>
            <div className="test-picker">
              <label htmlFor="scenario">Choose a test</label>
              <select
                id="scenario"
                value={scenarioId}
                onChange={(e) => setScenario(e.target.value)}
                disabled={busy}
              >
                {!data.scenarios.length && (
                  <option value="case-07">Waiting for available tests</option>
                )}
                {data.scenarios.map((s) => (
                  <option key={s.id} value={s.id}>
                    {s.title}
                  </option>
                ))}
              </select>
              <div className="run-row">
                <span className="mono">
                  {setupNames[scenario?.setup || ""] ||
                    "Renderer connection required"}
                </span>
                <button
                  className="primary"
                  disabled={!canRun}
                  onClick={() => void start()}
                >
                  {busy ? "Starting…" : running ? "Test running…" : "Run test"}{" "}
                  <span aria-hidden="true">↗</span>
                </button>
              </div>
              {capacityFull && (
                <small>
                  Case storage is full. Export runs before an administrator
                  archives them.
                </small>
              )}
            </div>
          </section>
          <main className="story-shell">
            <aside className="story-rail">
              <div className="rail-label">FOLLOW THE CASE</div>
              {steps.map((s, i) => (
                <button
                  key={s}
                  className={step === i ? "active" : ""}
                  aria-current={step === i ? "step" : undefined}
                  onClick={() => go(i)}
                >
                  <span>{String(i + 1).padStart(2, "0")}</span>
                  <b>{s}</b>
                  <i aria-hidden="true">↗</i>
                </button>
              ))}
              <div className={"run-state " + (running ? "is-running" : "")}>
                <i />
                <div>
                  <b>{statusName(selected)}</b>
                  <span>{selected ? selected.id : "No measurements yet"}</span>
                </div>
              </div>
            </aside>
            <section className="story-stage" aria-label={steps[step]}>
              <div className="stage-top">
                <span className="eyebrow">
                  {String(step + 1).padStart(2, "0")}{" "}
                  <span className="slash">/</span> {steps[step]}
                </span>
                {selected && (
                  <span className="mono">
                    {opened?.data.mode === "comparison"
                      ? "MEASURED COMPARISON"
                      : "LIVE INVESTIGATION"}
                    {opened?.data.mode === "comparison" &&
                    opened?.data.view_seed
                      ? ` · VIEW SEED ${opened.data.view_seed}`
                      : ""}
                  </span>
                )}
              </div>
              {step === 0 && (
                <div className="report-scene">
                  <div className="report-text">
                    <span className="large-quote">“</span>
                    <h2>{report}</h2>
                    <div className="report-meta">
                      <span className="dot" />
                      {scenario
                        ? setupNames[scenario.setup]
                        : "Ready when you are"}
                    </div>
                    <p>
                      {comparisonMode
                        ? "Same scene. Same machine. One issue switched on."
                        : "Follow the evidence from this investigation."}
                    </p>
                    <button
                      className="text-button"
                      disabled={!canRun}
                      onClick={() => void start()}
                    >
                      Let’s see it happen <span>→</span>
                    </button>
                  </div>
                  <RenderImage
                    src={affected?.image || baseline?.image}
                    label={sourceLabel(
                      affected || baseline,
                      affected || baseline
                        ? "Output from this run"
                        : "The renderer will create the picture",
                    )}
                  />
                </div>
              )}
              {step === 1 && (
                <div className="run-scene">
                  <div className="run-pipeline">
                    {[
                      "Render with the issue",
                      "Render the baseline",
                      "Replay 40 frames",
                    ].map((label, i) => (
                      <div
                        key={label}
                        className={
                          (
                            i === 0
                              ? successful(current)
                              : i === 1
                                ? successful(reference)
                                : successful(current) &&
                                  successful(reference) &&
                                  affected?.preview
                          )
                            ? "ready"
                            : running
                              ? "pending"
                              : ""
                        }
                      >
                        <span>
                          {(
                            i === 0
                              ? successful(current)
                              : i === 1
                                ? successful(reference)
                                : successful(current) &&
                                  successful(reference) &&
                                  affected?.preview
                          )
                            ? "✓"
                            : String(i + 1).padStart(2, "0")}
                        </span>
                        <h3>{label}</h3>
                        <p>
                          {i === 0
                            ? "Capture the reported behavior."
                            : i === 1
                              ? "Keep the scene and settings matched."
                              : "Inspect frames from the measured runs."}
                        </p>
                      </div>
                    ))}
                  </div>
                  <div className="run-message" role="status">
                    <i className={running ? "pulse" : ""} />
                    <div>
                      <h2>
                        {busy
                          ? "Starting the renderer…"
                          : running
                            ? "The renderer is working."
                            : selected
                              ? statusName(selected) + "."
                              : "Ready for a real run."}
                      </h2>
                      <p>
                        {selected?.waiting_for ||
                          "Images and measurements arrive when each render finishes."}
                      </p>
                    </div>
                  </div>
                  {restarted && (
                    <div className="restart-note">
                      ↻ Romeo restarted. This run’s saved evidence was
                      recovered.
                    </div>
                  )}
                  {stopped && (
                    <p className="error-note">
                      {stopped.data.missing || stopped.data.reason}
                    </p>
                  )}
                </div>
              )}
              {step === 2 && (
                <div className="compare-scene">
                  <div className="scene-heading">
                    <h2>
                      {comparisonMode
                        ? "Same scene. Two results."
                        : "The latest render evidence."}
                    </h2>
                    <label className="toggle">
                      <input
                        type="checkbox"
                        checked={detail}
                        onChange={(e) => setDetail(e.target.checked)}
                      />
                      Zoom into the texture
                    </label>
                  </div>
                  <div className="image-pair">
                    <RenderImage
                      src={affected?.image}
                      label={sourceLabel(affected, "With the issue")}
                      detail={detail}
                    />
                    <RenderImage
                      src={baseline?.image}
                      label={sourceLabel(baseline, "Baseline")}
                      detail={detail}
                    />
                  </div>
                  <div className="metric-pair">
                    {[current, reference].map((r, i) => (
                      <div key={i} className={i ? "baseline-metrics" : ""}>
                        {r?.event.data.source === "fixture" && (
                          <span className="fixture-label">FIXTURE</span>
                        )}
                        <Metric
                          label="Typical frame"
                          value={r?.summary?.p50_ms}
                          unit="ms"
                        />
                        <Metric
                          label="Slowest frame"
                          value={r?.summary?.max_ms}
                          unit="ms"
                        />
                        <Metric
                          label="Memory at end"
                          value={
                            typeof r?.summary?.resident_end_bytes === "number"
                              ? r.summary.resident_end_bytes / 1048576
                              : undefined
                          }
                          unit="MiB"
                        />
                      </div>
                    ))}
                  </div>
                  <div className="chart-tabs">
                    <button
                      className={!memory ? "selected" : ""}
                      onClick={() => setMemory(false)}
                    >
                      Frame times
                    </button>
                    <button
                      className={memory ? "selected" : ""}
                      onClick={() => setMemory(true)}
                    >
                      Memory
                    </button>
                    <span>
                      40 warm-up frames excluded from timing statistics
                    </span>
                  </div>
                  <Chart
                    runs={[current, reference].filter(
                      (r): r is Run =>
                        !!r &&
                        (!current ||
                          r.event.data.setup === current.event.data.setup),
                    )}
                    memory={memory}
                  />
                </div>
              )}
              {step === 3 && (
                <div>
                  <div className="scene-heading">
                    <div>
                      <h2>Replay what the renderer saw.</h2>
                      <p>
                        40 sampled frames. Every picture comes from this run.
                      </p>
                    </div>
                    <button
                      disabled={!canRun}
                      onClick={() => void start(false, true)}
                    >
                      Test new angles ↗
                    </button>
                  </div>
                  <div className="chart-tabs gallery-tabs">
                    <button
                      className={gallerySide === "current" ? "selected" : ""}
                      onClick={() => setGallerySide("current")}
                    >
                      With the issue
                    </button>
                    <button
                      className={gallerySide === "baseline" ? "selected" : ""}
                      onClick={() => setGallerySide("baseline")}
                    >
                      Baseline
                    </button>
                  </div>
                  <PreviewGallery
                    run={gallerySide === "current" ? affected : baseline}
                    caseId={selected?.id}
                  />
                  <p className="small-note">
                    Frames are re-rendered after timing is recorded. “Test new
                    angles” runs a fresh comparison with a different camera
                    path; it does not establish what a model saw in training.
                  </p>
                </div>
              )}
              {step === 4 && (
                <>
                  <Decisions events={events} />
                  <div className="decision-footer">
                    <span>
                      Diagnostic choices come from the model’s actual response.
                    </span>
                    <button disabled={!canRun} onClick={() => void start(true)}>
                      Ask Romeo to investigate ↗
                    </button>
                  </div>
                </>
              )}
              {step === 5 && (
                <div className="knowledge-scene">
                  <div className="knowledge-main">
                    <span className="eyebrow">
                      {stopped
                        ? "MORE EVIDENCE NEEDED"
                        : complete
                          ? "THE COMPARISON IS SAVED"
                          : conclusion
                            ? "WHAT ROMEO FOUND"
                            : "LET THE RUN TELL US"}
                    </span>
                    <h2>
                      {stopped
                        ? stopped.data.reason || "More evidence needed."
                        : complete
                          ? "Now we can compare."
                          : conclusion
                            ? conclusion.data.diagnosis
                            : "Evidence comes first."}
                    </h2>
                    <p>
                      {stopped
                        ? stopped.data.missing || stopped.data.reason
                        : conclusion?.data.fix ||
                          complete?.data.summary ||
                          "Start a test to replace the empty views with real measurements."}
                    </p>
                    {(affected || baseline) && (
                      <div className="mini-comparison">
                        <RenderImage
                          src={affected?.image}
                          label={sourceLabel(affected, "With the issue")}
                        />
                        <RenderImage
                          src={baseline?.image}
                          label={sourceLabel(baseline, "Baseline")}
                        />
                      </div>
                    )}
                    <button className="text-button" onClick={() => go(2)}>
                      Look at the measurements →
                    </button>
                  </div>
                  <div className="knowledge-notes">
                    <div>
                      <span>01</span>
                      <h3>
                        {runs.some(successful) ? "Measured here" : "Test setup"}
                      </h3>
                      <p>
                        {selected
                          ? `${setupNames[selected.setup] || selected.setup}. ${runs.some(successful) ? "Results belong to this machine and this run." : "No completed measurements yet."}`
                          : "The image, timing and memory will come from your renderer."}
                      </p>
                    </div>
                    <div>
                      <span>02</span>
                      <h3>
                        {comparisonMode
                          ? "A controlled comparison"
                          : "An investigation"}
                      </h3>
                      <p>
                        {comparisonMode
                          ? "The issue is deliberately enabled. The baseline uses the same renderer without that issue."
                          : "Inspect the saved evidence and verification checks before accepting a proposed fix."}
                      </p>
                    </div>
                    <div>
                      <span>03</span>
                      <h3>What it doesn’t prove</h3>
                      <p>
                        {stopped
                          ? "Missing evidence prevents a conclusion."
                          : "A comparison does not prove a code fix, or performance on every scene."}
                      </p>
                    </div>
                  </div>
                </div>
              )}
              <div className="stage-bottom">
                <span>
                  {facts.length
                    ? `${facts.length} pieces of evidence saved`
                    : selected
                      ? `${events.length} events saved`
                      : "Real output appears after you run a test."}
                </span>
                <div>
                  <button
                    disabled={step === 0}
                    aria-label="Previous story step"
                    onClick={() => go(step - 1)}
                  >
                    ←
                  </button>
                  <span>
                    {step + 1} / {steps.length}
                  </span>
                  <button
                    disabled={step === steps.length - 1}
                    aria-label="Next story step"
                    onClick={() => go(step + 1)}
                  >
                    →
                  </button>
                </div>
              </div>
            </section>
          </main>
          <Evidence c={selected} runs={runs} />
        </>
      )}
      <footer>
        <span>
          ROMEO <span className="slash">/</span> REAL RENDERS. SAVED EVIDENCE.
        </span>
        <span>
          {data.system?.gpu_available === true &&
          typeof data.system.gpu_util_pct === "number"
            ? `GPU ${number(data.system.gpu_util_pct, 0)}% · `
            : ""}
          {connected ? "Live event stream" : "Waiting for the service"}
          {selected ? ` · ${selected.id}` : ""}
        </span>
      </footer>
    </div>
  );
}

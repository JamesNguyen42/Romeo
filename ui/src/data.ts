import { useCallback, useEffect, useRef, useState } from "react";

export type ConnectionState =
  | "connecting"
  | "live"
  | "reconnecting"
  | "offline";
export type CaseStatus = "investigating" | "waiting" | "stopped" | "done";
export type RunMode = "comparison" | "investigation";
export interface RunOptions {
  mode: RunMode;
  view_seed?: number;
}
export interface Scenario {
  id: string;
  title: string;
  report: string;
  setup: string;
  difficulty?: string;
}
export interface CaseEvent {
  seq: number;
  case: string;
  t_ms: number;
  type: string;
  data: Record<string, any>;
}
export interface CaseSummary {
  id: string;
  title: string;
  setup: string;
  status: CaseStatus;
  waiting_for: string;
  updated_ms: number;
  event_count: number;
  scenario_id?: string;
  mode?: RunMode;
  view_seed?: number;
}
export interface CaseDetail extends CaseSummary {
  events: CaseEvent[];
  report?: string;
  difficulty?: string;
}
export interface Capacity {
  used: number;
  max: number;
}
export interface SystemSnapshot {
  gpu_available?: boolean;
  gpu_sample_ms?: number;
  gpu_util_pct?: number;
  gpu_power_w?: number;
  gpu_temp_c?: number;
  gpu_clock_mhz?: number;
  mem_total_gb?: number;
  mem_used_gb?: number;
  running_jobs?: number;
  t_ms?: number;
  sampled_at: number;
  cases?: Record<string, number>;
  clef?: { decisions: number; mean_ms: number };
  ornith?: { up: boolean; [key: string]: unknown };
}

function object(value: unknown): value is Record<string, any> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function isEvent(value: unknown): value is CaseEvent {
  return (
    object(value) &&
    Number.isSafeInteger(value.seq) &&
    value.seq >= 0 &&
    typeof value.case === "string" &&
    value.case.length > 0 &&
    Number.isFinite(value.t_ms) &&
    typeof value.type === "string" &&
    object(value.data)
  );
}

function isSummary(value: unknown): value is CaseSummary {
  return (
    object(value) &&
    typeof value.id === "string" &&
    typeof value.title === "string" &&
    typeof value.setup === "string" &&
    ["investigating", "waiting", "stopped", "done"].includes(value.status) &&
    typeof value.waiting_for === "string" &&
    Number.isFinite(value.updated_ms) &&
    Number.isFinite(value.event_count)
  );
}

export function mergeEvents(
  ...groups: ReadonlyArray<readonly CaseEvent[]>
): CaseEvent[] {
  const unique = new Map<string, CaseEvent>();
  for (const group of groups)
    for (const event of group) {
      unique.set(`${event.case}:${event.seq}`, event);
    }
  return [...unique.values()].sort(
    (a, b) => a.seq - b.seq || a.case.localeCompare(b.case),
  );
}

// Apply the same state transitions as the service, so an older snapshot cannot
// erase an event received while that snapshot was being downloaded.
export function deriveCase(
  summary: CaseSummary,
  events: readonly CaseEvent[],
): CaseDetail {
  const result: CaseDetail = { ...summary, events: [...events] };
  const pending = new Map<string, string>();
  for (const event of events) {
    const d = event.data;
    if (event.type === "case_opened") {
      result.status = "investigating";
      result.waiting_for = "";
      for (const key of [
        "title",
        "setup",
        "report",
        "difficulty",
        "scenario_id",
      ] as const) {
        if (typeof d[key] === "string") result[key] = d[key];
      }
      if (d.mode === "comparison" || d.mode === "investigation")
        result.mode = d.mode;
      if (Number.isSafeInteger(d.view_seed)) result.view_seed = d.view_seed;
    } else if (event.type === "action_requested") {
      const label =
        typeof d.label === "string" ? d.label : String(d.kind ?? "a result");
      if (typeof d.action_id === "string") pending.set(d.action_id, label);
      result.status = "waiting";
      result.waiting_for = label;
    } else if (event.type === "action_result") {
      if (typeof d.action_id === "string") pending.delete(d.action_id);
      if (pending.size === 0 && result.status === "waiting") {
        result.status = "investigating";
        result.waiting_for = "";
      }
    } else if (event.type === "stopped") {
      result.status = "stopped";
    } else if (event.type === "done" || event.type === "comparison_complete") {
      result.status = "done";
      result.waiting_for = "";
    }
    result.updated_ms = Math.max(result.updated_ms, event.t_ms);
  }
  result.event_count = Math.max(result.event_count, events.length);
  return result;
}

export function artifactUrl(caseId: string, path: unknown): string | null {
  if (typeof path !== "string" || !path || !caseId) return null;
  let relative = path;
  if (relative.startsWith("/api/artifacts/")) {
    const prefix = `/api/artifacts/${encodeURIComponent(caseId)}/`;
    if (!relative.startsWith(prefix)) return null;
    relative = relative.slice(prefix.length);
  }
  if (/[:?#\\]/.test(relative) || relative.startsWith("/")) return null;
  const segments = relative.split("/");
  if (segments.length < 2) return null;
  try {
    const decoded = segments.map((segment) => decodeURIComponent(segment));
    if (
      decoded.some(
        (segment) =>
          !segment ||
          segment === "." ||
          segment === ".." ||
          /[/\\\x00-\x1f]/.test(segment),
      )
    )
      return null;
    return `/api/artifacts/${encodeURIComponent(caseId)}/${decoded.map(encodeURIComponent).join("/")}`;
  } catch {
    return null;
  }
}

// Renderer CSV files use a plain numeric format; missing values remain missing.
export function parseCsv(text: string): Array<Record<string, number>> {
  const lines = text.trim().split(/\r?\n/);
  if (lines.length < 2) return [];
  const headings = lines[0]
    .replace(/^\uFEFF/, "")
    .split(",")
    .map((value) => value.trim());
  const rows: Array<Record<string, number>> = [];
  for (const line of lines.slice(1)) {
    if (!line.trim()) continue;
    const row: Record<string, number> = {};
    line.split(",").forEach((cell, index) => {
      const value = Number(cell);
      if (headings[index] && cell.trim() !== "" && Number.isFinite(value))
        row[headings[index]] = value;
    });
    if (Object.keys(row).length) rows.push(row);
  }
  return rows;
}

async function jsonRequest(
  path: string,
  init: RequestInit = {},
): Promise<unknown> {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 20_000);
  const onAbort = () => controller.abort();
  if (init.signal?.aborted) controller.abort();
  else init.signal?.addEventListener("abort", onAbort, { once: true });
  try {
    const response = await fetch(path, {
      ...init,
      signal: controller.signal,
      cache: "no-store",
    });
    const text = await response.text();
    let value: unknown;
    try {
      value = JSON.parse(text);
    } catch {
      throw new Error(
        `Romeo returned an unreadable response (${response.status}).`,
      );
    }
    if (!response.ok)
      throw new Error(
        object(value) && typeof value.error === "string"
          ? value.error
          : `Romeo returned ${response.status}.`,
      );
    return value;
  } finally {
    clearTimeout(timeout);
    init.signal?.removeEventListener("abort", onAbort);
  }
}

function errorText(error: unknown): string {
  if (error instanceof Error && error.name === "AbortError")
    return "Romeo did not respond in time. Check the service and retry.";
  return error instanceof Error ? error.message : "Could not reach Romeo.";
}

async function caseDetails(
  summaries: CaseSummary[],
  signal: AbortSignal,
): Promise<CaseDetail[]> {
  const result: CaseDetail[] = [];
  let next = 0;
  // Leave connection slots available for jobs, the event stream and image assets.
  await Promise.all(
    Array.from({ length: Math.min(4, summaries.length) }, async () => {
      while (next < summaries.length) {
        const summary = summaries[next++];
        const raw = await jsonRequest(
          `/api/cases/${encodeURIComponent(summary.id)}`,
          { signal },
        );
        if (!object(raw) || !Array.isArray(raw.events) || !isSummary(raw))
          throw new Error(`Case ${summary.id} could not be read.`);
        const detail = raw as CaseSummary & { events: unknown[] };
        const events = detail.events
          .filter(isEvent)
          .filter((event) => event.case === summary.id);
        if (events.length !== detail.events.length)
          throw new Error(`Case ${summary.id} contains an invalid event.`);
        result.push(deriveCase(raw, mergeEvents(events)));
      }
    }),
  );
  return result;
}

export function useRomeo() {
  const [connection, setConnection] = useState<ConnectionState>("connecting");
  const [cases, setCases] = useState<CaseDetail[]>([]);
  const [scenarios, setScenarios] = useState<Scenario[]>([]);
  const [capacity, setCapacity] = useState<Capacity | null>(null);
  const [system, setSystem] = useState<SystemSnapshot | null>(null);
  const [error, setError] = useState<string | null>(null);
  const records = useRef(new Map<string, CaseDetail>());
  const refreshController = useRef<AbortController | null>(null);
  const lifetimeController = useRef<AbortController | null>(null);
  const mounted = useRef(false);

  const publish = useCallback(() => {
    if (!mounted.current) return;
    setCases(
      [...records.current.values()].sort(
        (a, b) => b.updated_ms - a.updated_ms || a.id.localeCompare(b.id),
      ),
    );
    setCapacity((current) =>
      current ? { ...current, used: records.current.size } : current,
    );
  }, []);

  const refresh = useCallback(async () => {
    refreshController.current?.abort();
    const controller = new AbortController();
    refreshController.current = controller;
    const before = new Map(records.current);
    try {
      const [summaries, catalog, metrics] = await Promise.all([
        jsonRequest("/api/cases", { signal: controller.signal }),
        jsonRequest("/api/scenarios", { signal: controller.signal }),
        jsonRequest("/api/system", { signal: controller.signal }),
      ]);
      if (!Array.isArray(summaries) || !summaries.every(isSummary))
        throw new Error("Romeo returned an invalid case list.");
      if (
        !object(catalog) ||
        !Array.isArray(catalog.scenarios) ||
        !catalog.scenarios.every(
          (item: unknown) =>
            object(item) &&
            ["id", "title", "report", "setup"].every(
              (key) => typeof item[key] === "string",
            ),
        )
      )
        throw new Error("Romeo returned an invalid test list.");
      const details = await caseDetails(summaries, controller.signal);
      if (controller.signal.aborted || !mounted.current) return;
      const next = new Map<string, CaseDetail>();
      for (const detail of details) {
        const current = records.current.get(detail.id);
        next.set(
          detail.id,
          deriveCase(detail, mergeEvents(detail.events, current?.events ?? [])),
        );
      }
      // A case opened after the list request belongs to the new snapshot too.
      for (const [id, current] of records.current) {
        if (!next.has(id) && current !== before.get(id)) next.set(id, current);
      }
      records.current = next;
      setScenarios(catalog.scenarios);
      if (
        object(catalog.capacity) &&
        Number.isFinite(catalog.capacity.used) &&
        Number.isFinite(catalog.capacity.max)
      ) {
        setCapacity({ used: next.size, max: catalog.capacity.max });
      }
      if (object(metrics))
        setSystem((current) =>
          !current || Number(metrics.t_ms) >= Number(current.t_ms)
            ? ({ ...metrics, sampled_at: Date.now() } as SystemSnapshot)
            : current,
        );
      setError(null);
      publish();
    } catch (failure) {
      if (!controller.signal.aborted && mounted.current)
        setError(errorText(failure));
    }
  }, [publish]);

  useEffect(() => {
    mounted.current = true;
    lifetimeController.current = new AbortController();
    let opened = false;
    const stream = new EventSource("/api/stream");
    stream.onopen = () => {
      opened = true;
      setConnection("live");
      // Subscribe first, then reconcile the snapshot. This closes the gap while
      // connecting and recovers events missed during a network interruption.
      void refresh();
    };
    stream.onerror = () => {
      setConnection(opened ? "reconnecting" : "offline");
    };
    stream.onmessage = (message) => {
      try {
        const event: unknown = JSON.parse(message.data);
        if (!isEvent(event))
          throw new Error("Romeo sent an unreadable case event.");
        const current = records.current.get(event.case);
        const summary: CaseSummary = current ?? {
          id: event.case,
          title: event.case,
          setup: "",
          status: "investigating",
          waiting_for: "",
          updated_ms: event.t_ms,
          event_count: 0,
        };
        records.current.set(
          event.case,
          deriveCase(summary, mergeEvents(current?.events ?? [], [event])),
        );
        publish();
      } catch (failure) {
        setError(errorText(failure));
      }
    };
    stream.addEventListener("system", (message) => {
      try {
        const value: unknown = JSON.parse((message as MessageEvent).data);
        if (object(value))
          setSystem({ ...value, sampled_at: Date.now() } as SystemSnapshot);
      } catch {
        /* A later telemetry sample can recover without losing case events. */
      }
    });
    return () => {
      mounted.current = false;
      stream.close();
      refreshController.current?.abort();
      lifetimeController.current?.abort();
    };
  }, [publish, refresh]);

  const runScenario = useCallback(
    async (id: string, options: RunOptions): Promise<string> => {
      try {
        const raw = await jsonRequest(
          `/api/scenarios/${encodeURIComponent(id)}/runs`,
          {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(options),
            signal: lifetimeController.current?.signal,
          },
        );
        if (!isSummary(raw))
          throw new Error("Romeo did not return a run identifier.");
        if (mounted.current) {
          const current = records.current.get(raw.id);
          records.current.set(raw.id, deriveCase(raw, current?.events ?? []));
          setError(null);
          publish();
        }
        return raw.id;
      } catch (failure) {
        const message = errorText(failure);
        if (mounted.current) setError(message);
        throw new Error(message);
      }
    },
    [publish],
  );

  return {
    connection,
    cases,
    scenarios,
    capacity,
    system,
    error,
    refresh,
    runScenario,
  };
}

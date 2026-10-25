import assert from "node:assert/strict";
import test from "node:test";
import { artifactUrl, deriveCase, mergeEvents, parseCsv } from "./data.ts";
import type { CaseEvent, CaseSummary } from "./data.ts";

const summary: CaseSummary = {
  id: "run-1",
  title: "A renderer run",
  setup: "B",
  status: "waiting",
  waiting_for: "rendering",
  updated_ms: 20,
  event_count: 2,
};
function event(
  seq: number,
  type: string,
  data: Record<string, unknown> = {},
): CaseEvent {
  return { seq, type, data, case: "run-1", t_ms: seq * 10 };
}

test("a reconnect snapshot cannot erase completion already received over SSE", () => {
  const opened = event(1, "case_opened", {
    title: "Spikes",
    setup: "B",
    mode: "comparison",
    view_seed: 7,
  });
  const request = event(2, "action_requested", {
    action_id: "a1",
    label: "Rendering",
  });
  const result = event(3, "action_result", { action_id: "a1" });
  const done = event(4, "comparison_complete", { success: true });
  const merged = mergeEvents([opened, request], [request, result, done]);
  const recovered = deriveCase(summary, merged);
  assert.equal(recovered.status, "done");
  assert.equal(recovered.waiting_for, "");
  assert.equal(recovered.updated_ms, 40);
  assert.equal(recovered.events.length, 4);
  assert.equal(recovered.mode, "comparison");
  assert.equal(recovered.view_seed, 7);
});

test("one completed action does not hide another pending result", () => {
  const recovered = deriveCase(summary, [
    event(1, "case_opened"),
    event(2, "action_requested", { action_id: "a1", label: "First image" }),
    event(3, "action_requested", { action_id: "a2", label: "Baseline image" }),
    event(4, "action_result", { action_id: "a1" }),
  ]);
  assert.equal(recovered.status, "waiting");
  assert.equal(recovered.waiting_for, "Baseline image");
});

test("case and sequence together identify an event", () => {
  assert.equal(
    mergeEvents(
      [event(1, "case_opened")],
      [{ ...event(1, "case_opened"), case: "run-2" }],
    ).length,
    2,
  );
});

test("artifact URLs cannot escape the current case path", () => {
  assert.equal(
    artifactUrl("run-1", "action-1/image.png"),
    "/api/artifacts/run-1/action-1/image.png",
  );
  assert.equal(
    artifactUrl("run-1", "/api/artifacts/run-1/action-1/image.png"),
    "/api/artifacts/run-1/action-1/image.png",
  );
  for (const path of [
    "https://example.com/a.png",
    "/api/artifacts/other/a/image.png",
    "../image.png",
    "action/%2e%2e/image.png",
    "action/%2fimage.png",
    "action\\image.png",
    "action/image.png?redirect=yes",
  ]) {
    assert.equal(artifactUrl("run-1", path), null, path);
  }
});

test("missing CSV measurements never become zero", () => {
  assert.deepEqual(parseCsv("frame,ms,memory\n0,21.4,\n1,NaN,42\n"), [
    { frame: 0, ms: 21.4 },
    { frame: 1, memory: 42 },
  ]);
});

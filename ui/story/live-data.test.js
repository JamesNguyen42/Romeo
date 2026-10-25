import assert from 'node:assert/strict';
import test from 'node:test';
import { LiveStore, liveApply, liveArtifactUrl, liveMergeEvents, liveParseCsv } from './live-data.js';

const scenario = { id: 'case-07', title: 'Soft image', report: 'Textures look soft.', setup: 'C' };
const event = (seq, type, data = {}, id = 'run-1') => ({ seq, case: id, t_ms: seq * 10, type, data });
const summary = (id = 'run-1', status = 'waiting', count = 2) => ({
  id, title: 'Soft image', setup: 'C', status, waiting_for: status === 'waiting' ? 'Rendering' : '',
  updated_ms: count * 10, event_count: count, mode: 'comparison', scenario_id: 'case-07', view_seed: 0,
});
const opened = event(1, 'case_opened', { title: 'Soft image', setup: 'C', mode: 'comparison', scenario_id: 'case-07' });
const requested = event(2, 'action_requested', { action_id: 'a1', label: 'Rendering' });
const result = event(3, 'action_result', { action_id: 'a1', source: 'run', baseline: false, metrics: { p50_ms: 21.5 }, artifacts: ['a1/image.png', 'a1/summary.json'] });
const complete = event(4, 'comparison_complete', { success: true });
const json = (value, status = 200) => new Response(JSON.stringify(value), { status, headers: { 'Content-Type': 'application/json' } });
const settle = async () => { for (let i = 0; i < 5; i++) await new Promise(resolve => setTimeout(resolve, 0)); };

function harness(route) {
  const calls = [];
  class Stream {
    constructor(url) { this.url = url; harness.stream = this; }
    open() { return this.onopen?.(); }
    message(value) { return this.onmessage?.({ data: JSON.stringify(value) }); }
    close() { this.closed = true; }
  }
  const store = new LiveStore(() => {}, {
    location: { protocol: 'http:', origin: 'http://romeo.test' }, EventSource: Stream,
    fetch: async (url, options = {}) => {
      const path = new URL(url).pathname;
      calls.push({ path, method: options.method || 'GET', body: options.body });
      if (path === '/api/scenarios') return json({ scenarios: [scenario], capacity: { used: 1, max: 64 } });
      return route(path, options);
    },
  });
  return { store, calls, get stream() { return harness.stream; } };
}

test('opening the page subscribes and reads lists, without selecting or running a case', async () => {
  const h = harness(path => path === '/api/cases' ? json([summary('run-1', 'done', 4)]) : json({ error: 'unexpected request' }, 500));
  h.store.start();
  assert.equal(h.calls.length, 0);
  assert.equal(h.stream.url, 'http://romeo.test/api/stream');
  await h.stream.open();
  assert.equal(h.store.state.ready, true);
  assert.equal(h.store.state.selectedId, '');
  assert.equal(h.store.state.selected, null);
  assert.deepEqual(h.store.state.runs, []);
  assert.deepEqual(h.calls.map(call => call.path).sort(), ['/api/cases', '/api/scenarios']);
  assert.ok(h.calls.every(call => call.method === 'GET'));
  h.store.stop();
  assert.equal(h.stream.closed, true);
});

test('file URLs do not contact a default server', () => {
  let called = false;
  const store = new LiveStore(() => {}, {
    location: { protocol: 'file:', origin: 'null' },
    fetch: () => { called = true; }, EventSource: class { constructor() { called = true; } },
  });
  store.start();
  assert.equal(called, false);
  assert.equal(store.state.selectedId, '');
  assert.match(store.state.error, /http:\/\/127\.0\.0\.1:18790/);
  store.stop();
});

test('reconnection merges a stale selected snapshot with newer stream events', async () => {
  const h = harness(path => {
    if (path === '/api/cases') return json([summary()]);
    if (path === '/api/cases/run-1') return json({ ...summary(), events: [opened, requested] });
    return json({ p50_ms: 21.5 });
  });
  h.store.start();
  await h.stream.open();
  await h.store.select('run-1');
  h.stream.message(result);
  h.stream.message(complete);
  h.stream.message(complete);
  assert.equal(h.store.state.selected.status, 'done');
  await h.stream.open();
  assert.equal(h.store.state.selected.status, 'done');
  assert.equal(h.store.state.selected.events.length, 4);
  assert.equal(h.store.state.selected.event_count, 4);
  h.store.stop();
});

test('old partial history never overrides a newer case summary', () => {
  assert.equal(liveApply(summary('run-1', 'done', 4), [opened, requested]).status, 'done');
});

test('artifact fetches are cached and persisted metrics survive a missing summary file', async () => {
  const h = harness(path => {
    if (path === '/api/cases') return json([summary('run-1', 'done', 4)]);
    if (path === '/api/cases/run-1') return json({ ...summary('run-1', 'done', 4), events: [opened, requested, result, complete] });
    return json({ error: 'missing summary' }, 404);
  });
  h.store.start();
  await h.stream.open();
  await h.store.select('run-1');
  await settle();
  assert.equal(h.store.state.runs[0].summary.p50_ms, 21.5);
  assert.equal(h.store.state.runs[0].image, 'http://romeo.test/api/artifacts/run-1/a1/image.png');
  assert.match(h.store.state.runs[0].error, /summary\.json/);
  h.stream.message(event(5, 'fact', { text: 'Evidence saved.' }));
  await h.store.select('run-1');
  await settle();
  assert.equal(h.calls.filter(call => call.path.endsWith('/summary.json')).length, 1);
  assert.equal(h.store.state.runs.length, 1);
  h.store.stop();
});

test('switching cases aborts old artifact loads without exposing their images', async () => {
  let aborted = false;
  const h = harness((path, options) => {
    if (path === '/api/cases') return json([summary(), summary('run-2', 'done', 1)]);
    if (path === '/api/cases/run-1') return json({ ...summary('run-1', 'done', 4), events: [opened, requested, result, complete] });
    if (path === '/api/cases/run-2') return json({ ...summary('run-2', 'done', 1), events: [event(1, 'done', {}, 'run-2')] });
    return new Promise((resolve, reject) => options.signal.addEventListener('abort', () => {
      aborted = true;
      reject(new DOMException('Aborted', 'AbortError'));
    }, { once: true }));
  });
  h.store.start();
  await h.stream.open();
  await h.store.select('run-1');
  await Promise.resolve();
  await h.store.select('run-2');
  await settle();
  assert.equal(aborted, true);
  assert.equal(h.store.state.selectedId, 'run-2');
  assert.deepEqual(h.store.state.runs, []);
  h.store.stop();
});

test('every emitted state keeps run artifacts attached to a selected case', async () => {
  let listed = true;
  const h = harness(path => {
    if (path === '/api/cases') return json(listed ? [summary('run-1', 'done', 4)] : []);
    if (path === '/api/cases/run-1') return listed
      ? json({ ...summary('run-1', 'done', 4), events: [opened, requested, result, complete] })
      : json({ error: 'no such case' }, 404);
    return json({ p50_ms: 21.5 });
  });
  const emissions = [];
  h.store.onState = state => emissions.push(state);
  h.store.start();
  await h.stream.open();
  await h.store.select('run-1');
  await settle();
  await h.store.select('run-1');
  listed = false;
  await h.store.refresh();
  assert.ok(emissions.every(state => state.selected || !state.runs.length));
  h.store.stop();
});

test('only an explicit run call sends POST and selects its returned run', async () => {
  const h = harness((path, options) => {
    if (path === '/api/cases') return json([]);
    if (path === '/api/scenarios/case-07/runs') {
      assert.deepEqual(JSON.parse(options.body), { mode: 'comparison', view_seed: 123 });
      return json(summary('run-new', 'investigating', 1));
    }
    if (path === '/api/cases/run-new') return json({ ...summary('run-new', 'investigating', 1), events: [{ ...opened, case: 'run-new' }] });
    return json({ error: 'unexpected request' }, 500);
  });
  h.store.start();
  await h.stream.open();
  const id = await h.store.run('case-07', 'comparison', 123);
  assert.equal(id, 'run-new');
  assert.equal(h.store.state.selectedId, id);
  assert.equal(h.store.state.starting, false);
  assert.equal(h.calls.filter(call => call.method === 'POST').length, 1);
  h.store.stop();
});

test('artifact paths stay inside the selected case and missing CSV values stay missing', () => {
  for (const path of ['https://evil.test/p.png', '../image.png', 'a/%2e%2e/image.png', '/api/artifacts/other/a/image.png']) {
    assert.equal(liveArtifactUrl('run-1', path), null);
  }
  assert.deepEqual(liveParseCsv('frame,ms\n0,\n1,24.5\n2,NaN'), [{ frame: 0 }, { frame: 1, ms: 24.5 }, { frame: 2 }]);
  assert.equal(liveMergeEvents([opened], [opened], [{ ...opened, case: 'run-2' }]).length, 2);
});

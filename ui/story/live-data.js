export function liveObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

export function liveMergeEvents(...groups) {
  const seen = new Map();
  for (const events of groups) for (const event of events || []) {
    if (liveObject(event) && typeof event.case === 'string' && Number.isSafeInteger(event.seq)
      && Number.isFinite(event.t_ms) && typeof event.type === 'string' && liveObject(event.data)) {
      seen.set(`${event.case}:${event.seq}`, event);
    }
  }
  return [...seen.values()].sort((a, b) => a.seq - b.seq || a.case.localeCompare(b.case));
}

export function liveArtifactUrl(caseId, path, base = '') {
  if (typeof path !== 'string' || !path || typeof caseId !== 'string' || !caseId) return null;
  let relative = path;
  if (relative.startsWith('/api/artifacts/')) {
    const prefix = `/api/artifacts/${encodeURIComponent(caseId)}/`;
    if (!relative.startsWith(prefix)) return null;
    relative = relative.slice(prefix.length);
  }
  if (/[:?#\\]/.test(relative) || relative.startsWith('/')) return null;
  try {
    const parts = relative.split('/').map(decodeURIComponent);
    if (parts.length < 2 || parts.some(part => !part || part === '.' || part === '..' || /[/\\\x00-\x1f]/.test(part))) return null;
    return `${base}/api/artifacts/${encodeURIComponent(caseId)}/${parts.map(encodeURIComponent).join('/')}`;
  } catch { return null; }
}

export function liveParseCsv(text) {
  const lines = text.trim().split(/\r?\n/);
  if (lines.length < 2) return [];
  const keys = lines[0].replace(/^\uFEFF/, '').split(',').map(key => key.trim());
  return lines.slice(1).filter(line => line.trim()).map(line => {
    const row = {};
    line.split(',').forEach((cell, i) => {
      const value = Number(cell);
      if (keys[i] && cell.trim() && Number.isFinite(value)) row[keys[i]] = value;
    });
    return row;
  }).filter(row => Object.keys(row).length);
}

export function liveSummary(value) {
  return liveObject(value) && typeof value.id === 'string' && typeof value.title === 'string'
    && typeof value.setup === 'string' && ['investigating', 'waiting', 'stopped', 'done'].includes(value.status)
    && typeof value.waiting_for === 'string' && Number.isSafeInteger(value.event_count)
    && Number.isFinite(value.updated_ms);
}

export function liveApply(summary, events) {
  const next = { ...summary };
  const pending = new Map();
  for (const event of events) {
    const d = event.data;
    if (event.type === 'action_requested') pending.set(d.action_id, d.label || d.kind || 'a result');
    if (event.type === 'action_result') pending.delete(d.action_id);
    // Summaries already include their event_count events. Replaying older partial
    // history would incorrectly turn a finished case back into an active one.
    if (event.seq <= summary.event_count) continue;
    if (event.type === 'case_opened') {
      Object.assign(next, { status: 'investigating', waiting_for: '' });
      for (const key of ['title', 'setup', 'scenario_id', 'mode', 'view_seed']) {
        if (d[key] !== undefined) next[key] = d[key];
      }
    } else if (event.type === 'action_requested') {
      next.status = 'waiting';
      next.waiting_for = d.label || d.kind || 'a result';
    } else if (event.type === 'action_result' && !pending.size && next.status === 'waiting') {
      next.status = 'investigating';
      next.waiting_for = '';
    } else if (event.type === 'stopped') next.status = 'stopped';
    else if (event.type === 'done' || event.type === 'comparison_complete') {
      next.status = 'done';
      next.waiting_for = '';
    }
    next.event_count = Math.max(next.event_count, event.seq);
    next.updated_ms = Math.max(next.updated_ms, event.t_ms);
  }
  return next;
}

export class LiveStore {
  constructor(onState, environment = {}) {
    this.onState = onState;
    this.fetch = environment.fetch || globalThis.fetch.bind(globalThis);
    this.EventSource = environment.EventSource || globalThis.EventSource;
    const location = environment.location || globalThis.location;
    this.base = environment.apiBase !== undefined ? environment.apiBase.replace(/\/$/, '')
      : location && /^https?:$/.test(location.protocol) ? location.origin : null;
    this.state = {
      connected: false, ready: false, error: '', scenarios: [], cases: [],
      selectedId: '', selected: null, runs: [], loading: false, starting: false,
    };
    this.records = new Map();
    this.runCache = new Map();
    this.runPending = new Map();
    this.disposed = false;
    this.selection = 0;
    this.refreshVersion = 0;
    this.lifetime = new AbortController();
  }

  patch(values) {
    if (this.disposed) return;
    this.state = { ...this.state, ...values };
    this.onState?.(this.state);
  }

  publish() {
    if (this.disposed) return;
    const cases = [...this.records.values()].map(record => record.summary)
      .sort((a, b) => b.updated_ms - a.updated_ms || a.id.localeCompare(b.id));
    const record = this.records.get(this.state.selectedId);
    this.patch({ cases, selected: record ? { ...record.summary, events: record.events } : null,
      ...(record ? {} : { runs: [] }) });
    this.loadRuns();
  }

  async request(path, options = {}) {
    if (this.base === null) throw new Error('Start Juliet, then open http://127.0.0.1:18790.');
    const controller = new AbortController();
    const signal = options.signal || this.lifetime.signal;
    const abort = () => controller.abort();
    if (signal.aborted) controller.abort();
    else signal.addEventListener('abort', abort, { once: true });
    const timer = setTimeout(abort, 20000);
    try {
      const response = await this.fetch(this.base + path, { ...options, signal: controller.signal, cache: 'no-store' });
      const text = await response.text();
      let value;
      try { value = JSON.parse(text); } catch { throw new Error(`Juliet returned an unreadable response (${response.status}).`); }
      if (!response.ok) throw new Error(value?.error || `Juliet returned ${response.status}.`);
      return value;
    } finally {
      clearTimeout(timer);
      signal.removeEventListener('abort', abort);
    }
  }

  start() {
    if (this.stream || this.disposed) return;
    if (this.base === null) {
      this.patch({ error: 'Start Juliet, then open http://127.0.0.1:18790.' });
      return;
    }
    try {
      this.stream = new this.EventSource(this.base + '/api/stream');
      this.stream.onopen = () => {
        this.patch({ connected: true });
        return this.refresh();
      };
      this.stream.onerror = () => this.patch({ connected: false, error: 'Connection lost. Waiting for Juliet to reconnect.' });
      this.stream.onmessage = message => {
        try {
          const raw = JSON.parse(message.data);
          const events = liveMergeEvents([raw]);
          if (events.length !== 1) throw new Error('Juliet sent an unreadable event.');
          const event = events[0];
          const old = this.records.get(event.case) || { summary: {
            id: event.case, title: event.case, setup: '', status: 'investigating',
            waiting_for: '', updated_ms: event.t_ms, event_count: 0,
          }, events: [], loaded: false };
          const merged = liveMergeEvents(old.events, [event]);
          this.records.set(event.case, { ...old, summary: liveApply(old.summary, merged), events: merged });
          this.publish();
        } catch (error) { this.patch({ error: error.message || 'Could not read a case event.' }); }
      };
    } catch (error) { this.patch({ connected: false, error: error.message || 'Could not connect to Juliet.' }); }
  }

  stop() {
    this.disposed = true;
    this.stream?.close();
    this.stream = null;
    this.lifetime.abort();
    this.snapshotController?.abort();
    this.detailController?.abort();
    this.cancelArtifacts();
  }

  async refresh() {
    if (this.disposed) return;
    this.snapshotController?.abort();
    const controller = this.snapshotController = new AbortController();
    const version = ++this.refreshVersion;
    const before = new Map(this.records);
    try {
      const [summaries, catalog] = await Promise.all([
        this.request('/api/cases', { signal: controller.signal }),
        this.request('/api/scenarios', { signal: controller.signal }),
      ]);
      if (!Array.isArray(summaries) || !summaries.every(liveSummary)) throw new Error('Juliet returned an invalid case list.');
      if (!liveObject(catalog) || !Array.isArray(catalog.scenarios) || !catalog.scenarios.every(item =>
        liveObject(item) && ['id', 'title', 'report', 'setup'].every(key => typeof item[key] === 'string')))
        throw new Error('Juliet returned an invalid test list.');
      if (this.disposed || controller.signal.aborted || version !== this.refreshVersion) return;
      const next = new Map();
      for (const summary of summaries) {
        const old = this.records.get(summary.id);
        const events = old?.events || [];
        next.set(summary.id, { summary: liveApply(summary, events), events, loaded: !!old?.loaded && events.length >= summary.event_count });
      }
      for (const [id, current] of this.records) if (!next.has(id) && current !== before.get(id)) next.set(id, current);
      this.records = next;
      this.patch({ scenarios: catalog.scenarios, ready: true, error: '', capacity: catalog.capacity || null });
      this.publish();
      if (this.state.selectedId) await this.loadSelected(this.state.selectedId, this.selection);
    } catch (error) {
      if (!controller.signal.aborted && !this.disposed) this.patch({ error: error.name === 'AbortError' ? 'Juliet did not respond in time.' : error.message });
    }
  }

  async select(id) {
    if (this.disposed) return;
    const changed = id !== this.state.selectedId;
    if (changed) {
      ++this.selection;
      this.detailController?.abort();
      this.cancelArtifacts();
    }
    this.patch({ selectedId: id || '', selected: changed ? null : this.state.selected,
      runs: changed ? [] : this.state.runs, error: '' });
    this.publish();
    if (id) await this.loadSelected(id, this.selection);
    else this.patch({ loading: false });
  }

  async loadSelected(id, selection) {
    this.detailController?.abort();
    const controller = this.detailController = new AbortController();
    this.patch({ loading: true });
    try {
      const raw = await this.request(`/api/cases/${encodeURIComponent(id)}`, { signal: controller.signal });
      if (!liveSummary(raw) || !Array.isArray(raw.events)) throw new Error('This case could not be read.');
      const snapshot = liveMergeEvents(raw.events).filter(event => event.case === id);
      if (snapshot.length !== raw.events.length) throw new Error('This case contains an unreadable event.');
      if (this.disposed || controller.signal.aborted || selection !== this.selection || this.state.selectedId !== id) return;
      const events = liveMergeEvents(snapshot, this.records.get(id)?.events || []);
      this.records.set(id, { summary: liveApply(raw, events), events, loaded: true });
      this.patch({ loading: false, error: '' });
      this.publish();
    } catch (error) {
      if (!controller.signal.aborted && !this.disposed && selection === this.selection)
        this.patch({ loading: false, error: error.name === 'AbortError' ? 'Juliet did not respond in time.' : error.message });
    }
  }

  async run(scenarioId, mode = 'comparison', view_seed = 0) {
    if (this.state.starting) throw new Error('A run is already being started.');
    this.patch({ starting: true, error: '' });
    try {
      const summary = await this.request(`/api/scenarios/${encodeURIComponent(scenarioId)}/runs`, {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ mode, view_seed }),
      });
      if (!liveSummary(summary)) throw new Error('Juliet did not return a run identifier.');
      if (this.disposed) return summary.id;
      const old = this.records.get(summary.id);
      this.records.set(summary.id, { summary: liveApply(summary, old?.events || []), events: old?.events || [], loaded: false });
      await this.select(summary.id);
      return summary.id;
    } catch (error) {
      const message = error.name === 'AbortError' ? 'The request timed out. Check saved cases before starting another run.' : error.message;
      this.patch({ error: message });
      throw new Error(message);
    } finally { this.patch({ starting: false }); }
  }

  cancelArtifacts() {
    for (const pending of this.runPending.values()) pending.controller.abort();
    this.runPending.clear();
  }

  loadRuns() {
    const selected = this.state.selected;
    if (!selected) { if (this.state.runs.length) this.patch({ runs: [] }); return; }
    const results = selected.events.filter(event => event.type === 'action_result' && Array.isArray(event.data.artifacts));
    const runs = results.map(event => {
      const key = `${selected.id}:${event.seq}`;
      const cached = this.runCache.get(key);
      if (cached) return cached;
      const files = event.data.artifacts.map(path => liveArtifactUrl(selected.id, path, this.base || '')).filter(Boolean);
      const run = { event, baseline: event.data.baseline === true, summary: liveObject(event.data.metrics) ? event.data.metrics : undefined,
        frames: [], memory: [], preview: undefined, image: files.find(path => path.endsWith('/image.png')),
        files, error: '' };
      if (!this.runPending.has(key)) this.fetchRun(key, run, selected.id, this.selection);
      return run;
    });
    this.patch({ runs });
  }

  async fetchRun(key, run, caseId, selection) {
    const controller = new AbortController();
    this.runPending.set(key, { controller });
    try {
      await Promise.resolve();
      if (this.disposed || controller.signal.aborted) return;
      for (const [suffix, field] of [['summary.json', 'summary'], ['frames.csv', 'frames'], ['memory.csv', 'memory'], ['preview.json', 'preview']]) {
        const url = run.files.find(path => path.endsWith('/' + suffix));
        if (!url) continue;
        try {
          const text = await this.readArtifact(url, controller.signal);
          const value = suffix.endsWith('.csv') ? liveParseCsv(text) : JSON.parse(text);
          if (!suffix.endsWith('.csv') && !liveObject(value)) throw new Error(`${suffix} has an invalid format.`);
          run[field] = value;
        } catch (error) {
          if (controller.signal.aborted) return;
          run.error += (run.error ? ' ' : '') + (error.message || `Could not read ${suffix}.`);
        }
      }
      if (this.disposed || controller.signal.aborted || selection !== this.selection || caseId !== this.state.selectedId) return;
      this.runCache.set(key, run);
    } finally {
      if (this.runPending.get(key)?.controller === controller) this.runPending.delete(key);
      if (!this.disposed && !controller.signal.aborted && selection === this.selection && caseId === this.state.selectedId) this.loadRuns();
    }
  }

  async readArtifact(url, signal) {
    const controller = new AbortController();
    const abort = () => controller.abort();
    signal.addEventListener('abort', abort, { once: true });
    if (signal.aborted) controller.abort();
    const timer = setTimeout(abort, 20000);
    try {
      const response = await this.fetch(url, { signal: controller.signal, cache: 'no-store' });
      if (!response.ok) throw new Error(`Could not read ${url.split('/').pop()}.`);
      return await response.text();
    } finally {
      clearTimeout(timer);
      signal.removeEventListener('abort', abort);
    }
  }
}

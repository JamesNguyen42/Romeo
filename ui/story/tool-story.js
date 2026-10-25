// Presentation of recorded choices only. This module never requests or runs a tool.
const toolStoryCatalog = [
  ['measure_frames', 0, 'Measure frame times', 'Measure how long each frame takes.'],
  ['measure_memory', 0, 'Watch memory use', 'Check whether memory grows during a longer run.'],
  ['capture_trace', 0, 'Look inside the slowdown', 'Capture where threads spend their time.'],
  ['compare_baseline', 0, 'Compare the baseline', 'Check the earlier build on the same setup.'],
  ['try_smaller_setup', 0, 'Try fewer cameras', 'Check whether the problem depends on the setup.'],
  ['fetch_logs', 1, 'Read the error log', 'Read errors recorded by a run.'],
  ['check_disk', 1, 'Check disk use', 'Look at temporary files left by a run.'],
  ['compare_images', 1, 'Compare the pictures', 'Check differences between rendered images.'],
  ['search_code', 1, 'Search the source', 'Find code related to the evidence.'],
  ['recall_similar', 1, 'Find similar cases', 'Ask case memory about earlier investigations.'],
  ['conclude', 2, 'Explain the cause', 'Propose a cause and fix from the evidence.'],
  ['stop_need_more', 2, 'Say what is missing', 'Explain which evidence is still needed.'],
];
const toolStoryKnown = new Map(toolStoryCatalog.map(([code, group, name, help]) => [code, { code, group, name, help }]));
const toolStoryGroups = [
  { title: 'Test runs', note: 'Test-related tools' },
  { title: 'Evidence tools', note: 'Check saved evidence' },
  { title: 'Writing', note: 'Explain the findings or stop' },
  { title: 'Other project tools', note: 'Tools offered by this project' },
];
const toolStoryText = value => typeof value === 'string' ? value.trim() : '';
const toolStoryData = event => event?.data && typeof event.data === 'object' ? event.data : {};
const toolStoryName = (code, label) => toolStoryText(label) || toolStoryKnown.get(code)?.name || toolStoryText(code).replaceAll('_', ' ');
const toolStoryPercent = p => typeof p === 'number' && Number.isFinite(p) && p >= 0 && p <= 1 ? `${Number((p * 100).toFixed(2))}%` : '—';
const toolStoryShort = text => text.length > 140 ? `${text.slice(0, 137).trimEnd()}…` : text;
const toolStoryAccent = 'var(--color-accent)';
const toolStoryInk = 'var(--color-text)';
const toolStoryPaper = 'var(--color-bg)';

function toolStoryNotebook(events, report, highlightFrom = Infinity) {
  const rows = [];
  if (report) rows.push({ num: 'R', text: toolStoryShort(`Report: ${report}`), fullText: `Report: ${report}`, border: toolStoryInk, bg: toolStoryPaper });
  let number = 0;
  for (let i = 0; i < events.length; i++) {
    const event = events[i], text = toolStoryText(toolStoryData(event).text);
    if (event.type !== 'fact' || !text) continue;
    const fresh = i >= highlightFrom;
    rows.push({ num: String(++number), text: toolStoryShort(text), fullText: text, border: fresh ? toolStoryAccent : toolStoryInk, bg: fresh ? 'var(--color-accent-100)' : toolStoryPaper });
  }
  if (!rows.length) rows.push({ num: '—', text: 'Report and findings appear here.', fullText: 'Report and findings appear here.', border: toolStoryInk, bg: toolStoryPaper });
  return rows;
}

function toolStoryCard(candidate, { reference = false, earlier = false, chosen = false } = {}) {
  const code = toolStoryText(candidate.action || candidate.code), known = toolStoryKnown.get(code);
  const pct = reference || earlier ? '—' : toolStoryPercent(candidate.p);
  return {
    code, name: toolStoryName(code, candidate.label),
    right: earlier ? 'Earlier step' : pct,
    pct, w: pct === '—' ? '0%' : `${candidate.p * 100}%`,
    bg: chosen ? toolStoryAccent : toolStoryPaper, fg: chosen ? '#fff' : toolStoryInk,
    border: chosen ? toolStoryAccent : earlier ? 'var(--color-neutral-400)' : toolStoryInk,
    opacity: earlier ? 0.45 : 1,
    track: chosen ? 'var(--color-accent-700)' : 'var(--color-neutral-200)',
    bar: chosen ? '#fff' : toolStoryInk,
    help: reference ? known?.help || 'A project tool.' : earlier ? 'Chosen in an earlier step. No score is shown for this round.' : `Offered in this recorded decision.${pct === '—' ? ' No valid probability was recorded.' : ` Recorded tool-choice probability: ${pct}.`}`,
  };
}

function toolStoryGroupCards(cards, reference = false) {
  return toolStoryGroups.map((group, index) => {
    const tools = cards.filter(card => (toolStoryKnown.get(card.code)?.group ?? 3) === index);
    return { ...group, note: reference ? 'Reference only · no recorded scores' : tools.length ? group.note : 'No options at this step', tools };
  }).filter((group, index) => index < 3 || group.tools.length);
}

function toolStoryResultState(data) {
  if (data.source === 'unavailable') return { label: 'Evidence unavailable', card: 'Unavailable', failed: true };
  if (data.source === 'timeout') return { label: 'Tool timed out', card: 'Timed out', failed: true };
  if (data.success === false || typeof data.exit_code === 'number' && data.exit_code !== 0) return { label: 'Tool returned a failure', card: 'Failed', failed: true };
  if (data.source === 'fixture') return { label: 'Fixture result recorded', card: 'Fixture', failed: false };
  return { label: 'Result recorded', card: 'Result saved', failed: false };
}

export function buildToolStory({ selected = null, roundIndex = null, progress = 1 } = {}) {
  const events = (Array.isArray(selected?.events) ? selected.events : []).filter(event => event && typeof event === 'object')
    .map((event, index) => ({ event, index })).sort((a, b) => (Number.isFinite(a.event.seq) && Number.isFinite(b.event.seq) ? a.event.seq - b.event.seq : 0) || a.index - b.index).map(item => item.event);
  const opened = events.find(event => event.type === 'case_opened');
  const report = toolStoryText(toolStoryData(opened).report) || toolStoryText(selected?.report);
  const decisions = events.map((event, index) => ({ event, index })).filter(item => item.event.type === 'decision');
  if (!decisions.length) {
    const stopped = [...events].reverse().find(event => event.type === 'stopped');
    const comparison = selected?.mode === 'comparison' || toolStoryData(opened).mode === 'comparison';
    const phaseLabel = !selected ? 'No decision yet' : comparison ? 'Direct comparison · no model call' : stopped ? 'Investigation stopped · no decision recorded' : 'Waiting for a recorded decision';
    return {
      roundCount: 0, roundIndex: 0, hasDecisions: false,
      groups: toolStoryGroupCards(toolStoryCatalog.map(([code]) => toolStoryCard({ code }, { reference: true })), true),
      notebook: toolStoryNotebook(events, report), roundLabel: 'Tool reference', phaseLabel, phaseBg: 'var(--color-neutral-700)',
      evidenceNote: stopped ? [toolStoryText(toolStoryData(stopped).reason), toolStoryText(toolStoryData(stopped).missing)].filter(Boolean).join(' ') : comparison ? 'This test follows a fixed comparison. No model probabilities were recorded.' : 'These cards explain the usual tools. They do not show which tools are currently available.',
      chosenName: '',
    };
  }

  const chosenRound = Number.isFinite(roundIndex) ? Math.max(0, Math.min(decisions.length - 1, Math.floor(roundIndex))) : decisions.length - 1;
  const { event: decision, index: decisionAt } = decisions[chosenRound], data = toolStoryData(decision);
  const end = decisions[chosenRound + 1]?.index ?? events.length;
  const timeline = events.slice(decisionAt + 1, end).map((event, offset) => ({ event, index: decisionAt + 1 + offset }))
    .filter(item => ['action_requested', 'action_result', 'fact', 'conclusion', 'stopped'].includes(item.event.type));
  const fraction = typeof progress === 'number' && Number.isFinite(progress) ? Math.max(0, Math.min(1, progress)) : 1;
  const visibleCount = Math.floor(fraction * timeline.length);
  const visibleEnd = visibleCount ? timeline[visibleCount - 1].index + 1 : decisionAt + 1;
  const visible = events.slice(0, visibleEnd), after = visible.slice(decisionAt + 1);
  const candidates = (Array.isArray(data.candidates) ? data.candidates : []).filter(candidate => candidate && toolStoryText(candidate.action));
  const chosenCode = toolStoryText(data.chosen), chosenCandidate = candidates.find(candidate => candidate.action === chosenCode);
  const chosenName = toolStoryName(chosenCode, chosenCandidate?.label);
  const nextRequest = after.find(event => event.type === 'action_requested');
  const request = toolStoryData(nextRequest).kind === chosenCode ? nextRequest : null;
  const actionId = toolStoryText(toolStoryData(request).action_id);
  const result = actionId ? after.find(event => event.type === 'action_result' && toolStoryData(event).action_id === actionId) : null;
  const stopped = [...after].reverse().find(event => event.type === 'stopped');
  const conclusion = chosenCode === 'conclude' ? after.find(event => event.type === 'conclusion') : null;
  let phaseLabel = chosenName ? `Picked: ${chosenName}` : 'No choice recorded';
  if (chosenCode === 'conclude') phaseLabel = 'Waiting for a conclusion';
  if (chosenCode === 'stop_need_more') phaseLabel = 'Waiting for the missing-evidence note';
  if (request) phaseLabel = 'Requested · waiting for result';
  if (result) {
    const status = toolStoryResultState(toolStoryData(result));
    phaseLabel = status.label;
    const factAfter = after.some(event => event.type === 'fact' && after.indexOf(event) > after.indexOf(result));
    if (!status.failed && toolStoryData(result).source !== 'fixture' && factAfter) phaseLabel = 'Result recorded · notebook updated';
  }
  if (conclusion) phaseLabel = toolStoryData(conclusion).supported === false ? 'Conclusion recorded · more evidence needed' : 'Conclusion recorded · proposed fix';
  if (stopped) phaseLabel = 'Investigation stopped';

  const earlier = new Map();
  for (const previous of decisions.slice(0, chosenRound)) {
    const prior = toolStoryData(previous.event), code = toolStoryText(prior.chosen);
    if (!code || candidates.some(candidate => candidate.action === code)) continue;
    const candidate = (Array.isArray(prior.candidates) ? prior.candidates : []).find(value => value?.action === code);
    earlier.set(code, { action: code, label: candidate?.label });
  }
  const cards = candidates.map(candidate => toolStoryCard(candidate, { chosen: candidate.action === chosenCode }))
    .concat([...earlier.values()].map(candidate => toolStoryCard(candidate, { earlier: true })));
  const model = toolStoryText(data.model);
  const timing = typeof data.inference_ms === 'number' && Number.isFinite(data.inference_ms) && data.inference_ms >= 0 ? `${data.inference_ms} ms` : '';
  const recordedBy = [model, timing].filter(Boolean).join(' · ');
  const resultText = toolStoryText(toolStoryData(result).summary);
  const stoppedText = stopped ? [toolStoryText(toolStoryData(stopped).reason), toolStoryText(toolStoryData(stopped).missing)].filter(Boolean).join(' ') : '';
  const evidenceNote = stoppedText || resultText || (conclusion ? [toolStoryText(toolStoryData(conclusion).diagnosis), toolStoryText(toolStoryData(conclusion).fix)].filter(Boolean).join(' ') : '') || `${recordedBy ? `Recorded ${recordedBy}. ` : ''}Scores describe this tool choice, not confidence in a picture.`;
  return {
    roundCount: decisions.length, roundIndex: chosenRound, hasDecisions: true,
    groups: toolStoryGroupCards(cards), notebook: toolStoryNotebook(visible, report, decisionAt + 1),
    roundLabel: `${model || 'Recorded decision'} · Round ${chosenRound + 1} of ${decisions.length}`,
    phaseLabel, phaseBg: toolStoryAccent, evidenceNote, chosenName,
  };
}

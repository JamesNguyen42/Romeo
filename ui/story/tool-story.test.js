import assert from 'node:assert/strict';
import test from 'node:test';
import { buildToolStory } from './tool-story.js';

const event = (seq, type, data = {}) => ({ seq, type, data });
const decision = (seq, chosen, candidates) => event(seq, 'decision', { chosen, candidates, model: 'clef-flash', inference_ms: 12.5 });
const candidate = (action, p, label = action) => ({ action, p, label });
const selected = events => ({ id: 'run-1', mode: 'investigation', report: 'The image looks soft.', events });
const cards = story => story.groups.flatMap(group => group.tools);
const card = (story, code) => cards(story).find(tool => tool.code === code);

test('recorded rounds retain actual probabilities and the chosen tool even when it is not the largest score', () => {
  const caseData = selected([
    decision(1, 'compare_images', [candidate('measure_frames', 0.8), candidate('compare_images', 0.2)]),
    event(2, 'action_requested', { kind: 'compare_images', action_id: 'images' }),
    event(3, 'action_result', { action_id: 'images', source: 'run', summary: 'Images differ.' }),
    event(4, 'fact', { text: 'Images differ.' }),
    decision(5, 'conclude', [candidate('conclude', 0.75), candidate('stop_need_more', 0.25)]),
  ]);
  const early = buildToolStory({ selected: caseData, roundIndex: 0, progress: 0 });
  const late = buildToolStory({ selected: caseData, roundIndex: 0, progress: 1 });
  assert.equal(early.roundCount, 2);
  assert.equal(early.chosenName, 'compare_images');
  assert.equal(card(early, 'compare_images').pct, '20%');
  assert.equal(card(early, 'compare_images').right, '20%');
  assert.equal(card(late, 'compare_images').right, '20%');
  assert.equal(card(early, 'compare_images').w, card(late, 'compare_images').w);
  assert.notEqual(card(early, 'compare_images').bg, card(early, 'measure_frames').bg);
  assert.equal(buildToolStory({ selected: caseData }).roundIndex, 1);
});

test('playback reveals only recorded events and facts in their sequence', () => {
  const caseData = selected([
    event(1, 'fact', { text: 'Existing evidence.' }),
    decision(2, 'measure_frames', [candidate('measure_frames', 1)]),
    event(3, 'action_requested', { kind: 'measure_frames', action_id: 'a', job: true }),
    event(4, 'action_result', { action_id: 'a', source: 'run', exit_code: 0 }),
    event(5, 'fact', { text: 'Measured 24 ms.' }),
  ]);
  const at = progress => buildToolStory({ selected: caseData, progress });
  assert.match(at(0).phaseLabel, /^Picked:/);
  assert.equal(at(0).notebook.length, 2);
  assert.equal(at(1 / 3).phaseLabel, 'Requested · waiting for result');
  assert.equal(at(2 / 3).phaseLabel, 'Result recorded');
  assert.equal(at(2 / 3).notebook.length, 2);
  assert.equal(at(1).phaseLabel, 'Result recorded · notebook updated');
  assert.deepEqual(at(1).notebook.map(row => row.num), ['R', '1', '2']);
  assert.equal(at(1).notebook[2].fullText, 'Measured 24 ms.');
});

test('missing and invalid probabilities stay blank; a real zero remains zero', () => {
  const story = buildToolStory({ selected: selected([decision(1, 'fetch_logs', [candidate('fetch_logs', undefined), candidate('check_disk', 0), candidate('compare_images', null), candidate('search_code', 5), candidate('recall_similar', NaN)])]) });
  assert.equal(card(story, 'fetch_logs').pct, '—');
  assert.equal(card(story, 'check_disk').pct, '0%');
  for (const code of ['compare_images', 'search_code', 'recall_similar']) assert.equal(card(story, code).pct, '—');
  assert.equal(card(story, 'search_code').w, '0%');
});

test('earlier choices are context, and re-offered tools retain their current score', () => {
  const story = buildToolStory({ selected: selected([
    decision(1, 'fetch_logs', [candidate('fetch_logs', 1)]),
    event(2, 'action_requested', { kind: 'fetch_logs', action_id: 'a' }),
    event(3, 'action_result', { action_id: 'a', source: 'unavailable' }),
    decision(4, 'measure_frames', [candidate('measure_frames', 1)]),
    decision(5, 'measure_frames', [candidate('measure_frames', 0.7), candidate('stop_need_more', 0.3)]),
  ]) });
  assert.equal(card(story, 'fetch_logs').right, 'Earlier step');
  assert.equal(card(story, 'fetch_logs').pct, '—');
  assert.equal(card(story, 'measure_frames').pct, '70%');
  assert.equal(card(story, 'measure_frames').opacity, 1);
  assert.doesNotMatch(JSON.stringify(story), /After a run|After 3 facts|not available|Used/);
});

test('unknown tools keep their recorded labels in a separate project group', () => {
  const story = buildToolStory({ selected: selected([decision(1, 'custom_probe', [candidate('custom_probe', 1, 'Custom probe')])]) });
  assert.equal(story.groups[3].title, 'Other project tools');
  assert.equal(story.groups[3].tools[0].name, 'Custom probe');
  assert.equal(story.chosenName, 'Custom probe');
});

test('no-decision cases show a reference, never fabricated model scores', () => {
  const empty = buildToolStory();
  assert.equal(empty.phaseLabel, 'No decision yet');
  const comparison = buildToolStory({ selected: { mode: 'comparison', events: [event(1, 'comparison_complete', { success: true })] } });
  assert.equal(comparison.roundCount, 0);
  assert.equal(comparison.phaseLabel, 'Direct comparison · no model call');
  assert.ok(cards(comparison).every(tool => tool.right === '—' && tool.pct === '—' && tool.w === '0%'));
  assert.ok(comparison.groups.every(group => /Reference only/.test(group.note)));
  const stopped = buildToolStory({ selected: selected([event(1, 'stopped', { reason: 'Clef could not answer.', missing: 'Start Clef.' })]) });
  assert.match(stopped.phaseLabel, /no decision recorded/);
  assert.equal(stopped.evidenceNote, 'Clef could not answer. Start Clef.');
});

test('a pending request never becomes a completed run through playback alone', () => {
  const story = buildToolStory({ selected: selected([
    decision(1, 'measure_frames', [candidate('measure_frames', 1)]),
    event(2, 'action_requested', { kind: 'measure_frames', action_id: 'a', job: true, runner: 'script' }),
  ]), progress: 10 });
  assert.equal(story.phaseLabel, 'Requested · waiting for result');
  assert.equal(card(story, 'measure_frames').right, '100%');
  assert.doesNotMatch(JSON.stringify(story), /Done|Running|renderer process|Result saved/);
});

test('failure, timeout, unavailable and fixture outcomes retain their provenance after a fact arrives', () => {
  for (const [data, label] of [
    [{ source: 'run', exit_code: 2 }, 'Tool returned a failure'],
    [{ source: 'run', exit_code: 0, success: false }, 'Tool returned a failure'],
    [{ source: 'timeout' }, 'Tool timed out'],
    [{ source: 'unavailable' }, 'Evidence unavailable'],
    [{ source: 'fixture' }, 'Fixture result recorded'],
  ]) {
    const story = buildToolStory({ selected: selected([
      decision(1, 'measure_frames', [candidate('measure_frames', 1)]),
      event(2, 'action_requested', { kind: 'measure_frames', action_id: 'a' }),
      event(3, 'action_result', { action_id: 'a', ...data }),
      event(4, 'fact', { text: 'The recorded result.' }),
    ]) });
    assert.equal(story.phaseLabel, label);
    assert.equal(story.notebook.length, 2);
  }
});

test('results match the chosen action and automatic follow-up jobs are not presented as its choice', () => {
  const pending = buildToolStory({ selected: selected([
    decision(1, 'measure_frames', [candidate('measure_frames', 1)]),
    event(2, 'action_requested', { kind: 'capture_trace', action_id: 'other' }),
    event(3, 'action_result', { action_id: 'other', source: 'run' }),
    event(4, 'action_requested', { kind: 'measure_frames', action_id: 'later' }),
    event(5, 'action_result', { action_id: 'later', source: 'run' }),
  ]) });
  assert.match(pending.phaseLabel, /^Picked:/);
  const conclusion = buildToolStory({ selected: selected([
    decision(1, 'conclude', [candidate('conclude', 1)]),
    event(2, 'conclusion', { diagnosis: 'Lock held too long.', fix: 'Shorten the lock.', supported: true }),
    event(3, 'action_requested', { kind: 'verify_fix', action_id: 'automatic' }),
    event(4, 'action_result', { action_id: 'automatic', source: 'run', exit_code: 0 }),
  ]) });
  assert.equal(conclusion.phaseLabel, 'Conclusion recorded · proposed fix');
  assert.equal(conclusion.chosenName, 'conclude');
  assert.equal(cards(conclusion).length, 1);
  assert.equal(conclusion.evidenceNote, 'Lock held too long. Shorten the lock.');
});

test('model stop notes and unsupported conclusions do not imply a verified fix', () => {
  const story = buildToolStory({ selected: selected([
    decision(1, 'stop_need_more', [candidate('stop_need_more', 1)]),
    event(2, 'stopped', { reason: 'Cannot establish more.', missing: 'A trace is needed.' }),
  ]) });
  assert.equal(story.phaseLabel, 'Investigation stopped');
  assert.equal(card(story, 'stop_need_more').right, '100%');
  assert.equal(story.evidenceNote, 'Cannot establish more. A trace is needed.');
  const unsupported = buildToolStory({ selected: selected([
    decision(1, 'conclude', [candidate('conclude', 1)]),
    event(2, 'conclusion', { diagnosis: 'Unknown.', supported: false }),
  ]) });
  assert.equal(unsupported.phaseLabel, 'Conclusion recorded · more evidence needed');
});

test('empty groups and an empty notebook explain their absence without adding a fact', () => {
  const empty = buildToolStory();
  assert.deepEqual(empty.notebook.map(row => [row.num, row.text]), [['—', 'Report and findings appear here.']]);
  const story = buildToolStory({ selected: { events: [decision(1, 'conclude', [candidate('conclude', 1)])] } });
  assert.equal(story.groups[0].note, 'No options at this step');
  assert.equal(story.groups[1].note, 'No options at this step');
  assert.notEqual(story.groups[2].note, 'No options at this step');
  assert.equal(story.notebook[0].num, '—');
});

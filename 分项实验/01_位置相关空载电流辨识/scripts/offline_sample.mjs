import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(process.argv[2] ?? fileURLToPath(new URL('../verification/sample', import.meta.url)));
const dense = process.argv.includes('--dense');
const script = fileURLToPath(new URL('./analyze.mjs', import.meta.url));
for (const folder of ['manifest', 'data/raw']) fs.mkdirSync(path.join(root, folder), { recursive: true });
assert(!fs.existsSync(path.join(root, 'models', 'position_current.json')),
  'Use a fresh sample output directory');

const planFields = 'session_id,trial_id,phase,group,use,direction,target_mm,approach_mm,speed_mm_s';
const trialFields = planFields + ',start_mm,hold_s,window_s,period_s,pos_tol_mm,vel_tol_mm_s,status,reason';
const rawFields = 'session_id,trial_id,phase,group,use,stage,segment,direction,target_mm,speed_mm_s,' +
  't_s,read_s,gap_s,x_mm,v_mm_s,current_fb_raw,status,enabled,alarm,mode,planner,limit_pos,limit_neg,' +
  'temp_card_c,powered_s,rc_pos,rc_vel,rc_current,rc_status,rc_mode,rc_limit,rc_planner,rc_temp,valid';

function write(relative, header, rows) {
  fs.writeFileSync(path.join(root, relative), header + '\n' + rows.map(r => r.join(',')).join('\n') + '\n');
}

const positionCurrent = x => 180 + .21 * (x - 16) ** 2 + 2.2 * x;
function staticPlans(nodes, groups, use, speed = 2) {
  return Array.from({ length: groups }, (_, i) => nodes.flatMap(x => [-1, 1]
    .filter(d => (x !== 0 || d === -1) && (x !== 32 || d === 1))
    .map(d => ({ group: i + 1, x, d, approach: x - d, speed, use })))).flat();
}

function session(id, phase, plan, protocol) {
  const meta = { session_id: id, phase, config: 'synthetic-v1', R_state: 'fixed',
    current_api: 'GTN_GetAtlTorque', current_unit: 'raw', core: 2, axis: 1,
    pulse_per_mm: 1000, acc_mm_s2: dense ? phase === 'C' ? 400 : 20 : 100,
    position_mode: 2, loop_index: 1, segments: phase === 'A1' ? 3 : 1 };
  if (protocol) meta.B_protocol = protocol;
  fs.writeFileSync(path.join(root, 'manifest', id + '.txt'),
    Object.entries(meta).map(pair => pair.join('=')).join('\n') + '\n');
  const dt = phase === 'C' ? .005 : dense && phase === 'B' ? .0155 : .02;
  const plans = [], summaries = [], raw = [];
  let time = 0;
  for (const [i, t] of plan.entries()) {
    const base = [id, i + 1, phase, t.group, t.use];
    const tags = [t.d, t.x, t.speed];
    const planned = [...base, t.d, t.x, t.approach, t.speed];
    const start = t.error === 'same_position' ? t.x : t.approach;
    const duration = phase === 'A1' ? 1 : phase === 'B' || (phase === 'A2' && i !== 7) ||
      (dense && phase === 'D') ? 4 : 2;
    const endpoint = ['A0', 'B', 'C'].includes(phase) && (t.x === 0 || t.x === 32);
    const settlePosition = endpoint ? t.x + .003 * t.d : t.x;
    plans.push(planned);
    if (t.error === 'unstarted') continue;
    const status = t.error === 'abort' ? 'aborted' : 'completed';
    summaries.push([...planned, start, duration, 1, dt, .01, .05, status,
      status === 'aborted' ? 'operator_stop' : 'ok']);
    function sample(stage, x, v, segment = 0, invalid = false, moving = v !== 0) {
      time += dt;
      const load = Math.round(positionCurrent(x) + 8 * t.d + .7 * t.group +
        .5 * Math.sin(time * 12) + (phase === 'A1' ? .1 * time : 0));
      raw.push([...base, stage, segment, ...tags, time.toFixed(4), .001, dt, x, v,
        invalid ? 'nan' : load, moving ? 1536 : 512, 1, 0, 2, 0, 0, 0, 32 + time / 400,
        120 + time, 0, 0, invalid ? -1 : 0, 0, 0, 0, 0, 0, invalid ? 0 : 1]);
    }
    sample('approach_start', start, 0);
    if (phase === 'C') {
      const speed = t.speed, accel = meta.acc_mm_s2;
      const ramp = speed / accel, rampDistance = .5 * accel * ramp ** 2;
      const total = 2 * ramp + (32 - 2 * rampDistance) / speed;
      for (let s = 0; s < total; s += dt) {
        const near = s < ramp, far = s > total - ramp;
        const offset = near ? .5 * accel * s ** 2 : far ? 32 - .5 * accel * (total - s) ** 2 :
          rampDistance + speed * (s - ramp);
        const v = near ? accel * s : far ? accel * (total - s) : speed;
        const x = Math.round((start + t.d * offset) * 1000) / 1000;
        const spike = v > 0 && Math.round(s / dt) % 7 === 0;
        sample('scan', x, spike ? -t.d * 200 : t.d * v, 0, false, v > 0);
      }
      sample('scan', settlePosition, 0);
      sample('turnaround', settlePosition, 0, 1);
      continue;
    }
    if (t.d) sample('approach', settlePosition, 0);
    for (let segment = 1; segment <= Number(meta.segments); segment++) {
      const lastSample = Math.floor(duration / dt + 1e-9);
      for (let j = 0; j <= lastSample; j++) {
        if (t.error === 'abort' && j > 4) break;
        const moving = t.error === 'moving' && j > lastSample - 10;
        const spike = t.error === 'velocity_spike' && j === lastSample - 1;
        sample('hold', settlePosition, spike ? 20 : moving ? .2 : 0,
          segment, t.error === 'missing' && j === 90, moving);
      }
    }
  }
  write('manifest/' + id + '.plan.csv', planFields, plans);
  write('manifest/' + id + '.trials.csv', trialFields, summaries);
  write('data/raw/' + id + '.csv', rawFields, raw);
}

const a0 = staticPlans([0, 8, 16, 24, 32], 1, 'diagnostic');
session('A0', 'A0', a0);
session('A1', 'A1', [8, 24].map((x, i) => ({ group: i + 1, x, d: 0, approach: 'nan',
  speed: 2, use: 'diagnostic', error: undefined } )).map(t => ({ ...t, approach: t.x })));
const a2 = staticPlans([8, 24], 5, 'diagnostic');
a2[0].error = 'same_position';
a2[1].error = 'missing';
a2[2].error = 'moving';
a2[3].error = 'abort';
a2[4].error = 'unstarted';
a2[6].error = 'velocity_spike';
session('A2', 'A2', a2);
const b = dense ? staticPlans(Array.from({ length: 301 }, (_, i) => (1000 + 100 * i) / 1000), 2, 'train')
  : staticPlans(Array.from({ length: 17 }, (_, i) => 2 * i), 5, 'train');
b[0].error = 'velocity_spike';
b[1].error = 'velocity_spike';
session('B', 'B', b, dense ? 'B' : undefined);
if (dense) {
  for (const [id, low] of [['BL', true], ['BH', false]]) {
    const edge = Array.from({ length: 11 }, (_, i) => (low ? i : 310 + i) / 10);
    session(id, 'B', staticPlans(edge, 1, 'boundary_single').filter(row => row.d === (low ? -1 : 1)), id);
  }
  session('BE', 'B', staticPlans([.5, .6, .7, .8, .9, 1, 31, 31.1, 31.2, 31.3, 31.4, 31.5], 1,
    'boundary_compare').map(row => ({ ...row, approach: row.x - .5 * row.d })), 'BE');
}
const c = [];
for (let group = 1; group <= 5; group++) {
  const speeds = [2, 10, 40];
  for (let i = 0; i < 3; i++) {
    const speed = speeds[(i + group - 1) % 3];
    const use = group <= 3 ? 'development' : 'scan_holdout';
    c.push({ group, x: 32, d: 1, approach: 0, speed, use },
      { group, x: 0, d: -1, approach: 32, speed, use });
  }
}
session('C', 'C', c);
const nodes = [3, 7, 11, 15, 19, 23, 27, 29].map(x => dense ? x + .05 : x);
session('D1', 'D', staticPlans(nodes, 3, 'unseen'));
session('D2', 'D', staticPlans(nodes, 3, 'cross_time'));

function run(expected, ...args) {
  const result = spawnSync(process.execPath, [script, args[0], root, ...args.slice(1)], { encoding: 'utf8' });
  assert.equal(result.status, expected, result.stderr);
  if (expected === 0) process.stdout.write(result.stdout);
  return result.stderr;
}

run(0, 'diagnose', 'A0', 'A1', 'A2');
const results = name => fs.readFileSync(path.join(root, 'results', name), 'utf8');
const rejected = results('diagnostics_rejected.csv');
for (const reason of ['invalid_measured_approach', 'missing_invalid_or_unordered_samples',
  'unstable_or_gapped_window', 'aborted/operator_stop', 'missing_trial_summary']) assert.ok(rejected.includes(reason));
assert.equal(results('A1_windows.csv').trim().split('\n').length, 7);
assert.equal(results('A2_repeatability.csv').trim().split('\n').length, 5);
assert.ok(results('A2_trials.csv').split('\n').some(row => row.startsWith('A2,7,')));
const a2Windows = results('A2_trials.csv').trim().split('\n').slice(1).map(row => row.split(','));
assert.ok(a2Windows.filter(row => row[7] === '4').every(row =>
  row.slice(8, 11).every(value => value !== '' && Number.isFinite(Number(value))) && row[6] === row[9]));
assert.ok(a2Windows.filter(row => row[7] === '2').every(row => row.slice(8, 11).every(value => value === '')));
const a0rows = results('A0_scout.csv').trim().split('\n').slice(1);
assert.equal(a0rows.filter(row => row.startsWith('A0,0,-1,')).length, 1);
assert.equal(a0rows.filter(row => row.startsWith('A0,32,1,')).length, 1);
assert.equal(a0rows.length, 8);
if (dense) {
  run(0, 'summarize-b', 'B', 'BL', 'BH', 'BE');
  const lineCount = name => results(name).trim().split('\n').length - 1;
  assert.equal(lineCount('B.holds.csv'), 602);
  assert.equal(lineCount('BL.holds.csv'), 11);
  assert.equal(lineCount('BH.holds.csv'), 11);
  assert.equal(lineCount('BE.holds.csv'), 12);
  assert.equal(lineCount('B.boundary-overlap.csv'), 4);
}
run(0, 'freeze', 'B', 'C', ...(dense ? ['BL', 'BH', 'BE'] : []), '--quadratic');
const model = JSON.parse(fs.readFileSync(path.join(root, 'models/position_current.json'), 'utf8'));
assert.deepEqual(model.model.range, dense ? [1, 31] : [2, 30]);
assert.equal(model.training_pulses.length, dense ? 301 : 15);
assert.equal(model.model.type, 'quadratic');
const bRows = results('B_and_C_development.csv').trim().split('\n').filter(row => row.startsWith('B,'));
assert.equal(bRows.filter(row => row.includes(',single,')).length, dense ? 0 : 10);
assert.equal(bRows.filter(row => row.includes(',paired,')).length, dense ? 602 : 75);
assert.equal(model.evidence.filter(item => item.session === 'B').length, dense ? 2 : 5);
assert.ok(!model.evidence.some(item => ['BL', 'BH', 'BE'].includes(item.session)));
assert.equal(results('freeze_rejected.csv').trim().split('\n').length, 1);
const devScan = results('B_and_C_development.csv').trim().split('\n').slice(1)
  .filter(row => row.startsWith('C,')).map(row => row.split(','));
assert.equal(new Set(devScan.map(row => row[3])).size, 9);
assert.ok(devScan.every(row => Math.abs(Number(row[12]) - Number(row[5])) < 0.1 &&
  Math.abs(Number(row[13]) + Number(row[5])) < 0.1));
run(0, 'evaluate', 'D1', 'D2', 'C');
const rows = results('acceptance_metrics.csv').trim().split('\n').slice(1);
assert.equal(rows.length, 5);
assert.ok(rows.every(row => Number(row.split(',')[4]) < 3), rows.join('\n'));
assert.ok(rows.some(row => row.startsWith('unseen,,3,24,')));
assert.ok(rows.some(row => row.startsWith('cross_time,,3,24,')));
if (dense) {
  session('D_on_grid', 'D', staticPlans([3], 3, 'unseen'));
  assert.match(run(1, 'evaluate', 'D_on_grid'), /D unseen point is on the B grid/);
  const incomplete = staticPlans([3.77], 1, 'unseen');
  incomplete[0].error = 'unstarted';
  session('D_missing', 'D', incomplete);
  run(0, 'freeze-d-models', 'B', 'C');
  const dPrimary = JSON.parse(fs.readFileSync(path.join(root, 'models', 'position_B_primary_v1.json')));
  const dOverlap = JSON.parse(fs.readFileSync(path.join(root, 'models', 'position_BC_overlap_v1.json')));
  assert.equal(dPrimary.node_pulse.length, 301);
  assert.equal(dOverlap.node_pulse.length, 38);
  assert.deepEqual(dOverlap.weights, { B: 1 / 2, C2: 1 / 6, C10: 1 / 6, C40: 1 / 6 });
  assert.ok(dPrimary.evidence.every(item => item.use !== 'scan_holdout'));
  assert.match(run(1, 'evaluate-d', 'D_on_grid'), /overlaps B training/);
  run(0, 'evaluate-d', 'D1', 'D2', 'D_missing');
  const resultFile = suffix => fs.readdirSync(path.join(root, 'results')).find(name => name.endsWith(suffix));
  const direction = results(resultFile('.direction_residuals.csv')).split('\n');
  assert.equal(direction.filter(row => row.startsWith('D1,D_missing,') && row.endsWith(',no_center')).length, 4);
  const center = results(resultFile('.center_predictions.csv')).split('\n');
  assert.ok(center.some(row => row.startsWith('D1,D_missing,') && row.endsWith(',missing_direction')));
  assert.ok(center.some(row => row.startsWith('D1,D1,') && row.split(',')[11] === ''));
  assert.equal(fs.readdirSync(path.join(root, 'data', 'figures')).filter(name => /^Z-D_.*\.svg$/.test(name)).length, 3);
}

const files = ['manifest/C.plan.csv', 'manifest/C.trials.csv', 'data/raw/C.csv'].map(f => path.join(root, f));
const originals = files.map(f => fs.readFileSync(f, 'utf8'));
try {
  files.forEach((f, i) => fs.writeFileSync(f, originals[i].replaceAll(',development,', ',scan_holdout,')));
  assert.match(run(1, 'evaluate', 'C'), /Training\/development and acceptance overlap/);
} finally {
  files.forEach((f, i) => fs.writeFileSync(f, originals[i]));
}
console.log('PASS: endpoints, missing samples, unstable hold, interrupted/unstarted trials, frozen model, overlap rejection.');
console.log(root);

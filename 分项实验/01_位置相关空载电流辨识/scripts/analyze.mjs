import fs from 'node:fs';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { runD } from './d_models.mjs';

const project = fileURLToPath(new URL('../', import.meta.url));
const policy = { gapPeriods: 2.5, speedFraction: 0.10, accelFraction: 0.10,
  accelFloor: 1, gridMm: 0.25, minApproachFraction: 0.95, scanDerivativeSpanS: 0.05 };
const mean = values => values.reduce((a, b) => a + b, 0) / values.length;
const rms = values => Math.sqrt(mean(values.map(v => v * v)));
const std = values => {
  const center = mean(values);
  return values.length < 2 ? NaN :
    Math.sqrt(values.reduce((sum, value) => sum + (value - center) ** 2, 0) / (values.length - 1));
};
const hash = data => createHash('sha256').update(data).digest('hex');
const sourceHash = hash(fs.readFileSync(fileURLToPath(import.meta.url)));
const trialKey = row => [row.session_id, row.trial_id].join('/');
const groupKey = row => [row.session_id, row.phase, row.group,
  row.phase === 'C' ? row.speed_mm_s : ''].join('/');
const finite = v => Number.isFinite(v);

function check(ok, message) {
  if (!ok) throw new Error(message);
}

function bucket(rows, key) {
  const result = new Map();
  for (const row of rows) {
    const k = key(row);
    if (!result.has(k)) result.set(k, []);
    result.get(k).push(row);
  }
  return result;
}

const textColumns = new Set(['session_id', 'phase', 'use', 'stage', 'status', 'reason']);
const bSampleFields = new Set(['session_id', 'trial_id', 'phase', 'group', 'use', 'stage',
  'direction', 'speed_mm_s', 't_s', 'x_mm', 'v_mm_s', 'current_fb_raw', 'status', 'valid']);

function csvRead(filename, projection) {
  const [header, ...lines] = fs.readFileSync(filename, 'utf8').trimEnd().split(/\r?\n/);
  const columns = header.split(',');
  const selected = columns.map((name, index) => ({ name, index }))
    .filter(({ name }) => !projection || projection.has(name));
  check(!projection || selected.length === projection.size, 'Missing CSV fields in ' + filename);
  return lines.map(line => {
    const fields = line.split(',');
    check(fields.length === columns.length, 'Invalid CSV row in ' + filename);
    const row = {};
    for (const { name, index } of selected) {
      const value = fields[index];
      row[name] = textColumns.has(name) ? value : value === '' ? NaN : Number(value);
    }
    return row;
  });
}

function csvWrite(file, columns, rows) {
  const encode = value => {
    if (value === undefined || (typeof value === 'number' && !finite(value))) return '';
    const text = String(value);
    return /[",\n\r]/.test(text) ? '"' + text.replaceAll('"', '""') + '"' : text;
  };
  fs.writeFileSync(file, [columns.join(','), ...rows.map(row =>
    columns.map(col => encode(row[col])).join(','))].join('\n') + '\n');
}

function load(root, sessions, predicate) {
  const groups = new Map();
  const rejected = [];
  const conditions = new Map();
  check(sessions.length && new Set(sessions).size === sessions.length, 'Duplicate or missing session IDs');
  for (const id of sessions) {
    check(/^[A-Za-z0-9_-]+$/.test(id), 'Invalid session ID');
    const base = path.join(root, 'manifest', id);
    const meta = Object.fromEntries(fs.readFileSync(base + '.txt', 'utf8').trimEnd().split(/\r?\n/)
      .map(line => [line.slice(0, line.indexOf('=')), line.slice(line.indexOf('=') + 1)]));
    const plan = csvRead(base + '.plan.csv');
    const selected = plan.filter(predicate);
    if (!selected.length) continue;
    check(plan.every(row => {
      const allowed = { READ: ['diagnostic'], A0: ['diagnostic'], A1: ['diagnostic'],
        A2: ['diagnostic'], B: ['train', 'boundary_single', 'boundary_compare'], C: ['development', 'scan_holdout'],
        D: ['unseen', 'cross_time'] };
      return allowed[row.phase]?.includes(row.use);
    }), 'Invalid phase or data purpose: ' + id);
    check(meta.session_id === id && meta.phase === plan[0].phase, 'Session metadata mismatch: ' + id);
    check(meta.current_api === 'GTN_GetAtlTorque' && meta.current_unit === 'raw' &&
      meta.core === '2' && meta.axis === '1' && meta.pulse_per_mm === '1000', 'Unsupported configuration: ' + id);
    for (const field of ['config', 'R_state', 'position_mode', 'loop_index'])
      check(meta[field]?.length, 'Missing ' + field + ' in ' + id);
    conditions.set(id, Object.fromEntries(['config', 'R_state', 'position_mode', 'loop_index',
      'current_api', 'current_unit', 'core', 'axis', 'pulse_per_mm'].map(field => [field, meta[field]])));
    const trials = csvRead(base + '.trials.csv');
    const rawFile = path.join(root, 'data', 'raw', id + '.csv');
    const isB = meta.phase === 'B';
    const rawDigest = isB ? hash(fs.readFileSync(rawFile)) : undefined;
    const samples = bucket(csvRead(rawFile, isB ? bSampleFields : undefined), trialKey);
    const expected = new Set(plan.map(trialKey));
    check(expected.size === plan.length && plan.every(row => row.session_id === id &&
      row.phase === meta.phase && Number.isInteger(row.trial_id) && Number.isInteger(row.group)),
    'Invalid planned trial identity: ' + id);
    check(new Set(trials.map(trialKey)).size === trials.length, 'Duplicate trial summary: ' + id);
    const actual = new Map(trials.map(row => [trialKey(row), row]));
    check([...actual.keys(), ...samples.keys()].every(key => expected.has(key)), 'Unplanned trial: ' + id);
    check(new Set(plan.map(row => row.use)).size === 1 || meta.phase === 'C',
      'Mixed purposes in static session: ' + id);
    for (const [key, rows] of bucket(plan, groupKey)) {
      check(new Set(rows.map(row => row.use)).size === 1, 'Overlapping group purposes: ' + key);
    }

    for (const expectedTrial of selected) {
      const key = trialKey(expectedTrial);
      const t = actual.get(key);
      const rows = samples.get(key) ?? [];
      samples.delete(key);
      const group = groupKey(expectedTrial);
      if (!groups.has(group)) groups.set(group, []);
      const record = { ...expectedTrial, key, groupKey: group, meta, t,
        ...(isB ? { rawDigest } : { rows }) };
      groups.get(group).push(record);
      try {
        check(t, 'missing_trial_summary');
        check(['phase', 'group', 'use', 'direction', 'target_mm', 'approach_mm', 'speed_mm_s']
          .every(field => Object.is(t[field], expectedTrial[field])), 'trial_plan_mismatch');
        check(t.status === 'completed', t.status + '/' + t.reason);
        check(rows.length && rows.every((s, i) => s.valid === 1 && finite(s.current_fb_raw) &&
          finite(s.x_mm) && finite(s.v_mm_s) && finite(s.t_s) &&
          (!i || s.t_s > rows[i - 1].t_s) && s.phase === t.phase && s.group === t.group &&
          s.use === t.use && s.direction === t.direction && s.speed_mm_s === t.speed_mm_s),
        'missing_invalid_or_unordered_samples');
        if (t.direction) {
          const start = rows.filter(s => s.stage === 'approach_start').at(-1);
          const arrivals = rows.filter(s => s.stage === (t.phase === 'C' ? 'scan' : 'approach'));
          check(start && Math.abs(start.x_mm - t.start_mm) < 1e-7 &&
            Math.abs(t.approach_mm - t.start_mm) <= t.pos_tol_mm &&
            t.direction * (t.target_mm - t.start_mm) >=
              policy.minApproachFraction * Math.abs(t.target_mm - t.approach_mm) &&
            arrivals.length && t.direction * (arrivals.at(-1).x_mm - t.start_mm) >=
              .9 * Math.abs(t.target_mm - t.approach_mm), 'invalid_measured_approach');
        }
        if (t.phase === 'C') record.curve = constantSpeed(record);
        else if (t.phase === 'A1') {
          record.windows = driftWindows(record);
          check(record.windows.length === Number(meta.segments) &&
            record.windows.every((window, index) => window.segment === index + 1),
          'missing_or_unordered_continuous_segment');
        }
        else {
          const hold = rows.filter(s => s.stage === 'hold');
          record.value = stableHold(t, hold);
          if (isB) {
            const start = hold.at(-1).t_s - t.window_s;
            record.holdPositionPulse = 1000 * mean(hold.filter(s => s.t_s >= start).map(s => s.x_mm));
          }
          if (t.phase === 'A2' && t.hold_s >= 4) {
            record.seconds = [3, 4].map(second => {
              try {
                const end = hold.at(-1).t_s - (t.hold_s - second);
                return { value: windowMean(t, hold, end - 1, end) };
              } catch (error) {
                return { reason: error.message };
              }
            });
          }
        }
      } catch (error) {
        record.error = error.message;
        rejected.push({ session: id, trial: expectedTrial.trial_id, phase: expectedTrial.phase,
          group, reason: error.message });
      }
    }
  }
  return { groups, rejected, conditions };
}

function durationMean(rows) {
  let integral = 0;
  for (let i = 1; i < rows.length; i++)
    integral += (rows[i].t_s - rows[i - 1].t_s) *
      (rows[i - 1].current_fb_raw + rows[i].current_fb_raw) / 2;
  return integral / (rows.at(-1).t_s - rows[0].t_s);
}

function stable(t, rows) {
  const encoderMotionChecks = ['A2', 'B', 'D'].includes(t.phase);
  return rows.length >= 3 && rows.every((s, i) =>
    s.x_mm >= t.target_mm - t.pos_tol_mm && s.x_mm <= t.target_mm + t.pos_tol_mm &&
    (encoderMotionChecks || Math.abs(s.v_mm_s) <= t.vel_tol_mm_s) && !(s.status & 0x400) &&
    (!i || s.t_s - rows[i - 1].t_s <= policy.gapPeriods * t.period_s));
}

function stableHold(t, hold) {
  check(hold.length, 'missing_hold');
  const end = hold.at(-1).t_s;
  check(end - hold[0].t_s >= t.hold_s - policy.gapPeriods * t.period_s, 'short_hold');
  return windowMean(t, hold, end - t.window_s, end);
}

function windowMean(t, hold, start, end) {
  const first = hold.findLastIndex(s => s.t_s <= start);
  const last = hold.findIndex(s => s.t_s >= end);
  check(first >= 0 && last > first, 'missing_stability_window');
  const window = hold.slice(first, last + 1);
  check(stable(t, window), 'unstable_or_gapped_window');
  const edge = (a, b, time) => ({ t_s: time, current_fb_raw: a.current_fb_raw +
    (b.current_fb_raw - a.current_fb_raw) * (time - a.t_s) / (b.t_s - a.t_s) });
  return durationMean([edge(window[0], window[1], start), ...window.slice(1, -1),
    edge(window.at(-2), window.at(-1), end)]);
}

function driftWindows(record) {
  const { t, rows } = record;
  return Array.from(bucket(rows.filter(s => s.stage === 'hold'), s => s.segment), ([segment, block]) => {
    check(stable(t, block) && block.at(-1).t_s - block[0].t_s >=
      t.hold_s - policy.gapPeriods * t.period_s, 'invalid_continuous_hold_segment');
    const ts = block.map(s => s.t_s);
    const ys = block.map(s => s.current_fb_raw);
    const drift = slope(ts, ys);
    return { session: t.session_id, trial: t.trial_id, position: t.target_mm, segment,
      t_s: mean(ts), powered_s: mean(block.map(s => s.powered_s)),
      card_temp_c: mean(block.filter(s => finite(s.temp_card_c)).map(s => s.temp_card_c)),
      mean_raw: durationMean(block), sample_std_raw: std(ys),
      detrended_sample_std_raw: std(ys.map((y, i) => y - drift * ts[i])) };
  });
}

function slope(x, y) {
  const xc = mean(x), yc = mean(y);
  return x.reduce((sum, value, i) => sum + (value - xc) * (y[i] - yc), 0) /
    x.reduce((sum, value) => sum + (value - xc) ** 2, 0);
}

function constantSpeed(record) {
  const { t, rows, meta } = record;
  const scan = rows.filter(s => s.stage === 'scan');
  const speed = t.speed_mm_s;
  const accelLimit = Math.max(policy.accelFloor, Number(meta.acc_mm_s2) * policy.accelFraction);
  const gapLimit = policy.gapPeriods * t.period_s;
  const gaps = [0];
  for (let i = 1; i < scan.length; i++)
    gaps.push(gaps[i - 1] + Number(scan[i].t_s - scan[i - 1].t_s > gapLimit));
  let run = [], best = [];
  let left = 0, right = 1;
  const span = policy.scanDerivativeSpanS;
  for (let i = 1; i + 1 < scan.length; i++) {
    const b = scan[i];
    if (b.t_s - scan[0].t_s < span) continue;
    while (left + 1 < i && b.t_s - scan[left + 1].t_s >= span) left++;
    while (right < scan.length && scan[right].t_s - b.t_s < span) right++;
    if (right === scan.length) break;
    const a = scan[left], c = scan[right];
    const before = (b.x_mm - a.x_mm) / (b.t_s - a.t_s);
    const after = (c.x_mm - b.x_mm) / (c.t_s - b.t_s);
    const acceleration = 2 * (after - before) / (c.t_s - a.t_s);
    const good = gaps[left] === gaps[right] && (b.status & 0x400) &&
      [before, after].every(v => Math.abs(t.direction * v - speed) <= policy.speedFraction * speed) &&
      Math.abs(acceleration) <= accelLimit;
    if (!good) {
      run = [];
      continue;
    }
    if (run.length && t.direction * (b.x_mm - run.at(-1).x_mm) <= 0) run = [];
    run.push({ ...b, encoder_speed: (c.x_mm - a.x_mm) / (c.t_s - a.t_s) });
    if (run.length >= 3 && (!best.length ||
        Math.abs(run.at(-1).x_mm - run[0].x_mm) > Math.abs(best.at(-1).x_mm - best[0].x_mm))) {
      // Retain the contiguous run, including the part accumulated before it became the longest.
      best = run;
    }
  }
  check(best.length >= 3, 'no_valid_constant_speed_span');
  return t.direction > 0 ? best : best.reverse();
}

function pairStatic(records) {
  const rows = [];
  const failed = records.some(r => r.error);
  for (const [position, pair] of bucket(records, r => r.target_mm)) {
    const p = pair.find(r => r.direction === 1);
    const n = pair.find(r => r.direction === -1);
    const expectedOne = position === 0 ? n : position === 32 ? p : undefined;
    const boundary = pair.length === 1 && pair[0].use === 'boundary_single' &&
      (position <= 1 ? n : position >= 31 ? p : undefined);
    if (boundary || (expectedOne && pair.length === 1 && ['A0', 'B'].includes(expectedOne.phase))) {
      if (!(boundary || expectedOne).error) rows.push({ position, positive: p?.value, negative: n?.value,
        positive_actual_pulse: p?.holdPositionPulse, negative_actual_pulse: n?.holdPositionPulse, kind: 'single' });
    } else if (pair.length === 2 && p && n && !p.error && !n.error) {
      rows.push({ position, positive: p.value, negative: n.value, ip: (p.value + n.value) / 2,
        positive_actual_pulse: p.holdPositionPulse, negative_actual_pulse: n.holdPositionPulse,
        h: (p.value - n.value) / 2, kind: 'paired' });
    } else if (!failed) {
      throw new Error('Invalid static pair: ' + records[0].groupKey + ' at ' + position);
    }
  }
  return rows;
}

function interp(x, nodes, values, tangents) {
  if (x < nodes[0] || x > nodes.at(-1)) return NaN;
  const right = Math.max(1, nodes.findIndex(v => v >= x));
  const left = right - 1;
  const h = nodes[right] - nodes[left], t = (x - nodes[left]) / h;
  const [a, b] = [values[left], values[right]];
  if (!tangents) return a + t * (b - a);
  return (2 * t ** 3 - 3 * t ** 2 + 1) * a + (t ** 3 - 2 * t ** 2 + t) * h * tangents[left] +
    (-2 * t ** 3 + 3 * t ** 2) * b + (t ** 3 - t ** 2) * h * tangents[right];
}

function pairScan(records) {
  const p = records.find(r => r.direction === 1), n = records.find(r => r.direction === -1);
  check(records.length === 2 && p && n && p.target_mm === 32 && n.target_mm === 0,
    'Invalid scan pair: ' + records[0].groupKey);
  if (records.some(r => r.error)) return [];
  const a = p.curve.map(s => s.x_mm), b = n.curve.map(s => s.x_mm);
  const py = p.curve.map(s => s.current_fb_raw), ny = n.curve.map(s => s.current_fb_raw);
  const positiveSpeed = mean(p.curve.map(s => s.encoder_speed));
  const negativeSpeed = mean(n.curve.map(s => s.encoder_speed));
  const step = policy.gridMm;
  const result = [];
  for (let x = Math.ceil(Math.max(a[0], b[0]) / step) * step;
      x <= Math.min(a.at(-1), b.at(-1)); x += step) {
    const positive = interp(x, a, py), negative = interp(x, b, ny);
    result.push({ position: x, positive, negative, ip: (positive + negative) / 2,
      h: (positive - negative) / 2, kind: 'paired',
      positive_speed: positiveSpeed, negative_speed: negativeSpeed,
      positive_min: a[0], positive_max: a.at(-1), negative_min: b[0], negative_max: b.at(-1) });
  }
  return result.length >= 3 ? result : [];
}

function references(data, phases) {
  const result = [];
  for (const records of data.groups.values()) {
    const first = records[0];
    if (!phases.includes(first.phase)) continue;
    const paired = first.phase === 'C' ? pairScan(records) : pairStatic(records);
    if (first.phase === 'C' && !paired.length) data.rejected.push({
      session: first.session_id, phase: 'C', group: first.groupKey, reason: 'no_paired_scan_coverage'
    });
    for (const pair of paired) result.push({ ...pair, session: first.session_id,
      group: first.groupKey, use: first.use, phase: first.phase, speed: first.speed_mm_s,
      protocol: first.meta.B_protocol,
      group_complete: Number(!records.some(r => r.error)) });
  }
  return result;
}

function pchip(nodes, values) {
  const h = nodes.slice(1).map((x, i) => x - nodes[i]);
  const d = h.map((width, i) => (values[i + 1] - values[i]) / width);
  function edge(h0, h1, d0, d1) {
    const s = ((2 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
    if (Math.sign(s) !== Math.sign(d0)) return 0;
    return Math.sign(d0) !== Math.sign(d1) && Math.abs(s) > 3 * Math.abs(d0) ? 3 * d0 : s;
  }
  return nodes.map((_, i) => {
    if (i === 0) return edge(h[0], h[1], d[0], d[1]);
    if (i === nodes.length - 1) return edge(h.at(-1), h.at(-2), d.at(-1), d.at(-2));
    if (d[i - 1] * d[i] <= 0) return 0;
    const w1 = 2 * h[i] + h[i - 1], w2 = h[i] + 2 * h[i - 1];
    return (w1 + w2) / (w1 / d[i - 1] + w2 / d[i]);
  });
}

function poly(xs, ys, degree) {
  const cols = Array.from({ length: degree + 1 }, (_, j) => xs.map(x => ((x - 16) / 16) ** j));
  // Modified Gram-Schmidt with small, centered design matrices.
  const q = [], r = cols.map(() => cols.map(() => 0));
  const dot = (a, b) => a.reduce((sum, v, i) => sum + v * b[i], 0);
  for (const [j, col] of cols.entries()) {
    let v = col;
    for (let i = 0; i < j; i++) {
      r[i][j] = dot(q[i], v);
      v = v.map((value, k) => value - r[i][j] * q[i][k]);
    }
    r[j][j] = Math.sqrt(dot(v, v));
    check(r[j][j] > 1e-12, 'Singular fit');
    q.push(v.map(value => value / r[j][j]));
  }
  const coef = q.map(v => dot(v, ys));
  for (let i = degree; i >= 0; i--) {
    for (let j = i + 1; j <= degree; j++) coef[i] -= r[i][j] * coef[j];
    coef[i] /= r[i][i];
  }
  return coef;
}

function fit(type, refs) {
  const byX = bucket(refs, r => r.position);
  const nodes = [...byX.keys()].sort((a, b) => a - b);
  const values = nodes.map(x => mean(byX.get(x).map(r => r.ip)));
  const range = [nodes[0], nodes.at(-1)];
  if (type === 'pchip') return { type, range, nodes, values, tangents: pchip(nodes, values) };
  return { type, range, center_mm: 16, scale_mm: 16,
    coefficients: poly(nodes, values, ['constant', 'linear', 'quadratic'].indexOf(type)) };
}

function predict(model, x) {
  if (x < model.range[0] || x > model.range[1]) return NaN;
  if (model.type === 'pchip') return interp(x, model.nodes, model.values, model.tangents);
  return model.coefficients.reduce((sum, value, i) =>
    sum + value * ((x - model.center_mm) / model.scale_mm) ** i, 0);
}

function selectModel(refs, quadratic) {
  const groups = bucket(refs, r => r.group);
  const nodes = [...new Set(refs.map(r => r.position))].sort((a, b) => a - b);
  check(groups.size >= 2 && nodes.length >= 5 &&
    [...groups.values()].every(rows => rows.length === nodes.length &&
      new Set(rows.map(r => r.position)).size === nodes.length),
  'Need at least two complete B groups with the same paired nodes');
  const pulses = nodes.map(x => Math.round(x * 1000));
  const legacy = pulses.length === 15 && pulses.every((x, i) => x === 2000 + i * 2000);
  const dense = pulses.length === 301 && pulses.every((x, i) => x === 1000 + i * 100);
  check(legacy || dense, 'Unexpected B paired grid; use only one complete geometry per model');
  return ['constant', 'linear', ...(quadratic ? ['quadratic'] : []), 'pchip'].map(type => {
    const residuals = [];
    for (const [group, validation] of groups) {
      for (const row of validation.filter(r => r.position !== nodes[0] && r.position !== nodes.at(-1))) {
        const trained = fit(type, refs.filter(r => r.group !== group && r.position !== row.position));
        residuals.push(predict(trained, row.position) - row.ip);
      }
    }
    return { model: fit(type, refs), cv_rmse_raw: rms(residuals), cv_n: residuals.length };
  });
}

function diagnostics(data, results) {
  const records = [...data.groups.values()].flat();
  const windows = records.flatMap(r => r.windows ?? []);
  csvWrite(path.join(results, 'A1_windows.csv'),
    ['session', 'trial', 'position', 'segment', 't_s', 'powered_s', 'card_temp_c', 'mean_raw',
      'sample_std_raw', 'detrended_sample_std_raw'], windows);
  const drift = Array.from(bucket(windows, w => [w.session, w.trial].join('/')), ([, rows]) => {
    const ts = rows.map(r => r.t_s), ys = rows.map(r => r.mean_raw);
    return { ...rows[0], n: rows.length, window_std_raw: std(ys),
      drift_raw_per_min: 60 * slope(ts, ys), change_raw: ys.at(-1) - ys[0] };
  });
  csvWrite(path.join(results, 'A1_drift.csv'),
    ['session', 'trial', 'position', 'n', 'window_std_raw', 'drift_raw_per_min', 'change_raw'], drift);

  const repeats = records.filter(r => r.phase === 'A2' && !r.error).map(r => ({
    session: r.session_id, trial: r.trial_id, position: r.target_mm, direction: r.direction,
    mean_raw: r.value, actual_start: r.t.start_mm, hold_s: r.t.hold_s,
    third_second_mean_raw: r.seconds?.[0].value, fourth_second_mean_raw: r.seconds?.[1].value,
    fourth_minus_third_raw: r.seconds?.[1].value - r.seconds?.[0].value,
    third_second_reason: r.seconds?.[0].reason, fourth_second_reason: r.seconds?.[1].reason,
    position_error: mean(r.rows.filter(s => s.stage === 'hold').map(s => s.x_mm)) - r.target_mm
  }));
  csvWrite(path.join(results, 'A2_trials.csv'),
    ['session', 'trial', 'position', 'direction', 'actual_start', 'position_error', 'mean_raw', 'hold_s',
      'third_second_mean_raw', 'fourth_second_mean_raw', 'fourth_minus_third_raw',
      'third_second_reason', 'fourth_second_reason'], repeats);
  const a2 = Array.from(bucket(repeats, r => [r.session, r.position, r.direction].join('/')),
    ([, rows]) => {
      const vals = rows.map(r => r.mean_raw);
      const deltas = rows.map(r => r.fourth_minus_third_raw).filter(finite);
      return { ...rows[0], n: rows.length, mean_raw: mean(vals), repeat_std_raw: std(vals),
        n_window_pairs: deltas.length, mean_fourth_minus_third_raw: mean(deltas),
        mean_abs_window_change_raw: mean(deltas.map(Math.abs)),
        max_abs_window_change_raw: deltas.length ? Math.max(...deltas.map(Math.abs)) : NaN };
    });
  csvWrite(path.join(results, 'A2_repeatability.csv'),
    ['session', 'position', 'direction', 'n', 'mean_raw', 'repeat_std_raw', 'n_window_pairs',
      'mean_fourth_minus_third_raw', 'mean_abs_window_change_raw', 'max_abs_window_change_raw'], a2);

  const a0 = records.filter(r => r.phase === 'A0' && !r.error).map(r => ({
    session: r.session_id, position: r.target_mm, direction: r.direction, mean_raw: r.value,
    peak_abs_raw: Math.max(...r.rows.filter(s => s.stage === 'hold').map(s => Math.abs(s.current_fb_raw)))
  }));
  csvWrite(path.join(results, 'A0_scout.csv'),
    ['session', 'position', 'direction', 'mean_raw', 'peak_abs_raw'], a0);
  const suggestions = [];
  for (const [, session] of bucket(a0, r => r.session)) {
    const byX = Array.from(bucket(session, r => r.position), ([position, rows]) => ({
      session: rows[0].session, position, abs_mean_raw: mean(rows.map(r => Math.abs(r.mean_raw))),
      peak_abs_raw: Math.max(...rows.map(r => r.peak_abs_raw))
    })).sort((a, b) => a.abs_mean_raw - b.abs_mean_raw);
    const interior = byX.filter(r => r.position > 0 && r.position < 32);
    if (interior.length) suggestions.push({ ...interior.reduce((a, b) =>
      Math.abs(a.position - 16) < Math.abs(b.position - 16) ? a : b), role: 'typical' });
    suggestions.push({ ...byX[0], role: 'low' }, { ...byX.at(-1), role: 'high' });
  }
  csvWrite(path.join(results, 'A0_representatives.csv'),
    ['session', 'role', 'position', 'abs_mean_raw', 'peak_abs_raw'], suggestions);
}

const refColumns = ['session', 'phase', 'use', 'group', 'group_complete', 'speed', 'position',
  'kind', 'positive', 'negative', 'ip', 'h', 'positive_speed', 'negative_speed',
  'positive_min', 'positive_max', 'negative_min', 'negative_max'];

function summarizeB(data, results) {
  consistent(data);
  const refs = references(data, ['B']);
  const columns = ['session', 'protocol', 'group', 'use', 'group_complete', 'target_pulse',
    'kind', 'positive_actual_pulse', 'negative_actual_pulse', 'positive', 'negative', 'ip', 'h'];
  for (const session of data.conditions.keys()) {
    const rows = refs.filter(r => r.session === session)
      .map(r => ({ ...r, target_pulse: Math.round(r.position * 1000) }));
    csvWrite(path.join(results, session + '.holds.csv'), columns, rows);
    csvWrite(path.join(results, session + '.rejected.csv'), ['session', 'trial', 'phase', 'group', 'reason'],
      data.rejected.filter(r => r.session === session));
  }
  const main = refs.filter(r => r.protocol === 'B' && r.use === 'train' && r.group_complete);
  const comparison = refs.filter(r => r.use === 'boundary_compare' && r.group_complete);
  const session = main[0]?.session;
  if (!session || !comparison.length) return;
  const matches = [];
  for (const row of comparison) {
    const baseline = main.filter(r => r.position === row.position && r.speed === row.speed);
    if (!baseline.length) continue;
    for (const direction of ['positive', 'negative']) {
      const mainMean = mean(baseline.map(r => r[direction]));
      matches.push({ main_sessions: [...new Set(baseline.map(r => r.session))].join(';'),
        boundary_session: row.session, boundary_group: row.group, target_pulse: Math.round(row.position * 1000),
        direction, n_main: baseline.length, main_raw: mainMean, boundary_raw: row[direction],
        boundary_minus_main_raw: row[direction] - mainMean });
    }
  }
  csvWrite(path.join(results, session + '.boundary-overlap.csv'),
    ['main_sessions', 'boundary_session', 'boundary_group', 'target_pulse', 'direction',
      'n_main', 'main_raw', 'boundary_raw', 'boundary_minus_main_raw'], matches);
}

function rejected(file, data) {
  csvWrite(file, ['session', 'trial', 'phase', 'group', 'reason'], data.rejected);
}

function consistent(data, preferred) {
  const conditions = [...data.conditions.values()];
  const expected = preferred ?? conditions[0];
  check(conditions.every(actual => JSON.stringify(actual) === JSON.stringify(expected)),
    'Site configuration, position mode, or fixed R state differs');
  return expected;
}

function evidence(data) {
  return [...data.groups.values()].map(records => ({
    group: records[0].groupKey, session: records[0].session_id,
    use: records[0].use, digest: hash(JSON.stringify(records))
  }));
}

function metrics(rows, model, baseline) {
  const sorted = (m) => rows.map(r => Math.abs(predict(m, r.position) - r.ip)).sort((a, b) => a - b);
  const errors = sorted(model), constant = sorted(baseline);
  const rmse = rms(errors), base = rms(constant);
  return { n: rows.length, rmse_raw: rmse, p95_abs_raw: errors[Math.ceil(errors.length * .95) - 1],
    constant_rmse_raw: base, improvement_pct: base > 0 ? 100 * (base - rmse) / base : NaN };
}

function graph(filename, refs, model, title) {
  fs.mkdirSync(path.dirname(filename), { recursive: true });
  const used = refs.filter(r => finite(r.ip) && r.group_complete);
  const xs = Array.from({ length: 113 }, (_, i) => model.range[0] +
    i * (model.range[1] - model.range[0]) / 112);
  const traces = [{ label: 'B frozen model', color: '#1a202c',
    rows: xs.map(position => ({ position, ip: predict(model, position) })) }];
  const colors = ['#007a70', '#8141a2', '#ae4a19', '#2466a2', '#a12062'];
  const scanLabels = [...new Set(used.filter(r => r.phase === 'C').map(r => r.speed))].sort((a, b) => a - b);
  scanLabels.forEach((speed, i) => traces.push({ label: 'C ' + speed + ' mm/s', color: colors[i],
    rows: used.filter(r => r.phase === 'C' && r.speed === speed) }));
  const pointRows = used.filter(r => r.phase !== 'C');
  if (pointRows.length) traces.push({ label: pointRows[0].phase + ' paired means', color: '#4555b8', rows: pointRows });
  const values = traces.flatMap(t => t.rows.map(r => r.ip));
  const lo = Math.min(...values), hi = Math.max(...values);
  const span = Math.max(1, hi - lo), y = value => 335 - 265 * (value - lo + .05 * span) / (1.1 * span);
  const x = position => 80 + position * 20;
  const layers = traces.map((t, i) => {
    const points = i ? t.rows.map(r => '<circle cx="' + x(r.position) + '" cy="' + y(r.ip) +
      '" r="2.4" opacity=".65" fill="' + t.color + '"><title>' + r.position + ' mm, ' +
      r.ip.toFixed(3) + ' raw</title></circle>').join('') :
      '<polyline fill="none" stroke="' + t.color + '" stroke-width="2" points="' +
      t.rows.map(r => x(r.position) + ',' + y(r.ip)).join(' ') + '"/>';
    return points + '<text x="755" y="' + (80 + i * 24) + '" fill="' + t.color + '">' + t.label + '</text>';
  });
  const ticks = Array.from({ length: 9 }, (_, i) => '<text x="' + x(i * 4) +
    '" y="371" text-anchor="middle">' + i * 4 + '</text>').join('');
  const yTicks = Array.from({ length: 5 }, (_, i) => {
    const tick = lo + span * i / 4, height = y(tick);
    return '<path d="M80 ' + height + 'H720" stroke="#d7dce1"/><text x="70" y="' + (height + 4) +
      '" text-anchor="end">' + tick.toFixed(2) + '</text>';
  }).join('');
  fs.writeFileSync(filename, '<!doctype html><meta charset="utf-8"><title>' + title +
    '</title><style>body{font:16px system-ui;margin:24px;color:#1a202c}svg{max-width:100%;height:auto}</style>' +
    '<h1>' + title + '</h1><svg viewBox="0 0 980 410" role="img" aria-label="' + title + '">' +
    '<text x="80" y="30">Paired current (raw)</text><path d="M80 65V350H720" fill="none" stroke="#333"/>' +
    ticks + yTicks + layers.join('') + '<text x="400" y="399">Actual position (mm)</text></svg>' +
    '<p>Scan dots cover the common valid constant-speed span of each round trip. Static dots use target nodes.</p>');
}

function main() {
  const [mode, rootArg, ...args] = process.argv.slice(2);
  if (['freeze-d-models', 'evaluate-d'].includes(mode)) {
    check(rootArg && args.length, 'Usage: node analyze.mjs freeze-d-models|evaluate-d ROOT SESSION ...');
    runD(mode, rootArg === '.' ? project : path.resolve(rootArg), args,
      { load, references, consistent, evidence, policy, hash, sourceHash, csvWrite, interp, windowMean });
    return;
  }
  check(['diagnose', 'summarize-b', 'freeze', 'evaluate'].includes(mode) && rootArg && args.length,
    'Usage: node analyze.mjs diagnose|summarize-b|freeze|evaluate ROOT SESSION [SESSION ...] [--quadratic]');
  const root = rootArg === '.' ? project : path.resolve(rootArg);
  const sessions = args.filter(v => v !== '--quadratic');
  const results = path.join(root, 'results');
  fs.mkdirSync(results, { recursive: true });
  const modelPath = path.join(root, 'models', 'position_current.json');
  if (mode === 'diagnose') {
    const data = load(root, sessions, r => ['A0', 'A1', 'A2'].includes(r.phase));
    diagnostics(data, results);
    rejected(path.join(results, 'diagnostics_rejected.csv'), data);
    return;
  }
  if (mode === 'summarize-b') {
    const data = load(root, sessions, r => r.phase === 'B');
    check(data.groups.size > 0, 'No B sessions selected');
    summarizeB(data, results);
    return;
  }
  if (mode === 'freeze') {
    check(!fs.existsSync(modelPath), 'Frozen model already exists. Use a new analysis dataset/version.');
    const data = load(root, sessions, r => (r.phase === 'B' && r.use === 'train') ||
      (r.phase === 'C' && r.use === 'development'));
    const conditions = consistent(data);
    const refs = references(data, ['B', 'C']);
    const training = refs.filter(r => r.phase === 'B' && r.group_complete && r.kind === 'paired');
    const candidates = selectModel(training, args.includes('--quadratic'));
    const choice = candidates.reduce((a, b) => b.cv_rmse_raw + 1e-9 < a.cv_rmse_raw ? b : a);
    const training_pulses = [...new Set(training.map(r => Math.round(r.position * 1000)))].sort((a, b) => a - b);
    const document = { schema: 1, created_utc: new Date().toISOString(), conditions, policy, sourceHash,
      training_pulses,
      selection: 'Leave one complete B group and one interior position out together; lowest CV RMSE',
      model: choice.model, baseline: candidates[0].model, candidates, evidence: evidence(data) };
    const comparisons = refs.map(r => ({ ...r, prediction: predict(choice.model, r.position),
      residual: predict(choice.model, r.position) - r.ip }));
    csvWrite(path.join(results, 'B_and_C_development.csv'), [...refColumns, 'prediction', 'residual'], comparisons);
    csvWrite(path.join(results, 'model_selection.csv'), ['type', 'cv_rmse_raw', 'cv_n'],
      candidates.map(r => ({ ...r, type: r.model.type })));
    rejected(path.join(results, 'freeze_rejected.csv'), data);
    graph(path.join(root, 'data', 'figures', 'development.html'), refs, choice.model, 'B and C development groups');
    fs.mkdirSync(path.dirname(modelPath), { recursive: true });
    fs.writeFileSync(modelPath, JSON.stringify(document, null, 2) + '\n', { flag: 'wx' });
    console.log('Frozen model: ' + choice.model.type + ' [' + choice.model.range.join(', ') + '] mm');
    return;
  }

  const frozen = JSON.parse(fs.readFileSync(modelPath, 'utf8'));
  check(frozen.schema === 1 && frozen.sourceHash === sourceHash &&
    JSON.stringify(frozen.policy) === JSON.stringify(policy), 'Frozen analysis version differs');
  const data = load(root, sessions, r => r.phase === 'D' ||
    (r.phase === 'C' && r.use === 'scan_holdout'));
  consistent(data, frozen.conditions);
  const oldGroups = new Set(frozen.evidence.map(r => r.group));
  const oldSessions = new Set(frozen.evidence.map(r => r.session));
  const trainingPulses = new Set(frozen.training_pulses);
  for (const r of [...data.groups.values()].flat()) {
    check(!oldGroups.has(r.groupKey) && (r.phase !== 'D' || !oldSessions.has(r.session_id)),
      'Training/development and acceptance overlap: ' + r.groupKey);
    if (r.phase === 'D') {
      check(['unseen', 'cross_time'].includes(r.use), 'Unknown D purpose');
      if (r.use === 'unseen')
        check(!trainingPulses.has(Math.round(r.target_mm * 1000)), 'D unseen point is on the B grid');
    }
  }
  const refs = references(data, ['C', 'D']);
  const eligible = refs.filter(r => r.group_complete && r.kind === 'paired' &&
    finite(predict(frozen.model, r.position)));
  for (const r of refs) if (r.group_complete && !finite(predict(frozen.model, r.position)))
    data.rejected.push({ session: r.session, phase: r.phase, group: r.group,
      reason: 'out_of_model_range_at_' + r.position });
  csvWrite(path.join(results, 'acceptance_predictions.csv'), [...refColumns, 'prediction', 'residual',
    'constant_prediction'], eligible.map(r => ({ ...r, prediction: predict(frozen.model, r.position),
    residual: predict(frozen.model, r.position) - r.ip, constant_prediction: predict(frozen.baseline, r.position) })));
  const summary = [];
  for (const use of ['unseen', 'cross_time', 'scan_holdout']) {
    const rows = eligible.filter(r => r.use === use);
    if (!rows.length) summary.push({ use, n: 0, groups: 0 });
    for (const [speed, group] of bucket(rows, r => use === 'scan_holdout' ? r.speed : '')) {
      summary.push({ use, speed, groups: new Set(group.map(r => r.group)).size,
        ...metrics(group, frozen.model, frozen.baseline) });
    }
  }
  csvWrite(path.join(results, 'acceptance_metrics.csv'),
    ['use', 'speed', 'groups', 'n', 'rmse_raw', 'p95_abs_raw', 'constant_rmse_raw', 'improvement_pct'], summary);
  rejected(path.join(results, 'acceptance_rejected.csv'), data);
  graph(path.join(root, 'data', 'figures', 'acceptance.html'), eligible, frozen.model, 'Independent acceptance groups');
  const a2file = path.join(results, 'A2_repeatability.csv');
  const a2note = fs.existsSync(a2file) ?
    'A2 reference: A2_repeatability.csv. Compare by position, direction and session.' :
    'A2 repeatability is unavailable. Run diagnose with the A2 sessions to generate the reference.';
  fs.writeFileSync(path.join(results, 'acceptance_notes.txt'),
    'Frozen model SHA-256: ' + hash(fs.readFileSync(modelPath)) + '\n' +
    'C n counts position-grid values; groups counts complete round trips contributing values.\n' +
    'D n counts paired position references; groups counts full repeat groups.\n' + a2note + '\n');
  console.log('Acceptance paired rows: ' + eligible.length);
}

try {
  main();
} catch (error) {
  console.error(error.message);
  process.exitCode = 1;
}

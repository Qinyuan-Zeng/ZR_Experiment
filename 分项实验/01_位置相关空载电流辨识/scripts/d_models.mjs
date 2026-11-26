import fs from 'node:fs';
import path from 'node:path';

const primaryFile = 'position_B_primary_v1.json';
const overlapFile = 'position_BC_overlap_v1.json';
const speeds = [2000, 10000, 40000];
const weights = { B: 1 / 2, C2: 1 / 6, C10: 1 / 6, C40: 1 / 6 };
const pulse = mm => Math.round(1000 * mm);
const mean = xs => xs.reduce((sum, x) => sum + x, 0) / xs.length;
const finite = Number.isFinite;
const phaseOf = record => record.phase === 'C' ? 'D3' : record.use === 'unseen' ? 'D1' : 'D2';

function requireThat(ok, message) {
  if (!ok) throw new Error(message);
}

function groups(rows, key) {
  const result = new Map();
  for (const row of rows) {
    const id = key(row);
    if (!result.has(id)) result.set(id, []);
    result.get(id).push(row);
  }
  return result;
}

function tableValue(table, x, interp) {
  return interp(x, table.node_pulse, table.current_raw);
}

function freeze(root, sessions, h, version) {
  const dir = path.join(root, 'models');
  fs.mkdirSync(dir, { recursive: true });
  const files = [primaryFile, overlapFile, 'position_BC_overlap_v1.nodes.csv', 'position_B_primary_v1.nodes.csv'];
  requireThat(files.every(name => !fs.existsSync(path.join(dir, name))), 'D v1 artifacts already exist');
  const data = h.load(root, sessions, r => (r.phase === 'B' && r.use === 'train') ||
    (r.phase === 'C' && r.use === 'development'));
  const conditions = h.consistent(data);
  const records = [...data.groups.values()].flat();
  const b = records.filter(r => r.phase === 'B');
  const c = records.filter(r => r.phase === 'C');
  const bGroups = groups(b, r => r.groupKey);
  const cGroups = groups(c, r => r.groupKey);
  requireThat(bGroups.size === 2 && [...bGroups.values()].every(g => g.length === 602),
    'Freeze needs exactly two dense B rounds of 602 trials');
  requireThat(b.every(r => r.t && r.t.hold_s === 4 && r.t.window_s === 1 &&
    r.speed_mm_s === 2 && Number(r.meta.acc_mm_s2) === 20 &&
    Math.abs(pulse(r.target_mm) - pulse(r.approach_mm)) === 1000),
  'Dense B protocol differs from the 4-second reference');
  requireThat(new Set(c.map(r => r.session_id)).size === 1 && cGroups.size === 9 &&
    speeds.every(v => [1, 2, 3].every(g => c.filter(r => pulse(r.speed_mm_s) === v && r.group === g).length === 2)),
  'Freeze needs C development round trips 1–3 at each specified speed');
  const refs = h.references(data, ['B', 'C']);
  requireThat(!data.rejected.length, JSON.stringify(data.rejected));
  const bRefs = groups(refs.filter(r => r.phase === 'B'), r => pulse(r.position));
  const primaryNodes = Array.from({ length: 301 }, (_, i) => 1000 + i * 100);
  requireThat(bRefs.size === primaryNodes.length && primaryNodes.every(x =>
    bRefs.get(x)?.length === 2 && bRefs.get(x).every(r => r.kind === 'paired' && r.group_complete)),
  'B grid, direction pairing or round completeness differs from the fixed protocol');
  const cBySpeed = groups(refs.filter(r => r.phase === 'C'), r => pulse(r.speed));
  const nodeRows = Array.from({ length: 38 }, (_, i) => 3500 + i * 500).map(x => {
    const B = mean(bRefs.get(x).map(r => r.ip));
    const [C2, C10, C40] = speeds.map(speed => {
      const matched = cBySpeed.get(speed)?.filter(r => pulse(r.position) === x) ?? [];
      requireThat(matched.length === 3 && new Set(matched.map(r => r.group)).size === 3,
        'Missing shared development node ' + x + ' at speed ' + speed);
      return mean(matched.map(r => r.ip));
    });
    return { position_pulse: x, B, C2, C10, C40, current_raw: B / 2 + (C2 + C10 + C40) / 6 };
  });
  const evidence = h.evidence(data);
  const inputSessions = [...new Set(evidence.map(x => x.session))];
  const raw_sha256 = Object.fromEntries(inputSessions.map(id => [id,
    h.hash(fs.readFileSync(path.join(root, 'data', 'raw', id + '.csv')))]));
  const common = { schema: 1, analysis_sha256: version, created_utc: new Date().toISOString(), conditions,
    interpolation: 'piecewise_linear', extrapolation: false, hold_s: 4, window_s: 1,
    scan_policy: h.policy, c_session: c[0].session_id, raw_sha256, evidence };
  const primary = { ...common, role: 'B_primary', range_pulse: [1000, 31000], node_pulse: primaryNodes,
    current_raw: primaryNodes.map(x => mean(bRefs.get(x).map(r => r.ip))) };
  const overlap = { ...common, role: 'BC_overlap', range_pulse: [3500, 22000],
    node_pulse: nodeRows.map(r => r.position_pulse), current_raw: nodeRows.map(r => r.current_raw), weights };
  const batch = h.hash(JSON.stringify([primary, overlap]));
  for (const [name, table] of [[primaryFile, primary], [overlapFile, overlap]]) {
    const payload = { ...table, batch_sha256: batch };
    const table_sha256 = h.hash(JSON.stringify(payload));
    fs.writeFileSync(path.join(dir, name), JSON.stringify({ ...payload, table_sha256 }, null, 2) + '\n', { flag: 'wx' });
  }
  h.csvWrite(path.join(dir, files[3]), ['position_pulse', 'current_raw'],
    primaryNodes.map((position_pulse, i) => ({ position_pulse, current_raw: primary.current_raw[i] })));
  h.csvWrite(path.join(dir, files[2]), ['position_pulse', 'B', 'C2', 'C10', 'C40', 'current_raw'], nodeRows);
  console.log('Frozen D v1: B 301 nodes, BC 38 nodes.');
}

function frozenTables(root, h, version) {
  const tables = [primaryFile, overlapFile].map(name => {
    const { table_sha256, ...table } = JSON.parse(fs.readFileSync(path.join(root, 'models', name), 'utf8'));
    requireThat(table.schema === 1 && table.analysis_sha256 === version &&
      h.hash(JSON.stringify(table)) === table_sha256, 'Frozen version or integrity check failed: ' + name);
    return table;
  });
  const [b, c] = tables;
  const valid = (table, role, first, last, spacing, n) => table.role === role &&
    table.range_pulse[0] === first && table.range_pulse[1] === last &&
    table.node_pulse.length === n && table.node_pulse.every((x, i) => x === first + i * spacing) &&
    table.current_raw.length === n && table.current_raw.every(finite);
  requireThat(b.batch_sha256 === c.batch_sha256 && valid(b, 'B_primary', 1000, 31000, 100, 301) &&
    valid(c, 'BC_overlap', 3500, 22000, 500, 38), 'Frozen tables do not form the expected D v1 batch');
  return tables;
}

function statistics(rows, field) {
  const valid = rows.filter(r => finite(r[field]));
  const errors = valid.map(r => r[field]);
  const sorted = errors.map(Math.abs).sort((a, b) => a - b);
  const max = valid.reduce((a, b) => !a || Math.abs(b[field]) > Math.abs(a[field]) ? b : a, null);
  const positions = [...new Set(valid.map(r => r.position_pulse))];
  return { n: valid.length, positions: positions.length,
    groups: new Set(valid.map(r => r.group)).size,
    min_pulse: positions.length ? Math.min(...positions) : NaN,
    max_pulse: positions.length ? Math.max(...positions) : NaN,
    mean_error_raw: mean(errors), rmse_raw: Math.sqrt(mean(errors.map(e => e * e))),
    p95_abs_raw: sorted[Math.ceil(.95 * sorted.length) - 1],
    max_abs_raw: max ? Math.abs(max[field]) : NaN, max_at_pulse: max?.position_pulse };
}

const scopeFields = ['stage', 'speed_pulse_s', 'scope', 'round_trip'];
const statsFields = ['n', 'positions', 'groups', 'min_pulse', 'max_pulse', 'mean_error_raw',
  'rmse_raw', 'p95_abs_raw', 'max_abs_raw', 'max_at_pulse'];
const rowFields = ['stage', 'session', 'group', 'trial', 'speed_pulse_s', 'position_pulse',
  'direction', 'window', 'duration_s', 'current_raw', 'B_raw', 'BC_raw', 'error_B_raw', 'error_BC_raw', 'status'];

function summaries(rows) {
  const sets = new Map();
  for (const row of rows) {
    const key = [row.stage, row.speed_pulse_s].join('/');
    if (!sets.has(key)) sets.set(key, []);
    sets.get(key).push(row);
  }
  return [...sets.values()].flatMap(set => {
    const [r] = set;
    const tags = { stage: r.stage, speed_pulse_s: r.speed_pulse_s };
    return [
      { ...tags, scope: 'all', rows: set },
      { ...tags, scope: 'above_22000', rows: set.filter(x => x.position_pulse > 22000) },
      { ...tags, scope: '28000_to_30000', rows: set.filter(x => x.position_pulse >= 28000 && x.position_pulse <= 30000) },
      ...(r.stage === 'D3' ? [...groups(set, x => x.group)].map(([round_trip, rows]) =>
        ({ ...tags, scope: 'round_trip', round_trip, rows })) : [])
    ];
  });
}

function evaluate(root, sessions, h, version) {
  const [primary, overlap] = frozenTables(root, h, version);
  const cSession = primary.c_session;
  for (const id of sessions.filter(id => id !== cSession)) {
    requireThat(/^[A-Za-z0-9_-]+$/.test(id), 'Invalid D session identifier');
    const manifest = fs.readFileSync(path.join(root, 'manifest', id + '.txt'), 'utf8');
    requireThat(/^phase=D\r?$/m.test(manifest), 'Expected a D1/D2 session or the frozen C session: ' + id);
  }
  const rawC = fs.readFileSync(path.join(root, 'data', 'raw', cSession + '.csv'));
  requireThat(h.hash(rawC) === primary.raw_sha256[cSession], 'Frozen C session SHA-256 has changed');
  const data = h.load(root, [...new Set([...sessions, cSession])],
    r => r.phase === 'D' || (r.phase === 'C' && r.use === 'scan_holdout'));
  h.consistent(data, primary.conditions);
  const trained = new Set(primary.node_pulse);
  const frozenGroups = new Set(primary.evidence.map(r => r.group));
  const centers = [], singles = [], windows = [], coverage = [];
  const enrich = (tags, x, current_raw) => {
    const B_raw = tableValue(primary, x, h.interp), BC_raw = tableValue(overlap, x, h.interp);
    return { ...tags, position_pulse: x, current_raw, B_raw, BC_raw,
      error_B_raw: current_raw - B_raw, error_BC_raw: current_raw - BC_raw };
  };
  for (const [group, records] of data.groups) {
    const first = records[0];
    const common = { stage: phaseOf(first), session: first.session_id, group,
      speed_pulse_s: pulse(first.speed_mm_s) };
    requireThat(!frozenGroups.has(group), 'Acceptance overlaps development: ' + group);
    if (first.phase === 'C') {
      requireThat(first.session_id === cSession && [4, 5].includes(first.group) &&
        speeds.includes(common.speed_pulse_s) && records.length === 2 &&
        records.some(r => r.direction === 1) && records.some(r => r.direction === -1),
      'Unexpected C holdout assignment: ' + group);
      const byX = new Map();
      const step = pulse(h.policy.gridMm);
      for (const r of records.filter(r => r.curve)) {
        const nodes = r.curve.map(s => s.x_mm * 1000);
        const values = r.curve.map(s => s.current_fb_raw);
        for (let x = Math.ceil(Math.max(nodes[0], 1000) / step) * step;
          x <= Math.min(nodes.at(-1), 31000); x += step) {
          const sample = enrich({ ...common, trial: r.trial_id, direction: r.direction,
            window: 'constant_speed', status: 'valid' }, x, h.interp(x, nodes, values));
          singles.push(sample);
          if (!byX.has(x)) byX.set(x, []);
          byX.get(x).push(sample);
        }
      }
      const paired = [...byX].filter(([, rows]) => rows.length === 2);
      const curves = records.map(r => r.curve).filter(Boolean);
      const lo = curves.length === 2 ? Math.max(...curves.map(c => pulse(c[0].x_mm))) : undefined;
      const hi = curves.length === 2 ? Math.min(...curves.map(c => pulse(c.at(-1).x_mm))) : undefined;
      coverage.push({ ...common, scope: 'round_trip', groups: Number(paired.length >= 3),
        grid_points: paired.length, actual_min_pulse: lo <= hi ? lo : undefined,
        actual_max_pulse: lo <= hi ? hi : undefined,
        min_pulse: paired.length ? Math.min(...paired.map(([x]) => x)) : undefined,
        max_pulse: paired.length ? Math.max(...paired.map(([x]) => x)) : undefined });
      if (paired.length < 3) data.rejected.push({ session: common.session, phase: 'C', group,
        reason: 'insufficient_common_constant_speed_coverage' });
      if (paired.length >= 3) for (const [x, rows] of paired)
        centers.push(enrich({ ...common, window: 'constant_speed', status: 'paired' }, x, mean(rows.map(r => r.current_raw))));
      for (const rows of byX.values()) if (rows.length !== 2 || paired.length < 3)
        rows.forEach(row => { row.status = 'no_center'; });
      continue;
    }
    for (const [x, pair] of groups(records, r => pulse(r.target_mm))) {
      requireThat(x >= 1000 && x <= 31000 && (first.use !== 'unseen' || !trained.has(x)),
        'D target outside B range or overlaps B training: ' + x);
      requireThat(pair.length === 2 && pair.some(r => r.direction === 1) &&
        pair.some(r => r.direction === -1), 'Invalid D direction plan: ' + group);
      const valid = pair.filter(r => !r.error);
      const paired = valid.length === 2;
      centers.push(enrich({ ...common, window: 'second_4', status: paired ? 'paired' : 'missing_direction' },
        x, paired ? mean(valid.map(r => r.value)) : NaN));
      for (const r of valid) {
        requireThat(r.t.hold_s === 4 && r.t.window_s === 1 && r.speed_mm_s === 2 &&
          Number(r.meta.acc_mm_s2) === 20 && Math.abs(x - pulse(r.approach_mm)) === 1000,
        'D protocol differs from frozen B: ' + r.key);
        const hold = r.rows.filter(s => s.stage === 'hold');
        const stop = hold.at(-1).t_s;
        for (let second = 1; second <= 4; second++) {
          const end = stop - (4 - second);
          const start = Math.max(end - 1, hold[0].t_s);
          const tags = { ...common, trial: r.trial_id, direction: r.direction, window: 'second_' + second,
            duration_s: end - start, status: paired ? 'valid' : 'no_center' };
          try {
            const row = enrich(tags, x, h.windowMean(r.t, hold, start, end));
            windows.push(row);
            singles.push(row);
          } catch (error) {
            windows.push(enrich({ ...tags, status: error.message }, x, NaN));
            data.rejected.push({ session: common.session, phase: 'D', group, trial: r.trial_id,
              reason: 'second_' + second + '/' + error.message });
          }
        }
      }
    }
  }
  requireThat(coverage.length === 6, 'Missing one or more C holdout round trips');
  const out = path.join(root, 'results');
  const figures = path.join(root, 'data', 'figures');
  fs.mkdirSync(out, { recursive: true });
  fs.mkdirSync(figures, { recursive: true });
  const tag = outputTag(out);
  const write = (name, fields, values) => h.csvWrite(path.join(out, tag + '.' + name + '.csv'), fields, values);
  const full = summaries(centers).map(({ rows, ...tags }) => ({ ...tags, ...statistics(rows, 'error_B_raw') }));
  const fair = summaries(centers).filter(s => ['all', 'round_trip'].includes(s.scope)).map(({ rows, ...tags }) => {
    const common = rows.filter(r => finite(r.error_B_raw) && finite(r.error_BC_raw));
    const b = statistics(common, 'error_B_raw'), c = statistics(common, 'error_BC_raw');
    const row = { ...tags, scope: tags.scope === 'all' ? 'overlap' : 'overlap_round_trip',
      rmse_BC_minus_B_raw: c.rmse_raw - b.rmse_raw };
    for (const field of statsFields) {
      row['B_' + field] = b[field];
      row['BC_' + field] = c[field];
    }
    return row;
  });
  const directionSets = [...groups(singles, r => [r.stage, r.speed_pulse_s, r.direction, r.window].join('/')).values()];
  const byDirection = directionSets.map(rows => ({ ...rows[0], ...statistics(rows, 'error_B_raw') }));
  const directionMetrics = directionSets.flatMap((rows, i) => {
    const common = rows.filter(r => finite(r.error_B_raw) && finite(r.error_BC_raw));
    return [{ ...byDirection[i], scope: 'all', model: 'B' },
      { ...rows[0], ...statistics(common, 'error_B_raw'), scope: 'overlap', model: 'B' },
      { ...rows[0], ...statistics(common, 'error_BC_raw'), scope: 'overlap', model: 'BC' }];
  });
  const speedCoverage = [...groups(coverage, r => r.speed_pulse_s)].map(([speed_pulse_s, rows]) => {
    const valid = rows.filter(r => r.groups);
    const actual_min_pulse = Math.max(...valid.map(r => r.actual_min_pulse));
    const actual_max_pulse = Math.min(...valid.map(r => r.actual_max_pulse));
    const min_pulse = Math.max(...valid.map(r => r.min_pulse));
    const max_pulse = Math.min(...valid.map(r => r.max_pulse));
    return { stage: 'D3', session: cSession, scope: 'common_across_round_trips', speed_pulse_s,
      groups: valid.length, grid_points: valid.reduce((n, r) => n + r.grid_points, 0),
      actual_min_pulse: actual_min_pulse <= actual_max_pulse ? actual_min_pulse : undefined,
      actual_max_pulse: actual_min_pulse <= actual_max_pulse ? actual_max_pulse : undefined,
      min_pulse: min_pulse <= max_pulse ? min_pulse : undefined,
      max_pulse: min_pulse <= max_pulse ? max_pulse : undefined };
  });
  write('windows', rowFields, windows);
  write('center_predictions', rowFields, centers);
  write('direction_residuals', rowFields, singles);
  write('full_metrics', [...scopeFields, ...statsFields], full);
  write('overlap_metrics', [...scopeFields, ...statsFields.map(f => 'B_' + f),
    ...statsFields.map(f => 'BC_' + f), 'rmse_BC_minus_B_raw'], fair);
  write('direction_metrics', ['stage', 'speed_pulse_s', 'direction', 'window', 'scope', 'model', ...statsFields], directionMetrics);
  write('scan_coverage', ['stage', 'session', 'group', 'scope', 'speed_pulse_s', 'groups',
    'grid_points', 'min_pulse', 'max_pulse', 'actual_min_pulse', 'actual_max_pulse'], [...coverage, ...speedCoverage]);
  write('excluded', ['session', 'trial', 'phase', 'group', 'reason'], data.rejected);
  fs.writeFileSync(path.join(out, tag + '.sources.json'), JSON.stringify({
    batch_sha256: primary.batch_sha256, frozen_tables: [primaryFile, overlapFile],
    evaluated_sessions: [...data.conditions.keys()], evaluation_groups: [...data.groups.keys()]
  }, null, 2) + '\n');
  makeFigures(figures, tag, primary, centers, fair, byDirection);
  console.log(tag + ': ' + centers.filter(r => finite(r.error_B_raw)).length + ' paired references.');
}

function outputTag(dir) {
  const parts = new Intl.DateTimeFormat('sv-SE', { timeZone: 'Asia/Shanghai',
    year: 'numeric', month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit',
    second: '2-digit', hourCycle: 'h23' }).formatToParts();
  const values = Object.fromEntries(parts.map(p => [p.type, p.value]));
  const day = values.year + values.month + values.day;
  const time = values.hour + values.minute + values.second;
  const used = fs.readdirSync(dir).flatMap(name => {
    const match = name.match(new RegExp('^Z-D_' + day + '-[0-9]{6}_([0-9]+)\\.'));
    return match ? [Number(match[1])] : [];
  });
  return 'Z-D_' + day + '-' + time + '_' + String(Math.max(0, ...used) + 1).padStart(2, '0');
}

function makeFigures(dir, tag, primary, centers, fair, direction) {
  const palette = ['#20242a', '#126e9b', '#bd541d', '#4f804d', '#724392'];
  const statics = centers.filter(r => r.stage !== 'D3' && finite(r.current_raw));
  if (statics.length) {
    const series = [{ label: 'B 主表', line: true, points: primary.node_pulse.map((x, i) => [x, primary.current_raw[i]]) }];
    for (const [stage, rows] of groups(statics, r => r.stage))
      series.push({ label: stage + ' · ' + new Set(rows.map(r => r.group)).size + ' 组',
        points: rows.map(r => [r.position_pulse, r.current_raw]) });
    plot(dir, tag + '.primary', 'B 主表与 D 静态实测', '位置（脉冲）', '末 1 s 双向中心（raw）', series, 30000);
  }
  const compared = fair.filter(r => r.scope === 'overlap' && r.B_n > 0);
  const labels = compared.map(r => r.stage + (r.stage === 'D3' ? ' ' + r.speed_pulse_s : '') + ' / ' + r.B_groups + '组');
  if (compared.length) {
    const series = ['B_rmse_raw', 'BC_rmse_raw', 'B_p95_abs_raw', 'BC_p95_abs_raw'].map((field, i) => ({
      label: ['B RMSE', '方案③ RMSE', 'B P95', '方案③ P95'][i],
      points: compared.map((r, j) => [j + .7 + i * .2, r[field]])
    }));
    plot(dir, tag + '.overlap', '重叠区相同试次的误差', '工况（D3 速度单位为脉冲/s）', '误差（raw）', series,
      undefined, labels);
  }
  const hold = direction.filter(r => r.stage !== 'D3');
  if (hold.length) {
    const series = [...groups(hold, r => r.stage + '/' + r.direction).values()].map(rows => ({
      label: rows[0].stage + (rows[0].direction > 0 ? ' 正向' : ' 反向') +
        ' · ' + Math.max(...rows.map(r => r.groups)) + ' 组', line: true,
      points: rows.map(r => [Number(r.window.slice(-1)), r.mean_error_raw]).sort((a, b) => a[0] - b[0])
    }));
    plot(dir, tag + '.direction', '正反方向各秒空载残差', '保持窗口（第几秒）', '平均残差（raw）', series);
  }

  function plot(directory, name, title, xLabel, yLabel, series, marker, labels = []) {
    const points = series.flatMap(s => s.points);
    const xs = points.map(p => p[0]), ys = points.map(p => p[1]);
    const loX = Math.min(...xs), hiX = Math.max(...xs);
    const loY = Math.min(0, ...ys), hiY = Math.max(0, ...ys);
    const x = v => 94 + 670 * (v - loX) / Math.max(.001, hiX - loX);
    const y = v => 347 - 278 * (v - loY) / Math.max(1, hiY - loY);
    const tick = (n, fn) => Array.from({ length: n }, (_, i) => fn(i)).join('');
    const marks = labels.length ? labels.map((label, i) =>
      '<text text-anchor="middle" x="' + x(i + 1) + '" y="373">' + label + '</text>').join('') :
      tick(hiX - loX === 3 ? 4 : 5, i => {
        const v = loX + (hiX - loX) * i / (hiX - loX === 3 ? 3 : 4);
        return '<text text-anchor="middle" x="' + x(v) + '" y="373">' + Number(v.toFixed(1)) + '</text>';
      });
    const yMarks = tick(5, i => {
      const v = loY + (hiY - loY) * i / 4;
      return '<path d="M94 ' + y(v) + 'H764" stroke="#ddd"/><text text-anchor="end" x="85" y="' +
        (y(v) + 5) + '">' + Number(v.toFixed(1)) + '</text>';
    });
    const graphics = series.map((s, i) => {
      const color = palette[i];
      const line = s.line ? '<polyline fill="none" stroke="' + color + '" stroke-width="2" points="' +
        s.points.map(p => x(p[0]) + ',' + y(p[1])).join(' ') + '"/>' : '';
      const dots = (s.label === 'B 主表' ? [] : s.points).map(([a, b]) =>
        '<circle fill="' + color + '" cx="' + x(a) + '" cy="' + y(b) + '" r="3.5"/>').join('');
      return line + dots + '<text fill="' + color + '" x="785" y="' + (86 + 25 * i) + '">' + s.label + '</text>';
    }).join('');
    const high = marker ? '<path d="M' + x(marker) + ' 65V347" stroke="#888" stroke-dasharray="4 4"/>' : '';
    fs.writeFileSync(path.join(directory, name + '.svg'),
      '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1000 430" font-family="SimSun, Times New Roman, serif" font-size="14">' +
      '<rect width="100%" height="100%" fill="white"/><text x="94" y="29" font-size="21">' + title +
      '</text><text x="94" y="51">' + yLabel + '</text><path d="M94 65V347H764" fill="none" stroke="#444"/>' +
      marks + yMarks + high + graphics + '<text x="430" y="409" text-anchor="middle">' + xLabel + '</text></svg>');
  }
}

export function runD(mode, root, sessions, h) {
  const version = h.hash(h.sourceHash + h.hash(fs.readFileSync(new URL(import.meta.url))));
  if (mode === 'freeze-d-models') freeze(root, sessions, h, version);
  else evaluate(root, sessions, h, version);
}

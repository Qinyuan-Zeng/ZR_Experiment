import { existsSync, mkdirSync, readFileSync, readdirSync, renameSync, statSync, writeFileSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { analyzeRun, average, check, completedPrefix, config, csv, evaluate, loadModel,
  metadata, number, pairResult, sd, t975 } from './analyze_a1.mjs'

const project = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const rules = 'b_matched_inertia_v1'
const matrix = { B1: [400000, 20000], B2: [400000, 16000], B3: [600000, 20000], B4: [600000, 16000] }
function expandedMatrix(root) {
  const [header, ...lines] = readFileSync(join(root, 'b_expanded_conditions.csv'), 'utf8').trimEnd().split(/\r?\n/u)
  check(header === 'condition,acc_pulse_s2,match_pulse_s' && lines.length === 24, 'invalid expanded condition table')
  let previousAcc = 0
  return Object.fromEntries(lines.map((line, i) => {
    const [id, accRaw, speedRaw, extra] = line.split(',')
    const acc = Number(accRaw), speed = Number(speedRaw), first = i % 2 === 0
    check(extra === undefined && acc >= 200000 && acc <= 800000 && acc % 1000 === 0 &&
      speed === (first ? 16000 : 20000) && (first ? acc > previousAcc : acc === previousAcc) &&
      id === 'X' + acc / 1000 + 'V' + speed / 1000, 'invalid acceleration tier or speed in expanded table')
    previousAcc = acc
    return [id, [acc, speed]]
  }))
}
const idPattern = /^[A-Za-z0-9][A-Za-z0-9_-]{0,99}$/u
const assumptions = {
  rules, model: 'position_B_primary_v1', x0_plan_pulse: 16000, peak_pulse_s: 40000, scale: 1000,
  fit_windows_ms: '15,21,31', current_windows_ms: '5,9,15', current_shift_ms: 0,
  position_tol_mm: 0.01, speed_target_tol_mm_s: 0.5, speed_pair_tol_mm_s: 0.5,
  fit_stability_fraction: 0.1, repeat_min_pairs: 5, snr_min: 5, ci_rel_half_max: 0.30,
  history_x_tol_pulse: 10, history_speed_tol_pulse_s: 500, history_acc_fraction: 0.10,
  preparation_hold_ms: 250
}
const planHeader = 'planned_pair_id,condition,round,series,history_condition,first_role'
const fields = obj => Object.entries(obj).map(([key, value]) => `${key}=${value ?? ''}`).join('\n') + '\n'
const list = value => value ? value.split(',') : []
const critical = df => t975[df] ?? (1.959964 + 2.3723 / df + 2.8225 / df ** 2)
const basename = s => check(idPattern.test(s ?? ''), 'invalid identifier')

function write(path, contents) {
  mkdirSync(dirname(path), { recursive: true })
  writeFileSync(path, contents)
}

function checkRules(meta, model) {
  check(meta.model_sha256 === model.hash && Object.entries(assumptions).every(([key, val]) =>
    typeof val === 'number' ? meta[key] !== undefined && Number(meta[key]) === val : meta[key] === val),
  'model or frozen B rules do not match')
}

function recordedMatchSpeed(pair) {
  // Earlier pair manifests predate the change from 12000 to 16000.
  if (pair.match_pulse_s !== undefined) return number(pair, 'match_pulse_s')
  return pair.condition === 'B2' || pair.condition === 'B4' ? 12000 : 20000
}

function motionCondition(condition, evalPulse, direction, recordedSpeed, cells = matrix) {
  check(Object.hasOwn(cells, condition) && [1, -1].includes(direction) &&
    Number.isInteger(evalPulse) && evalPulse >= 1000 && evalPulse <= 31000, 'invalid B condition')
  const [acc, matrixSpeed] = cells[condition]
  const speed = recordedSpeed ?? matrixSpeed
  check(speed === matrixSpeed || (cells === matrix && matrixSpeed === 16000 && speed === 12000),
    'recorded matching speed is outside the B condition')
  return { xPlan: 16, xEval: evalPulse / 1000, acc: acc / 1000, peak: 40, speed: speed / 1000,
    dir: direction, scale: 1000, xTol: 0.01, targetTol: 0.5, speedTol: 0.5,
    stability: 0.1, minCount: 5, snr: 5, half: 0.30 }
}

function gateValues(condition, p, level, model, status, evidence) {
  return { status, rules, model_sha256: model.hash, condition, level, direction: p.dir,
    x0_plan_pulse: p.xPlan * 1000, eval_pulse: Math.round(p.xEval * 1000), peak_pulse_s: p.peak * 1000,
    match_pulse_s: p.speed * 1000, acc_pulse_s2: p.acc * 1000, evidence }
}

function readGate(root, id, condition, p, level, model) {
  basename(id)
  const gate = metadata(join(root, 'results/b', id + '_gate.txt'))
  const expected = gateValues(condition, p, level, model, 'accepted', gate.evidence)
  check(gate.evidence && Object.entries(expected).every(([k, v]) => gate[k] === String(v)),
    `qualification ${id} does not cover the fixed ${condition} condition`)
}

function runPairSlot(i, condition) {
  return { id: `Q${i}`, condition, round: i, series: 'qualification', history: 'H1',
    first: i % 2 ? 'accel' : 'decel' }
}

function getSlot(meta) {
  return { id: meta.planned_pair_id, condition: meta.condition, round: number(meta, 'round'),
    series: meta.series, history: meta.history_condition, first: meta.first_role }
}

function verifySlot(actual, expected) {
  check(JSON.stringify(actual) === JSON.stringify(expected), 'pair metadata disagrees with immutable slot')
}

function verifyPreparation(meta, slot, p, protocol) {
  check(number(meta, 'preparation_hold_ms') === assumptions.preparation_hold_ms, 'wrong preparation dwell')
  const target = Math.round(number(meta, 'start_mm') * 1000)
  const route = [target]
  let speed = 2
  if (slot.series === 'history') {
    const approach = target - p.dir * number(protocol, 'history_approach_pulse')
    const excursion = approach - p.dir * number(protocol, 'history_excursion_pulse')
    route.unshift(approach)
    if (slot.history === 'H2') route.unshift(approach, excursion)
    speed = number(protocol, 'history_speed_mm_s')
  }
  check(number(meta, 'preparation_speed_mm_s') === speed &&
    meta.preparation_targets_pulse === route.join(','), 'actual preparation path disagrees with protocol')
}

function session(root, id, model) {
  basename(id)
  check(id.startsWith('Z-B_'), 'use a B session ID')
  const meta = metadata(join(root, 'manifest', id + '.txt'))
  check(meta.schema === rules && meta.session_id === id && meta.config === 'a33_core2_axis1' &&
    meta.core === '2' && meta.axis === '1', 'B session identity or platform mismatch')
  checkRules(meta, model)
  return meta
}

export async function replay(root, id, meta, i, expected, p, model, protocol, diagnostic = false) {
  const errors = [], runs = {}
  const order = expected.first === 'accel' ? ['accel', 'decel'] : ['decel', 'accel']
  try {
    const pair = metadata(join(root, 'manifest', `${id}_p${i}.txt`))
    check(pair.session_id === id && number(pair, 'pair_id') === i, 'pair identity mismatch')
    verifySlot(getSlot(pair), expected)
    check(pair.outcome === 'complete', 'pair was interrupted; resample both roles')
    check(recordedMatchSpeed(pair) === p.speed * 1000, 'pair matching speed disagrees with the planned condition')
    if (pair.condition.startsWith('X') || pair.acc_pulse_s2 !== undefined) {
      check(number(pair, 'acc_pulse_s2') === p.acc * 1000, 'pair acceleration disagrees with the planned condition')
    }
    if (!diagnostic) check(number(pair, 'eval_pulse') === Math.round(p.xEval * 1000), 'pair evaluation point mismatch')
    for (const [k, role] of order.entries()) {
      const run = metadata(join(root, 'manifest', `${id}_p${i}_r${k + 1}.txt`))
      verifySlot(getSlot(run), expected)
      verifyPreparation(run, expected, p, protocol)
      runs[role] = await analyzeRun(root, id, i, k + 1, role, p, model, diagnostic)
    }
  } catch (error) {
    errors.push(error.code === 'ENOENT' ? 'slot or trajectory was not recorded' : error.message)
  }
  if (meta.operator_field_confirmation !== 'START') errors.push('missing operator start confirmation')
  const joined = pairResult(id, i, order[0], runs.accel, runs.decel, p, errors)
  return { ...joined, slot: expected, p, diagnostic }
}

function bounds(values) {
  if (!values.length) return null
  const mean = average(values)
  if (values.length < 2) return { n: values.length, mean, ci95: null }
  const half = critical(values.length - 1) * sd(values, mean) / Math.sqrt(values.length)
  return { n: values.length, mean, ci95: [mean - half, mean + half] }
}

export function regression(points, free) {
  const n = points.length
  const terms = free ? 2 : 1
  if (n <= terms) return null
  const xs = points.map(p => p.x / 1e6)
  const ys = points.map(p => p.y)
  const xMean = free ? average(xs) : 0, yMean = free ? average(ys) : 0
  const ssx = xs.reduce((s, x) => s + (x - xMean) ** 2, 0)
  if (ssx <= 1e-16) return null
  const slope = xs.reduce((s, x, i) => s + (x - xMean) * (ys[i] - yMean), 0) / ssx
  const b = free ? yMean - slope * xMean : 0
  const residual = xs.map((x, i) => ys[i] - slope * x - b)
  const sse = residual.reduce((s, r) => s + r * r, 0)
  const mse = sse / (n - terms), t = critical(n - terms)
  const slopeHalf = t * Math.sqrt(mse / ssx)
  const bHalf = free ? t * Math.sqrt(mse * (1 / n + xMean * xMean / ssx)) : 0
  return { n, df: n - terms, alpha: slope / 1e6, alphaCi95: [(slope - slopeHalf) / 1e6, (slope + slopeHalf) / 1e6],
    intercept: b, interceptCi95: free ? [b - bHalf, b + bHalf] : null, rmse: Math.sqrt(sse / n), residual }
}

function outputPairs(prefix, rows, validForModel) {
  const table = [['session_id','pair_id','planned_pair_id','condition','round','series','history_condition','first_role',
    'eval_pulse','nominal_v_pulse_s','nominal_A_pulse_s2','valid','reason','accel_run_id','decel_run_id',
    'delta_x_pulse','delta_v_pulse_s','accel_x_pulse','decel_x_pulse','accel_v_pulse_s','decel_v_pulse_s',
    'accel_a_pulse_s2','decel_a_pulse_s2','delta_a_pulse_s2','delta_I_raw','delta_I_corr_raw','alpha_raw_per_pulse_s2']]
  const windows = [['session_id','pair_id','planned_pair_id','role','family','width_ms','frames',
    'x_pulse','v_pulse_s','a_pulse_s2','residual_pulse','I_raw','I_corrected_raw']]
  const pulse = x => x == null ? null : x * 1000
  for (const r of rows) {
    const { slot: s, p } = r
    const a = r.accel?.position.get(21), b = r.decel?.position.get(21)
    const valid = r.valid && validForModel
    table.push([r.id,r.pair,s.id,s.condition,s.round,s.series,s.history,s.first,Math.round(p.xEval * 1000),
      pulse(p.speed),pulse(p.acc),Number(valid),
      [!validForModel ? 'diagnostic; ineligible for qualification' : '', ...r.issues].filter(Boolean).join(' | '),
      r.accel?.key,r.decel?.key,pulse(r.dx),pulse(r.dv),pulse(a?.x),pulse(b?.x),pulse(a?.v),pulse(b?.v),
      pulse(a?.a),pulse(b?.a),pulse(r.da),r.raw,r.di,valid && r.alpha != null ? r.alpha / 1000 : null])
    for (const run of [r.accel, r.decel].filter(Boolean)) {
      for (const [width, fit] of run.position) {
        windows.push([r.id,r.pair,s.id,run.role,'position',width,fit.frames,pulse(fit.x),pulse(fit.v),
          pulse(fit.a),pulse(fit.residual),null,null])
      }
      for (const [width, fit] of run.current) {
        windows.push([r.id,r.pair,s.id,run.role,'current',width,fit.frames,null,null,null,null,fit.raw,fit.corrected])
      }
    }
  }
  write(prefix + '_pairs.csv', csv(table))
  write(prefix + '_windows.csv', csv(windows))
}

async function a1Baseline(root, model) {
  const ids = ['Z-A1_20261125-112511_825_34600', 'Z-A1_20261125-111250_763_46824']
  const accepted = [], repeatIds = new Set()
  const p = motionCondition('B1', 15976, 1)
  const gate = metadata(join(root, 'results/a1', 'Z-A1_20261125-110931_077_7452_qualification.txt'))
  check(gate.status === 'accepted' && gate.model_sha256 === model.hash, 'A1 baseline qualification missing')
  for (const id of ids) {
    const meta = metadata(join(root, 'manifest', id + '.txt'))
    const { count, ...declared } = config(meta)
    check(Object.entries(p).every(([key, value]) => declared[key] === value) &&
      meta.operator_qualification_session === 'Z-A1_20261125-110931_077_7452', 'A1 baseline did not use B1 condition')
    const entries = new Set(readdirSync(join(root, 'manifest')).filter(f => f.startsWith(id + '_p')))
    const finished = completedPrefix(root, id, count, meta, entries)
    const beginning = meta.repeat_start_role ?? 'accel'
    repeatIds.add(id)
    for (let i = 1; i <= finished; ++i) {
      const order = Boolean(i % 2) === (beginning === 'accel') ? ['accel','decel'] : ['decel','accel']
      const runs = {}
      for (const [k, role] of order.entries()) runs[role] = await analyzeRun(root,id,i,k+1,role,p,model,false)
      accepted.push(pairResult(id,i,order[0],runs.accel,runs.decel,p,[]))
    }
  }
  const result = evaluate(accepted, p, repeatIds)
  check(result.repeatPairs === 5 && result.status === 'passes_candidate_thresholds', 'A1 baseline replay did not meet its gate')
  const summary = { ids, pairs: result.repeatPairs, snr: result.snr, meanAlpha: result.meanAlpha / 1000,
    ci95: result.ci95.map(v => v / 1000), relativeHalfWidth: result.relativeHalfWidth, model_sha256: model.hash }
  const prefix = join(root, 'results/b/A1_B1')
  write(prefix + '_summary.json', JSON.stringify(summary, null, 2) + '\n')
  write(prefix + '_gate.txt', fields(gateValues('B1', p, 'baseline', model, 'accepted', ids.join(','))))
  return summary
}

async function qualification(root, ids, model, override) {
  const rows = []
  let level, condition, p, diagnostic, previousGate
  for (const id of ids) {
    const meta = session(root, id, model)
    const slots = list(meta.planned_slots)
    check(slots.length === number(meta, 'slots_requested') &&
      ['diagnostic','independent','qual_repeat'].includes(meta.purpose), 'not a qualification session')
    const initial = metadata(join(root, 'manifest', id + '_p1.txt'))
    const cells = initial.condition.startsWith('X') ? expandedMatrix(root) : matrix
    const thisP = motionCondition(initial.condition, override ?? number(initial, 'eval_pulse'),
      number(meta, 'direction'), recordedMatchSpeed(initial), cells)
    if (!p) {
      p = thisP
      condition = initial.condition
      level = meta.purpose === 'qual_repeat' ? 'repeat' : 'independent'
      diagnostic = meta.purpose === 'diagnostic' || override !== null
      previousGate = meta.qualification_id
    }
    check(condition === initial.condition && JSON.stringify(p) === JSON.stringify(thisP) &&
      (level === 'repeat' ? meta.purpose === 'qual_repeat' : ids.length === 1) &&
      meta.qualification_id === previousGate, 'qualification sessions use different conditions or purposes')
    if (level === 'repeat') readGate(root, previousGate, condition, p, 'independent', model)
    else check(slots.length === 1, 'first qualification must have exactly one planned pair')
    for (let i = 1; i <= slots.length; ++i) {
      const slot = runPairSlot(i, condition)
      check(slots[i - 1] === slot.id, 'qualification plan order mismatch')
      rows.push(await replay(root,id,meta,i,slot,p,model,null,override !== null))
    }
  }
  const prefix = diagnostic
    ? join(root, 'results/b/calibration', `${ids[0]}_eval_${Math.round(p.xEval * 1000)}`)
    : join(root, 'results/b', ids[0])
  outputPairs(prefix, rows, !diagnostic)
  const result = level === 'repeat' ? evaluate(rows,p,new Set(ids)) : null
  const accepted = !diagnostic && (level === 'repeat' ? result.status === 'passes_candidate_thresholds'
    : rows.length === 1 && rows[0].valid)
  if (!diagnostic) write(prefix + '_gate.txt', fields(gateValues(condition,p,level,model,
    accepted ? 'accepted' : 'rejected', ids.join(','))))
  const summary = { ids, condition, evaluatedPulse: Math.round(p.xEval * 1000), diagnostic, accepted,
    evaluablePairs: rows.filter(r => r.valid).length, qualificationRepeat: result
      ? { ...result, meanAlphaPulseUnits: result.meanAlpha == null ? null : result.meanAlpha / 1000 } : null }
  write(prefix + '_summary.json', JSON.stringify(summary, null, 2) + '\n')
  console.log(prefix)
  return summary
}

export function loadSchedule(root, id, model) {
  basename(id)
  const path = join(root, 'manifest/b', id)
  const protocol = metadata(path + '.txt')
  checkRules(protocol, model)
  check(protocol.protocol_id === id, 'protocol ID mismatch')
  const dir = number(protocol, 'direction'), n = number(protocol, 'rounds')
  const blocks = number(protocol, 'history_blocks')
  check(Number.isInteger(n) && n >= 5 && n <= 30 && Number.isInteger(blocks) &&
    blocks >= 0 && blocks <= 20 && blocks !== 1, 'invalid frozen round count')
  const ranged = protocol.matrix_design === 'expanded_range_v1'
  const expanded = ranged || protocol.matrix_design === 'expanded_12x2'
  check(protocol.matrix_design === undefined || expanded, 'unknown frozen matrix design')
  check(!expanded || (n === 5 && blocks === 0), 'invalid expanded matrix counts')
  let cells = expanded ? expandedMatrix(root) : matrix
  if (ranged) {
    const min = number(protocol, 'range_min_acc_pulse_s2'), max = number(protocol, 'range_max_acc_pulse_s2')
    const tiers = Object.values(cells).map(([acc]) => acc)
    check(min < max && tiers.includes(min) && tiers.includes(max) && number(protocol, 'max_batch_pairs') === 4,
      'invalid frozen acceleration range or batch size')
    cells = Object.fromEntries(Object.entries(cells).filter(([, [acc]]) => acc >= min && acc <= max))
  }
  const conditionIds = Object.keys(cells)
  const conditions = new Map()
  for (const c of conditionIds) {
    const p = motionCondition(c, number(protocol, c + '_eval_pulse'), dir, undefined, cells)
    const speedKey = c + '_match_pulse_s', accKey = c + '_acc_pulse_s2'
    if (expanded || protocol.d_plan_pulse !== undefined || protocol[speedKey] !== undefined || protocol[accKey] !== undefined) {
      check(number(protocol, speedKey) === p.speed * 1000 && number(protocol, accKey) === p.acc * 1000,
        'frozen motion parameters differ from the current matrix for ' + c)
    }
    const id = protocol[c + '_gate']
    const source = ranged ? protocol[c + '_gate_condition'] : c
    if (source !== c) {
      check(ranged && Object.hasOwn(matrix, source) &&
        matrix[source].every((value, i) => value === cells[c][i]), 'original B qualification does not match ' + c)
    }
    const level = source === 'B3' ? 'repeat' : source === 'B1' && id === 'A1_B1' ? 'baseline' : 'independent'
    readGate(root, id, source, p, level, model)
    conditions.set(c, p)
  }
  // Validate the emitted schedule against the versioned rotation, including order.
  const slots = []
  for (let i = 1; i <= n; ++i) for (let j = 0; j < conditionIds.length; ++j) {
    const order = (i - 1 + j) % conditionIds.length
    const index = !ranged ? order : order % 2 === 0 ? order / 2 : conditionIds.length - 1 - Math.floor(order / 2)
    const c = conditionIds[index]
    slots.push({ id: `${id}_M${i}_${c}`, condition: c, round: i, series: 'matrix', history: 'H1',
      first: i % 2 ? 'accel' : 'decel' })
  }
  for (let i = 1; i <= blocks; ++i) for (let j = 0; j < 2; ++j) {
    const h = (i + j) % 2 ? 'H1' : 'H2'
    slots.push({ id: `${id}_H${i}_${h}`, condition: 'B1', round: i, series: 'history', history: h,
      first: i % 2 ? 'accel' : 'decel' })
  }
  const contents = readFileSync(path + '.plan.csv', 'utf8').replace(/\r\n/gu, '\n')
  check(contents === planHeader + '\n' + slots.map(s => Object.values(s).join(',')).join('\n') + '\n',
    'plan file does not match frozen protocol order')
  return { protocol, conditions, slots }
}

export function selectAttempts(rows) {
  const slots = new Set(), runs = new Set()
  for (const row of rows) {
    if (!row.valid) continue
    const keys = [row.accel.key, row.decel.key]
    if (slots.has(row.slot.id) || keys.some(key => runs.has(key))) {
      row.issues.push('duplicate planned slot or original run ID')
      row.valid = false
      continue
    }
    slots.add(row.slot.id)
    for (const key of keys) runs.add(key)
  }
  return slots
}

function historyCompare(rows, protocol) {
  const history = rows.filter(r => r.valid && r.slot.series === 'history')
  const comparisons = []
  for (let block = 1; block <= Number(protocol.history_blocks); ++block) {
    const a = history.find(r => r.slot.round === block && r.slot.history === 'H1')
    const b = history.find(r => r.slot.round === block && r.slot.history === 'H2')
    const issue = []
    if (!a || !b) issue.push('both complete history slots required')
    else for (const role of ['accel', 'decel']) {
      const x = a[role].position.get(21), y = b[role].position.get(21)
      if (Math.abs(x.x - y.x) * 1000 > Number(protocol.history_x_tol_pulse)) issue.push(role + ' position mismatch')
      if (Math.abs(x.v - y.v) * 1000 > Number(protocol.history_speed_tol_pulse_s)) issue.push(role + ' velocity mismatch')
      if (Math.abs(x.a - y.a) > Number(protocol.history_acc_fraction) * (Math.abs(x.a) + Math.abs(y.a)) / 2) {
        issue.push(role + ' acceleration mismatch')
      }
    }
    comparisons.push({ block, h1: a?.id, h2: b?.id, valid: !issue.length, reason: issue.join(' | '),
      deltaI: a && b ? b.di - a.di : null, deltaAlpha: a && b ? (b.alpha - a.alpha) / 1000 : null })
  }
  return comparisons
}

export function validateAccelerationTiers(rows) {
  const tiers = [...new Set(rows.map(r => r.p.acc))].sort((a, b) => a - b)
  const speeds = [...new Set(rows.map(r => r.p.speed))].sort((a, b) => a - b)
  const results = []
  for (const tier of tiers) {
    for (const method of ['leave_tier_out', 'fit_lower_tiers']) {
      const eligible = rows.filter(r => method === 'leave_tier_out' ? r.p.acc !== tier : r.p.acc < tier)
      for (const trainingScope of ['pooled', 'same_speed']) for (const speed of speeds) {
        const train = trainingScope === 'pooled' ? eligible : eligible.filter(r => r.p.speed === speed)
        const test = rows.filter(r => r.p.acc === tier && r.p.speed === speed)
        const fit = regression(train.map(r => ({ x: r.da * 1000, y: r.di })), false)
        const errors = fit && test.length ? test.map(r => r.di - fit.alpha * r.da * 1000) : null
        results.push({
          method, trainingScope, acceleration: tier * 1000, speed: speed * 1000,
          trainingPairs: train.length, trainHeldOutPairs: train.filter(r => r.p.acc === tier).length,
          testPairs: test.length, alpha: fit?.alpha ?? null, bias: errors ? average(errors) : null,
          rmse: errors ? Math.sqrt(average(errors.map(x => x * x))) : null,
          maxAbsError: errors ? Math.max(...errors.map(Math.abs)) : null
        })
      }
    }
  }
  return results
}

export function continuousRangeValidation(rows) {
  const tiers = [...new Set(rows.map(r => r.p.acc))].sort((a, b) => a - b)
  const ranges = []
  for (const [start, first] of tiers.entries()) for (const last of tiers.slice(start + 1)) {
    const selected = rows.filter(r => r.p.acc >= first && r.p.acc <= last)
    const fit = regression(selected.map(r => ({ x: r.da * 1000, y: r.di })), false)
    const folds = validateAccelerationTiers(selected).filter(r => r.method === 'leave_tier_out' && r.trainingScope === 'pooled')
    if (!fit || folds.some(r => r.rmse === null)) continue
    const observations = folds.reduce((n, r) => n + r.testPairs, 0)
    const pooledRmse = Math.sqrt(folds.reduce((sum, r) => sum + r.rmse ** 2 * r.testPairs, 0) / observations)
    const worst = folds.reduce((highest, current) => current.rmse > highest.rmse ? current : highest)
    ranges.push([first * 1000, last * 1000, folds.length / 2, selected.length, fit.alpha, pooledRmse,
      worst.rmse, worst.acceleration, worst.speed, Math.max(...folds.map(r => Math.abs(r.bias)))])
  }
  return ranges
}

function formalResults(prefix, rows, slots, protocol, seen, revision, accepted) {
  outputPairs(prefix, rows, true)
  const pending = slots.filter(s => !accepted.has(s.id))
  write(prefix + '_pending.csv', csv([planHeader.split(','), ...pending.map(s => Object.values(s))]))
  const main = rows.filter(r => r.valid && r.slot.series === 'matrix')
  const mainComplete = pending.every(s => s.series !== 'matrix')
  const conditionIds = [...new Set(slots.filter(s => s.series === 'matrix').map(s => s.condition))]
  const expanded = protocol.matrix_design !== undefined
  const byCondition = Object.fromEntries(conditionIds.map(c => {
    const group = main.filter(r => r.slot.condition === c), x = group.map(r => Math.abs(r.da) * 1000)
    const meanDeltaI = expanded ? bounds(group.map(r => r.di)) : undefined
    const sigma = expanded && group.length > 1 ? sd(group.map(r => r.di), meanDeltaI.mean) : null
    return [c, { n: group.length, ...(expanded ? { meanDeltaI, snr: sigma > 0 ? Math.abs(meanDeltaI.mean) / sigma : null } : {}), absDeltaARange: x.length ? [Math.min(...x), Math.max(...x)] : null,
      meanAlpha: bounds(group.map(r => r.alpha / 1000)) }]
  }))
  const points = main.map(r => ({ x: r.da * 1000, y: r.di }))
  const zero = mainComplete ? regression(points, false) : null
  const free = mainComplete ? regression(points, true) : null
  if (mainComplete) check(zero, 'zero-intercept regression cannot be estimated from observed acceleration')
  const predictions = []
  if (mainComplete && !expanded) for (const [low, high] of [['B1','B3'],['B2','B4']]) {
    const alpha = byCondition[low].meanAlpha.mean
    for (const r of main.filter(row => row.slot.condition === high)) {
      const predicted = alpha * r.da * 1000
      predictions.push([low,high,r.slot.id,r.da * 1000,r.di,predicted,r.di - predicted])
    }
  }
  write(prefix + '_predictions.csv', csv([['training_condition','target_condition','planned_pair_id',
    'delta_a_pulse_s2','observed_raw','predicted_raw','error_raw'], ...predictions]))
  const residuals = []
  const groups = new Map()
  if (zero) for (const r of rows.filter(r => r.valid)) {
    const x = r.da * 1000
    const ez = r.di - zero.alpha * x, ef = free ? r.di - (free.alpha * x + free.intercept) : null
    residuals.push([r.slot.id,r.id,r.slot.condition,r.slot.series,r.slot.history,r.firstRole,
      r.p.speed * 1000,x,r.di,ez,ef])
    for (const [axis, level] of [['condition',r.slot.condition],['speed_pulse_s',r.p.speed * 1000],
      ['first_role',r.firstRole],['history_condition',r.slot.history]]) {
      const key = [r.slot.series,axis,level].join(',')
      if (!groups.has(key)) groups.set(key, [])
      groups.get(key).push(ef ?? ez)
    }
  }
  write(prefix + '_residuals.csv', csv([['planned_pair_id','session_id','condition','series','history_condition',
    'first_role','v_pulse_s','delta_a_pulse_s2','delta_I_corr_raw','residual_zero_raw','residual_free_raw'], ...residuals]))
  write(prefix + '_residual_groups.csv', csv([['series','group_type','group','n','mean_raw','rmse_raw'],
    ...[...groups].map(([id,values]) => [...id.split(','),values.length,average(values),
      Math.sqrt(average(values.map(x => x * x)))])]))
  const contrasts = historyCompare(rows, protocol)
  write(prefix + '_history_contrasts.csv', csv([['block','H1_session','H2_session','matched','reason','H2_minus_H1_delta_I_raw',
    'H2_minus_H1_alpha_raw_per_pulse_s2'], ...contrasts.map(x => [x.block,x.h1,x.h2,Number(x.valid),x.reason,x.deltaI,x.deltaAlpha])]))
  const qualifiedHistory = contrasts.filter(x => x.valid)
  const low = main.filter(r => r.p.acc === 400).map(r => Math.abs(r.da) * 1000)
  const high = main.filter(r => r.p.acc === 600).map(r => Math.abs(r.da) * 1000)
  const excitation = !expanded && low.length && high.length ? { lower: [Math.min(...low), Math.max(...low)],
    higher: [Math.min(...high), Math.max(...high)], separated: Math.max(...low) < Math.min(...high) } : null
  const predictionStats = expanded ? null : Object.fromEntries(['B3','B4'].map(c => {
    const error = predictions.filter(r => r[1] === c).map(r => r[6])
    return [c, error.length ? { n: error.length, bias: average(error),
      mae: average(error.map(Math.abs)), rmse: Math.sqrt(average(error.map(x => x * x))) } : null]
  }))
  const strip = fit => fit && Object.fromEntries(Object.entries(fit).filter(([key]) => key !== 'residual'))
  const matrixSpeeds = [...new Set(main.map(r => r.p.speed * 1000))].sort((a, b) => a - b)
  const speedFits = mainComplete ? Object.fromEntries(matrixSpeeds.map(speed => {
    const points = main.filter(r => r.p.speed * 1000 === speed).map(r => ({ x:r.da * 1000, y:r.di }))
    return [speed, { zero:strip(regression(points,false)), free:strip(regression(points,true)) }]
  })) : null
  let baselineComparison = null
  if (!expanded && protocol.B1_gate === 'A1_B1' && byCondition.B1.n) {
    const baselineFile = join(dirname(prefix), 'A1_B1_summary.json')
    if (existsSync(baselineFile)) {
      const baseline = JSON.parse(readFileSync(baselineFile, 'utf8'))
      check(baseline.model_sha256 === protocol.model_sha256, 'A1 B1 baseline model mismatch')
      baselineComparison = { a1: baseline.meanAlpha, b1: byCondition.B1.meanAlpha,
        difference: byCondition.B1.meanAlpha.mean - baseline.meanAlpha }
    }
  }
  const tierValidation = expanded && mainComplete ? validateAccelerationTiers(main) : null
  if (expanded) {
    const header = ['method','training_scope','held_out_acc_pulse_s2','speed_pulse_s','training_pairs',
      'train_held_out_pairs','test_pairs','alpha_raw_per_pulse_s2','bias_raw','rmse_raw','max_abs_error_raw']
    write(prefix + '_acceleration_validation.csv', csv([header, ...(tierValidation ?? [])
      .map(r => [r.method,r.trainingScope,r.acceleration,r.speed,r.trainingPairs,r.trainHeldOutPairs,
        r.testPairs,r.alpha,r.bias,r.rmse,r.maxAbsError])]))
    if (protocol.matrix_design === 'expanded_range_v1') {
      const ranges = mainComplete ? continuousRangeValidation(main) : []
      write(prefix + '_continuous_range_validation.csv', csv([['first_acc_pulse_s2','last_acc_pulse_s2','tiers','pairs',
        'alpha_raw_per_pulse_s2','held_out_rmse_raw','worst_cell_rmse_raw','worst_acc_pulse_s2','worst_speed_pulse_s',
        'max_absolute_cell_bias_raw'], ...ranges]))
    }
  }
  const summary = { protocol: protocol.protocol_id, ...(expanded ? { matrixDesign: protocol.matrix_design, tierValidation } : {}),
    sessions: seen, status: mainComplete ? 'matrix_complete' : 'matrix_pending',
    accepted: accepted.size, pending: pending.map(s => s.id), byCondition, excitation, fitZero: strip(zero),
    fitFree: strip(free), speedFits, predictionStats, baselineComparison,
    history: { plannedBlocks: Number(protocol.history_blocks),
      matchedBlocks: qualifiedHistory.length, deltaI: bounds(qualifiedHistory.map(x => x.deltaI)),
      deltaAlpha: bounds(qualifiedHistory.map(x => x.deltaAlpha)) },
    inference: 'OLS t intervals per independent pair; overlapping excitation intervals weaken the free-intercept fit',
    dReservation: Object.fromEntries(Object.entries(protocol).filter(([key]) => key.startsWith('d_'))) }
  write(prefix + '_summary.json', JSON.stringify(summary, null, 2) + '\n')
  const receipt = prefix + '_resume.txt'
  write(receipt + '.tmp', fields({ protocol_id: protocol.protocol_id, seen_revision: revision.join(','),
    accepted_slots: slots.filter(s => accepted.has(s.id)).map(s => s.id).join(',') }))
  renameSync(receipt + '.tmp', receipt)
  return summary
}

async function formal(root, id, model) {
  const { protocol, conditions, slots } = loadSchedule(root, id, model)
  check(!existsSync(join(root, 'manifest/b', id + '.lock')), 'close the active acquisition before replay')
  const plan = new Map(slots.map(s => [s.id, s]))
  const seen = [], revision = [], rows = []
  const names = readdirSync(join(root, 'manifest')).filter(name =>
    /^Z-B_\d{8}-\d{6}_\d+_\d+\.txt$/u.test(name)).sort()
  for (const name of names) {
    const runId = name.slice(0, -4)
    const manifest = join(root, 'manifest', name)
    const m = metadata(manifest)
    if (m.protocol_id !== id || m.operator_field_confirmation !== 'START') continue
    session(root, runId, model)
    check(m.purpose === 'formal' && Number(m.direction) === Number(protocol.direction),
      'formal attempt identity or direction mismatch')
    const requested = list(m.planned_slots)
    check(protocol.matrix_design !== 'expanded_range_v1' || requested.length <= 4,
      'formal attempt exceeds the frozen maximum batch size')
    check(requested.length === number(m, 'slots_requested') &&
      requested.every(s => plan.has(s)) && requested.length === new Set(requested).size, 'invalid session slot list')
    const expectedOrder = new Map(slots.map((s, i) => [s.id, i]))
    check(requested.every((key, i) => !i || expectedOrder.get(key) > expectedOrder.get(requested[i - 1])),
      'formal session did not follow frozen slot order')
    seen.push(runId)
    revision.push(`${runId}:${statSync(manifest).size}`)
    for (const [j, key] of requested.entries()) {
      const slot = plan.get(key)
      rows.push(await replay(root,runId,m,j+1,slot,conditions.get(slot.condition),model,protocol))
    }
  }
  const accepted = selectAttempts(rows)
  check(!existsSync(join(root, 'manifest/b', id + '.lock')), 'acquisition became active during replay')
  const prefix = join(root, 'results/b', id)
  const result = formalResults(prefix, rows, slots, protocol, seen, revision, accepted)
  console.log(prefix)
  return result
}

async function main() {
  const args = process.argv.slice(2)
  const take = flag => {
    const index = args.indexOf(flag)
    if (index < 0) return null
    const value = args.splice(index, 2)[1]
    check(value && !value.startsWith('--'), flag + ' requires a value')
    return value
  }
  const root = resolve(take('--root') ?? project)
  const position = take('--calibrate-position')
  const model = loadModel()
  let result
  if (args.length === 1 && args[0] === '--a1-baseline' && position === null) result = await a1Baseline(root, model)
  else if (args.length === 1 && args[0].startsWith('Z-BP_') && position === null) result = await formal(root, args[0], model)
  else {
    check(args.length && new Set(args).size === args.length &&
      args.every(x => idPattern.test(x) && x.startsWith('Z-B_')) &&
      (position === null || args.length === 1), 'use --a1-baseline, a protocol ID or B session IDs [--calibrate-position pulse]')
    result = await qualification(root, args, model, position === null ? null : Number(position))
  }
  console.log(JSON.stringify(result, null, 2))
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  main().catch(error => { console.error(error.message); process.exitCode = 1 })
}

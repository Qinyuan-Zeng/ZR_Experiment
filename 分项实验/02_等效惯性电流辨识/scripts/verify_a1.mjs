import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const project = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const root = join(project, 'verification', 'a1')
const { current_raw: model } = JSON.parse(readFileSync(join(project, '..',
  '01_位置相关空载电流辨识', 'models', 'position_B_primary_v1.json'), 'utf8'))
const alpha = 0.5
const qualify = 'synthetic_a1_first'
const repeat = 'synthetic_a1_repeat'
const enter = 2200, end = 9800, tick = 0.00005

function run(exe, args, options = {}) {
  const result = spawnSync(exe, args, { cwd: project, encoding: 'utf8', ...options })
  assert.equal(result.status, 0, result.stderr + result.stdout)
  return result.stdout
}

for (const build of ['Debug', 'Release']) {
  const exe = join(project, 'bin', 'x64', build, 'a1_matched_pair', 'ZRInertiaCurrent.exe')
  const plan = run(exe, ['--plan'])
  assert.match(plan, /accel 15\.5 -> 19\.5 mm/u)
  assert.match(plan, /decel 12\.5 -> 16\.5 mm/u)
  assert.match(plan, /x0_plan=16 mm, x_match_eval=15\.976 mm/u)
  assert.match(plan, /pending, theoretical only/u)
  assert.equal((plan.match(/platform before\/after=50\/50 ms, duration=200 ms/gu) ?? []).length, 2)
  const reverse = run(exe, ['--plan', '--input'], { input: '16,400,40,20,-1,1\n' })
  assert.match(reverse, /accel 16\.5 -> 12\.5 mm/u)
  assert.match(reverse, /decel 19\.5 -> 15\.5 mm/u)
  const alternate = run(exe, ['--plan', '--input'], { input: '16,400,40,20,1,1,15.970\n' })
  assert.match(alternate, /x0_plan=16 mm, x_match_eval=15\.97 mm/u)
  assert.match(alternate, /accel 15\.5 -> 19\.5 mm/u)
  const continuation = run(exe, ['--plan', '--input'], { input: '16,400,40,20,1,2,15.976,-1\n' })
  assert.match(continuation, /pairs=2/u)
  assert.match(continuation, /Pair 1\r?\nTransfer[^\n]*\r?\ndecel 12\.5 -> 16\.5 mm/u)
  assert.match(continuation, /Pair 2\r?\nTransfer[^\n]*\r?\naccel 15\.5 -> 19\.5 mm/u)
  const wrongPlatform = spawnSync(exe, ['--plan', '--input'], { input: '16,400,40,20,1,1,18\n' })
  assert.equal(wrongPlatform.status, 1)
  const outside = spawnSync(exe, ['--plan', '--input'], { input: '1,400,40,20,1,1\n' })
  assert.equal(outside.status, 1)
  const reserve = run(exe, ['--plan', '--input'], { input: '\n16,1,1000\n' })
  assert.match(reserve, /required reserve 1\.81 mm each side \[OK\]/u)
  const boundary = spawnSync(exe, ['--plan', '--input'], { input: '\n0,1,1000\n' })
  assert.equal(boundary.status, 1)
  console.log(`${build} x64 A1 plan PASS`)
}

function writeMeta(name, fields) {
  mkdirSync(join(root, 'manifest'), { recursive: true })
  writeFileSync(join(root, 'manifest', name + '.txt'),
    Object.entries(fields).map(([k, v]) => `${k}=${v}`).join('\n') + '\n')
}

function makeRun(id, pair, order, role, shift) {
  const key = `${id}_p${pair}_r${order}`
  const accel = role === 'accel'
  const initial = accel ? 15.5 : 12.5
  const target = initial + 4
  // Position and current are quantized like the real capture, with a known
  // position table contribution and known inertial-current coefficient.
  const rows = ['frame,card_tick_raw,encoder_position_raw,current_feedback_raw']
  const startTick = 2 ** 32 - 1500
  const cardTick = n => (n + startTick) >>> 0
  for (let n = 0; n <= end; ++n) {
    const t = (n - enter) * tick
    const acceleration = t < 0 || t >= .2 ? 0 : t < .1 ? 400 : -400
    const x = t < 0 ? initial : t < .1 ? initial + 200 * t ** 2 :
      t < .2 ? target - 200 * (.2 - t) ** 2 : target
    const pulse = Math.round(x * 1000)
    const units = (pulse - 1000) / 100
    const i = Math.floor(units)
    const baseline = model[i] + (units - i) * (model[i + 1] - model[i])
    const current = Math.round(baseline + 13 + alpha * acceleration + shift)
    rows.push(`${n},${cardTick(n)},${pulse},${current}`)
  }
  mkdirSync(join(root, 'data', 'raw'), { recursive: true })
  writeFileSync(join(root, 'data', 'raw', key + '.csv'), rows.join('\n') + '\n')
  writeMeta(key, {
    session_id: id, pair_id: pair, run_id: key, role, execution_order: order, direction: 1,
    outcome: 'complete', watch_aligned_contiguous: 1, core_period_ns: 50000,
    command_before_tick: cardTick(enter - 2), command_after_tick: cardTick(enter + 2),
    settled_tick: cardTick(6400), start_mm: initial, end_mm: target,
    planned_platform_begin_ms: accel ? 0 : 100, planned_platform_end_ms: accel ? 100 : 200,
    planned_match_ms: accel ? 50 : 150,
    ...(id !== qualify ? { x0_plan_mm: 16, x_match_eval_mm: 16,
      planned_eval_crossing_ms: accel ? 50 : 150, tail_stable_begin_tick: cardTick(6500),
      tail_stable_end_tick: cardTick(9500), tail_span_limit_pulse: 7.5 } : {})
  })
}

function session(id, count, completed = count, firstRole = 'accel') {
  writeMeta(id, {
    session_id: id, schema: 'a1_watch_pair_v1', outcome: completed === count ? 'complete' : 'incomplete',
    core: 2, axis: 1,
    config: 'a33_core2_axis1', model: 'position_B_primary_v1', encoder_pulse_per_mm: 1000,
    x0_mm: 16, peak_mm_s: 40, match_mm_s: 20, direction: 1, acc_mm_s2: 400,
    ...(count > 1 ? { x0_plan_mm: 16, x_match_eval_mm: 16 } : {}),
    pairs_requested: count, position_match_tol_mm: 0.01, speed_target_tol_mm_s: 0.5,
    speed_pair_tol_mm_s: 0.5, fit_stability_fraction: 0.1,
    fit_windows_ms: '15,21,31', current_windows_ms: '5,9,15',
    min_pair_count: 5, snr_threshold: 5, ci_relative_halfwidth_threshold: 0.3,
    operator_qualification_session: count > 1 ? qualify : '',
    repeat_start_role: firstRole,
    operator_field_confirmation: 'START', 'GTN_Close.rc': 0, 'GTN_SetWatchGroup.restore.rc': 0
  })
  for (let pair = 1; pair <= completed; ++pair) {
    const accelFirst = Boolean(pair % 2) === (firstRole === 'accel')
    makeRun(id, pair, accelFirst ? 1 : 2, 'accel', count > 1 ? (pair - 3) * 2 : 0)
    makeRun(id, pair, accelFirst ? 2 : 1, 'decel', 0)
  }
}

const analyzer = join(project, 'scripts', 'analyze_a1.mjs')
const file = (id, suffix) => join(root, 'results', 'a1', id + suffix)
session(qualify, 1)
assert.match(run(process.execPath, [analyzer, qualify, '--root', root]), /1\/1 valid pairs/u)
const row = readFileSync(file(qualify, '_pairs.csv'), 'utf8').split('\n')[1].split(',')
assert(Math.abs(Number(row[5])) < 0.02, 'known pair has a velocity mismatch')
assert(Math.abs(Number(row[6]) - 800) < 2, 'delta acceleration estimate')
assert(Math.abs(Number(row[9]) - alpha) <= 0.002, 'known alpha replay')
assert.equal(JSON.parse(readFileSync(file(qualify, '_summary.json'), 'utf8')).repeat.repeatPairs, 0)
assert.match(readFileSync(file(qualify, '_alignment.svg'), 'utf8'), /^<svg/u)
assert.match(readFileSync(file(qualify, '_qualification.txt'), 'utf8'), /x0_plan_mm=16\nx_match_eval_mm=16/u)

session(repeat, 5)
assert.match(run(process.execPath, [analyzer, repeat, qualify, '--root', root]), /6\/6 valid pairs/u)
const group = JSON.parse(readFileSync(file(repeat, '_summary.json'), 'utf8')).repeat
assert.equal(group.repeatPairs, 5)
assert.equal(group.status, 'passes_candidate_thresholds')
assert(group.ci95[0] < alpha && group.ci95[1] > alpha)

const interrupted = 'synthetic_a1_interrupted'
const continueId = 'synthetic_a1_continuation'
session(interrupted, 5, 3)
session(continueId, 2, 2, 'decel')
assert.match(run(process.execPath, [analyzer, interrupted, '--root', root]), /3\/5 valid pairs/u)
const tailPath = join(root, 'data', 'raw', continueId + '_p1_r1.csv')
const originalTail = readFileSync(tailPath, 'utf8')
const movingTail = originalTail.replace(/^(7000,\d+,)16500,/mu, (_, prefix) => prefix + '16508,')
assert.notEqual(movingTail, originalTail)
try {
  writeFileSync(tailPath, movingTail)
  assert.match(run(process.execPath, [analyzer, continueId, '--root', root]), /1\/2 valid pairs/u)
  assert.match(readFileSync(file(continueId, '_pairs.csv'), 'utf8'), /Watch tail encoder displacement exceeds tolerance/u)
} finally {
  writeFileSync(tailPath, originalTail)
}
const incompletePath = join(root, 'manifest', interrupted + '.txt')
const incompleteMeta = readFileSync(incompletePath, 'utf8')
try {
  writeFileSync(incompletePath, incompleteMeta.replace('GTN_Close.rc=0', 'GTN_Close.rc=-8'))
  assert.match(run(process.execPath, [analyzer, interrupted, '--root', root]), /0\/5 valid pairs/u)
} finally {
  writeFileSync(incompletePath, incompleteMeta)
}
run(process.execPath, [analyzer, continueId, interrupted, '--root', root])
const pooled = JSON.parse(readFileSync(file(continueId, '_summary.json'), 'utf8'))
assert.equal(pooled.repeat.repeatPairs, 5)
assert.deepEqual(pooled.repeat.byOrder.map(group => group.pairs), [3, 2])
assert.equal(pooled.sessionOutcomes[1].completedPairs, 3)
assert.equal(pooled.sessionOutcomes[1].outcome, 'incomplete')

const existing = JSON.parse(run(process.execPath, [analyzer, 'Z-HS_20261125-100708_416_12472']))
assert.equal(existing.eligible_for_pair, false)
assert.match(existing.reason, /ended while axis was moving/u)
console.log('Known alpha, independent repeats, and truncated real recording PASS')

const observed = 'Z-A1_20261125-105005_057_17836'
const observedPrefix = join(project, 'results', 'a1', observed)
assert.match(run(process.execPath, [analyzer, observed]), /0\/1 valid pairs/u)
const original = readFileSync(observedPrefix + '_pairs.csv', 'utf8')
const originalGate = readFileSync(observedPrefix + '_qualification.txt', 'utf8')
const observedRow = original.split('\n')[1].split(',')
assert.equal(observedRow[2], '0')
assert(Math.abs(Number(observedRow[5]) - 0.9433994) < 1e-5)
assert.match(originalGate, /status=rejected/u)

assert.match(run(process.execPath, [analyzer, observed, '--calibrate-position', '15.976']),
  /1\/1 diagnostic pairs within matching limits/u)
const diagnostic = join(project, 'results', 'a1', 'calibration', observed + '_eval_15.976')
const calibrated = readFileSync(diagnostic + '_fits.csv', 'utf8').trimEnd().split('\n').slice(1)
  .map(line => line.split(','))
const expected = {
  accel: { 15: [19.824, 386.6], 21: [19.844, 390.1], 31: [19.859, 393.3] },
  decel: { 15: [19.858, -414.6], 21: [19.838, -397.0], 31: [19.831, -391.7] }
}
assert.equal(calibrated.length, 6)
for (const r of calibrated) {
  const [velocity, acceleration] = expected[r[3]][r[4]]
  assert(Math.abs(Number(r[10]) - velocity) < .003, 'diagnostic velocity')
  assert(Math.abs(Number(r[11]) - acceleration) < .2, 'diagnostic acceleration')
}
assert.equal(readFileSync(observedPrefix + '_pairs.csv', 'utf8'), original)
assert.equal(readFileSync(observedPrefix + '_qualification.txt', 'utf8'), originalGate)
assert(!existsSync(diagnostic + '_qualification.txt'))
const result = JSON.parse(readFileSync(diagnostic + '_summary.json', 'utf8'))
assert.equal(result.validPairs, 0)
assert.equal(result.diagnosticChecksPassed, 1)
console.log('Original field rejection and isolated 15.976 mm calibration PASS')

const stoppedBatch = 'Z-A1_20261125-111250_763_46824'
assert.match(run(process.execPath, [analyzer, stoppedBatch]), /3\/5 valid pairs/u)
const stoppedPrefix = join(project, 'results', 'a1', stoppedBatch)
const stoppedSummary = JSON.parse(readFileSync(stoppedPrefix + '_summary.json', 'utf8'))
assert.equal(stoppedSummary.sessionOutcomes[0].outcome, 'incomplete')
assert.equal(stoppedSummary.repeat.repeatPairs, 3)
assert.match(readFileSync(stoppedPrefix + '_pairs.csv', 'utf8'), /decel: run incomplete \(not_at_confirmed_start\)/u)
console.log('Interrupted real session retains exactly three independently valid pairs PASS')

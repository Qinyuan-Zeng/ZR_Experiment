import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const scripts = dirname(fileURLToPath(import.meta.url))
const project = join(scripts, '..')
const verification = join(project, 'verification', 'watch_motion')
const staticId = 'Z-HS_20261125-094343_092_16300'
const syntheticId = 'synthetic_round_trip'
const tick = 50e-6
const runLength = 20

function analyze(id, root) {
  const result = spawnSync(process.execPath, [join(scripts, 'analyze_watch_motion.mjs'), id, root],
    { encoding: 'utf8' })
  assert.equal(result.status, 0, result.stderr)
  const dest = join(root, 'results', 'watch_motion', id)
  assert.match(readFileSync(join(dest, 'diagnostic.svg'), 'utf8'), /^<svg/u)
  const csv = name => readFileSync(join(dest, name), 'utf8').trimEnd().split('\n').slice(1)
    .map(line => line.split(','))
  return {
    summary: readFileSync(join(dest, 'summary.md'), 'utf8'),
    windows: csv('windows.csv'), segments: csv('segments.csv')
  }
}

function leg(elapsed) {
  const speed = 10
  const acceleration = 400
  const ramp = speed / acceleration
  const duration = 1 / speed + ramp
  if (elapsed <= 0) return 0
  if (elapsed < ramp) return acceleration * elapsed ** 2 / 2
  if (elapsed < duration - ramp) return speed * (elapsed - ramp / 2)
  if (elapsed < duration) return 1 - acceleration * (duration - elapsed) ** 2 / 2
  return 1
}

const stationary = analyze(staticId, project)
assert.match(stationary.summary, /未检测到运动/u)
const static5 = stationary.windows.filter(row => row[0] === '5')
assert.equal(static5.length, 100060)
assert(static5.every(row => row[9] === '100' && row[10] === '0' && row[11] === '5' && row[12] === '0'))
console.log('Static replay PASS')

mkdirSync(join(verification, 'manifest'), { recursive: true })
mkdirSync(join(verification, 'data', 'raw'), { recursive: true })
writeFileSync(join(verification, 'manifest', syntheticId + '.txt'), [
  'mode=synthetic', 'core_period_ns=50000', 'encoder_pulse_per_mm=1000',
  'expected_start_mm=0', 'expected_turnaround_mm=1', 'expected_speed_mm_s=10',
  'expected_accel_decel_mm_s2=400', 'outcome=synthetic', ''
].join('\n'))

const ticks = []
const raw = ['frame,card_tick_raw,encoder_position_raw,current_feedback_raw']
for (let n = 0; n <= 8200; ++n) {
  if (n >= 2400 && n < 2440) continue
  const t = n * tick
  const position = leg(t - 0.05) - leg(t - 0.225)
  // Alternating known levels every millisecond, including a negative current.
  const current = Math.floor(n / runLength) % 2 ? -60 : -80
  raw.push(`${ticks.length},${n},${Math.round(position * 1000)},${current}`)
  ticks.push(n)
}
writeFileSync(join(verification, 'data', 'raw', syntheticId + '.csv'), raw.join('\n') + '\n')
const synthetic = analyze(syntheticId, verification)
assert.match(synthetic.summary, /记录到往返范围，运动覆盖有时钟断点/u)
assert.match(synthetic.summary, /0 \/ 1 \/ 0/u)
assert.match(synthetic.summary, /1 \/ -1/u)
assert.match(synthetic.summary, /持续运动方向 1 → -1/u)
assert.match(synthetic.summary, /gap，缺失 tick 40/u)
for (const direction of ['1', '-1']) {
  for (const phase of ['acceleration', 'cruise', 'deceleration']) {
    assert(synthetic.segments.some(row => row[2] === phase && row[3] === direction),
      `Missing phase ${phase} in direction ${direction}`)
    assert(synthetic.windows.some(row => row[0] === '5' && row[6] === phase &&
      row[7] === direction && row[8] === 'within_segment'),
    `Missing 5 ms window for ${phase} in direction ${direction}`)
  }
}
assert(synthetic.segments.some(row => row[2] === 'reverse'))
assert(synthetic.windows.some(row => row[8] === 'cross_motion_segment'))
for (const width of [5, 15]) {
  const span = width / 1000 / tick
  const rows = synthetic.windows.filter(row => Number(row[0]) === width)
  assert(rows.some(row => row[8] === 'cross_clock_break'))
  for (const row of rows) {
    const first = ticks[Number(row[3])]
    const last = ticks[Number(row[4])]
    const crossesGap = first < 2400 && first + span > 2400
    assert.equal(row[8] === 'cross_clock_break', crossesGap)
    const changesThrough = n => Math.floor(n / runLength)
    const changes = last < 2440 || first >= 2440
      ? changesThrough(last) - changesThrough(first)
      : changesThrough(2399) - changesThrough(first) + changesThrough(last) - changesThrough(2440)
    assert.equal(Number(row[10]), changes)
    if (row[8] === 'within_segment') assert.equal(Number(row[9]), span)
  }
}
console.log('Synthetic round-trip replay PASS')

import { createHash } from 'node:crypto'
import { createReadStream, mkdirSync, readdirSync, readFileSync, writeFileSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { createInterface } from 'node:readline'
import { fileURLToPath, pathToFileURL } from 'node:url'

const project = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const modelPath = join(project, '..', '01_位置相关空载电流辨识', 'models', 'position_B_primary_v1.json')
const modelId = 'position_B_primary_v1'
const wrap = 2 ** 32
const header = 'frame,card_tick_raw,encoder_position_raw,current_feedback_raw'
const fitWidths = [15, 21, 31]
const currentWidths = [5, 9, 15]
const primaryFit = 21, primaryCurrent = 5

function check(value, message) {
  if (!value) throw new Error(message)
}

function metadata(path) {
  return Object.fromEntries(readFileSync(path, 'utf8').split(/\r?\n/u)
    .filter(line => line.includes('=')).map(line => {
      const sep = line.indexOf('=')
      return [line.slice(0, sep), line.slice(sep + 1)]
    }))
}

function number(meta, name) {
  const value = meta[name]?.trim() ? Number(meta[name]) : NaN
  check(Number.isFinite(value), `missing or invalid ${name}`)
  return value
}

function loadModel() {
  const { table_sha256: hash, ...table } = JSON.parse(readFileSync(modelPath, 'utf8'))
  check(hash === createHash('sha256').update(JSON.stringify(table)).digest('hex') &&
    table.role === 'B_primary' && table.interpolation === 'piecewise_linear' &&
    table.node_pulse.length === 301 && table.current_raw.length === 301 &&
    table.node_pulse.every((p, i) => p === 1000 + 100 * i) && table.current_raw.every(Number.isFinite),
  'unexpected frozen B primary table')
  return {
    hash, at(mm, scale, allowMissing = false) {
      const units = (mm * scale - table.node_pulse[0]) / 100
      if (allowMissing && (units < 0 || units > 300)) return null
      check(units >= 0 && units <= 300, 'current window outside frozen position model')
      const i = Math.min(Math.floor(units), 299)
      return table.current_raw[i] + (units - i) * (table.current_raw[i + 1] - table.current_raw[i])
    }
  }
}

const clockDelta = (a, b) => (a - b + wrap) % wrap

async function frames(path, tickMs, scale, readCurrent = true) {
  const input = createInterface({ input: createReadStream(path), crlfDelay: Infinity })
  const data = { t: [], x: [], current: [], breaks: 0, firstTick: null }
  let lineNo = 0, lastTick, elapsed = 0
  for await (const line of input) {
    if (++lineNo === 1) {
      check(line === header, 'unsupported raw frame layout')
      continue
    }
    const fields = line.split(',')
    const [i, raw, p] = fields.slice(0, 3).map(Number)
    const current = readCurrent ? Number(fields[3]) : null
    check(fields.length === 4 && fields.slice(0, readCurrent ? 4 : 3).every(x => x.trim() !== '') &&
      i === data.t.length && Number.isInteger(raw) && raw >= -(2 ** 31) && raw < wrap &&
      Number.isFinite(p) && (!readCurrent || (Number.isInteger(current) && current >= -32768 && current <= 32767)),
    `bad raw frame at line ${lineNo}`)
    const tick = raw >>> 0
    if (lastTick === undefined) data.firstTick = tick
    else {
      const delta = clockDelta(tick, lastTick)
      if (delta !== 1) ++data.breaks
      elapsed += delta > 0 && delta < wrap / 2 ? delta : 1
    }
    data.t.push(elapsed * tickMs)
    data.x.push(p / scale)
    if (readCurrent) data.current.push(current)
    lastTick = tick
  }
  check(data.t.length > 1, 'raw recording has fewer than two frames')
  return data
}

function lowerBound(times, target) {
  let low = 0, high = times.length
  while (low < high) {
    const mid = (low + high) >>> 1
    if (times[mid] < target) low = mid + 1
    else high = mid
  }
  return low
}

function windowIndices(data, center, width, begin, end) {
  const left = center - width / 2, right = center + width / 2
  check(left >= begin - 1e-8 && right <= end + 1e-8 &&
    left >= data.t[0] && right <= data.t.at(-1), `${width} ms window crosses platform/recording boundary`)
  const first = lowerBound(data.t, left - 1e-9)
  const last = lowerBound(data.t, right - 1e-9)
  check(last - first >= 5, 'too few samples in window')
  return [first, last]
}

function solve(matrix) {
  for (let i = 0; i < matrix.length; ++i) {
    let pivot = i
    for (let row = i + 1; row < matrix.length; ++row) {
      if (Math.abs(matrix[row][i]) > Math.abs(matrix[pivot][i])) pivot = row
    }
    ;[matrix[i], matrix[pivot]] = [matrix[pivot], matrix[i]]
    const divisor = matrix[i][i]
    check(Math.abs(divisor) > 1e-12, 'singular local fit')
    for (let col = i; col <= matrix.length; ++col) matrix[i][col] /= divisor
    for (let row = 0; row < matrix.length; ++row) {
      if (row === i) continue
      const factor = matrix[row][i]
      for (let col = i; col <= matrix.length; ++col) matrix[row][col] -= factor * matrix[i][col]
    }
  }
  return matrix.map(row => row.at(-1))
}

// Rescale time to [-1,1] first so the quadratic fit is well conditioned.
function fit(data, center, width, begin, end, value, degree) {
  const [first, last] = windowIndices(data, center, width, begin, end)
  const sums = Array(2 * degree + 1).fill(0)
  const targets = Array(degree + 1).fill(0)
  const span = width / 2
  for (let i = first; i < last; ++i) {
    const u = (data.t[i] - center) / span
    const y = value(i)
    let power = 1
    for (let k = 0; k < sums.length; ++k) {
      sums[k] += power
      if (k <= degree) targets[k] += power * y
      power *= u
    }
  }
  const c = solve(targets.map((y, i) => [...sums.slice(i, i + degree + 1), y]))
  let ss = 0
  for (let i = first; i < last; ++i) {
    const u = (data.t[i] - center) / span
    const predicted = c.reduceRight((acc, coefficient) => coefficient + u * acc, 0)
    ss += (value(i) - predicted) ** 2
  }
  return { x: c[0], v: c[1] * 1000 / span, a: degree === 2 ? 2 * c[2] * (1000 / span) ** 2 : null,
    residual: Math.sqrt(ss / (last - first)), frames: last - first, sampleCenter: (data.t[first]+data.t[last-1])/2 }
}

function crossing(data, x0, direction, begin, end) {
  const times = []
  const from = Math.max(1, lowerBound(data.t, begin))
  const to = lowerBound(data.t, end)
  for (let i = from; i < to; ++i) {
    const before = direction * (data.x[i - 1] - x0)
    const after = direction * (data.x[i] - x0)
    if (before < 0 && after >= 0) {
      const frac = -before / (after - before)
      times.push(data.t[i - 1] + frac * (data.t[i] - data.t[i - 1]))
    }
  }
  check(times.length === 1, `expected one directed crossing in planned platform, found ${times.length}`)
  return times[0]
}

function checkMotion(data, start, end, direction, command, settled, tolerance) {
  check(command >= 50 && data.t.at(-1) - settled >= 75 && settled > command,
    'missing stable baseline, command bracket or settled tail')
  const base = lowerBound(data.t, command - 10)
  const final = lowerBound(data.t, settled)
  check(data.x.slice(0, base || 1).every(x => Math.abs(x - start) <= tolerance) &&
    data.x.slice(final).every(x => Math.abs(x - end) <= tolerance), 'start or end stationary position out of tolerance')
  check(direction * (end - start) > tolerance, 'wrong trajectory direction')
  let greatest = direction * data.x[base]
  for (let i = base + 1; i <= final; ++i) {
    const position = direction * data.x[i]
    check(position >= greatest - tolerance, 'observed motion reversed within run')
    greatest = Math.max(greatest, position)
  }
}

function config(meta) {
  const p = {
    xPlan: number(meta, meta.x0_plan_mm === undefined ? 'x0_mm' : 'x0_plan_mm'),
    xEval: number(meta, meta.x_match_eval_mm === undefined ?
      (meta.x0_mm === undefined ? 'x0_plan_mm' : 'x0_mm') : 'x_match_eval_mm'),
    speed: number(meta, 'match_mm_s'),
    peak: number(meta, 'peak_mm_s'), acc: number(meta, 'acc_mm_s2'), dir: number(meta, 'direction'),
    scale: number(meta, 'encoder_pulse_per_mm'), xTol: number(meta, 'position_match_tol_mm'),
    targetTol: number(meta, 'speed_target_tol_mm_s'), speedTol: number(meta, 'speed_pair_tol_mm_s'),
    stability: number(meta, 'fit_stability_fraction'), count: number(meta, 'pairs_requested'),
    minCount: number(meta, 'min_pair_count'), snr: number(meta, 'snr_threshold'),
    half: number(meta, 'ci_relative_halfwidth_threshold')
  }
  check(meta.schema === 'a1_watch_pair_v1' && meta.config === 'a33_core2_axis1' &&
    meta.core === '2' && meta.axis === '1' && meta.model === modelId &&
    [p.xPlan, p.xEval].every(x => x >= 1 && x <= 31) &&
    (meta.x0_mm === undefined || number(meta, 'x0_mm') === p.xPlan) &&
    p.scale === 1000 && [1, -1].includes(p.dir) && p.acc > 0 && p.peak > p.speed && p.speed > 0 &&
    Number.isInteger(p.count) && p.count >= 1 && p.count <= 30 && p.minCount >= 5 &&
    p.xTol > 0 && p.targetTol > 0 && p.speedTol > 0 && p.stability > 0 && p.snr > 0 && p.half > 0 &&
    meta.fit_windows_ms === fitWidths.join(',') && meta.current_windows_ms === currentWidths.join(','),
  'incompatible A1 session or predeclared windows')
  return p
}

async function analyzeRun(root, id, pair, order, role, p, model, diagnostic) {
  const key = `${id}_p${pair}_r${order}`
  const meta = metadata(join(root, 'manifest', key + '.txt'))
  check(meta.session_id === id && meta.run_id === key && number(meta, 'pair_id') === pair &&
    number(meta, 'execution_order') === order && meta.role === role &&
    number(meta, 'direction') === p.dir, 'run identity/order or role mismatch')
  check(meta.outcome === 'complete' && meta.watch_aligned_contiguous === '1',
    `run incomplete (${meta.reason ?? 'unknown'})`)
  if (meta.x0_plan_mm !== undefined || meta.x_match_eval_mm !== undefined) {
    check(number(meta, 'x0_plan_mm') === p.xPlan &&
      (diagnostic || number(meta, 'x_match_eval_mm') === p.xEval), 'run and session positions disagree')
  }
  const tick = number(meta, 'core_period_ns')
  check(Number.isInteger(tick) && tick > 0, 'invalid card timebase')
  const data = await frames(join(root, 'data', 'raw', key + '.csv'), tick / 1e6, p.scale)
  check(!data.breaks, 'clock gap, repetition or backwards tick')
  const timestamp = name => {
    const event = number(meta, name)
    check(Number.isInteger(event) && event >= 0 && event < wrap, 'invalid card event tick')
    const offset = clockDelta(event, data.firstTick) * tick / 1e6
    check(offset <= data.t.at(-1), 'event outside captured clock')
    return offset
  }
  const before = timestamp('command_before_tick'), after = timestamp('command_after_tick')
  const settled = timestamp('settled_tick')
  check(before <= after && after < settled, 'invalid motion event order')
  const start = number(meta, 'start_mm'), end = number(meta, 'end_mm')
  checkMotion(data, start, end, p.dir, before, settled, p.xTol)
  if (['tail_stable_begin_tick', 'tail_stable_end_tick', 'tail_span_limit_pulse'].some(k => meta[k] !== undefined)) {
    const tailStart = timestamp('tail_stable_begin_tick')
    const tailEnd = timestamp('tail_stable_end_tick')
    const spanLimitPulse = number(meta, 'tail_span_limit_pulse')
    check(tailStart >= settled && tailEnd - tailStart >= 150 - tick / 1e6 && spanLimitPulse === 7.5,
      'missing full 150 ms stable tail or unexpected tail tolerance')
    const from = lowerBound(data.t, tailStart), to = lowerBound(data.t, tailEnd)
    let minimum = Infinity, maximum = -Infinity
    for (let i = from; i <= to; ++i) {
      minimum = Math.min(minimum, data.x[i])
      maximum = Math.max(maximum, data.x[i])
    }
    check(to > from && (maximum - minimum) * p.scale <= spanLimitPulse + 1e-8,
      'Watch tail encoder displacement exceeds tolerance')
  }
  const begin = after + number(meta, 'planned_platform_begin_ms')
  const finish = before + number(meta, 'planned_platform_end_ms')
  const plannedMatch = (before + after) / 2 + number(meta, 'planned_match_ms')
  check(plannedMatch > begin && plannedMatch < finish && finish <= settled,
    'inconsistent planned platform or command timing uncertainty')
  const plannedEval = !diagnostic && meta.planned_eval_crossing_ms !== undefined
    ? (before + after) / 2 + number(meta, 'planned_eval_crossing_ms') : null
  check(plannedEval === null || (plannedEval > begin && plannedEval < finish),
    'inconsistent planned evaluation crossing')
  const center = crossing(data, p.xEval, p.dir, begin, finish)
  const position = new Map(), current = new Map()
  const issues = []
  for (const width of fitWidths) {
    try {
      const result = fit(data, center, width, begin, finish, i => data.x[i], 2)
      position.set(width, result)
      if (Math.abs(result.x - p.xEval) > p.xTol) {
        issues.push(`${width} ms fitted center position outside predeclared tolerance`)
      }
      if (Math.abs(result.v - p.dir * p.speed) > p.targetTol || Math.sign(result.v) !== p.dir) {
        issues.push(`${width} ms center speed outside predeclared tolerance`)
      }
      if (Math.sign(result.a) !== (role === 'accel' ? p.dir : -p.dir)) {
        issues.push(`${width} ms acceleration sign disagrees with role`)
      }
    } catch (error) { issues.push(error.message) }
  }
  for (let i = 1; i < fitWidths.length; ++i) {
    const a = position.get(fitWidths[i - 1])?.a, b = position.get(fitWidths[i])?.a
    if (a !== undefined && b !== undefined &&
        Math.abs(a - b) > p.stability * (Math.abs(a) + Math.abs(b)) / 2) {
      issues.push(`${fitWidths[i - 1]}/${fitWidths[i]} ms acceleration unstable`)
    }
  }
  for (const width of currentWidths) {
    try {
      const raw = fit(data, center, width, begin, finish, i => data.current[i], 1)
      const corrected = fit(data, center, width, begin, finish,
        i => data.current[i] - model.at(data.x[i], p.scale), 1)
      current.set(width, { raw: raw.x, corrected: corrected.x, frames: raw.frames })
    } catch (error) { issues.push(error.message) }
  }
  return { key, role, data, position, current, center, plannedMatch, plannedEval, begin, finish, issues }
}

function pairResult(id, pair, firstRole, accel, decel, p, errors) {
  const issues = [...errors, ...[accel, decel].flatMap(run => run?.issues.map(e => `${run.role}: ${e}`) ?? [])]
  for (const width of fitWidths) {
    const a = accel?.position.get(width), b = decel?.position.get(width)
    if (!a || !b) continue
    if (Math.abs(a.x - b.x) > p.xTol) issues.push(`${width} ms position mismatch`)
    if (Math.abs(a.v - b.v) > p.speedTol) issues.push(`${width} ms velocity mismatch`)
    if (!(Math.sign(a.a) * Math.sign(b.a) === -1)) issues.push(`${width} ms accelerations are not opposite`)
  }
  const a = accel?.position.get(primaryFit), b = decel?.position.get(primaryFit)
  const ciA = accel?.current.get(primaryCurrent), ciB = decel?.current.get(primaryCurrent)
  const da = a && b ? a.a - b.a : null
  const di = ciA && ciB ? ciA.corrected - ciB.corrected : null
  const valid = !issues.length && Number.isFinite(da) && da !== 0 && Number.isFinite(di)
  return { id, pair, firstRole, accel, decel, valid, issues, dx: a && b ? a.x - b.x : null,
    dv: a && b ? a.v - b.v : null, da, di,
    raw: ciA && ciB ? ciA.raw - ciB.raw : null, alpha: valid ? di / da : null }
}

const average = data => data.reduce((acc, value) => acc + value, 0) / data.length
const sd = (data, mean) => Math.sqrt(data.reduce((acc, value) => acc + (value - mean) ** 2, 0) / (data.length - 1))
const t975 = [null, 12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262,
  2.228, 2.201, 2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086, 2.080,
  2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042]

function evaluate(results, p, idsWithRepeats) {
  const repetition = results.filter(r => r.valid && idsWithRepeats.has(r.id))
  if (repetition.length < p.minCount) return { status: 'insufficient_independent_repeat_pairs',
    repeatPairs: repetition.length, minimum: p.minCount }
  const deltaI = repetition.map(r => r.di)
  const alpha = repetition.map(r => r.alpha)
  const meanI = average(deltaI), meanAlpha = average(alpha)
  const sigmaI = sd(deltaI, meanI), sigmaAlpha = sd(alpha, meanAlpha)
  const df = alpha.length - 1
  const critical = t975[df] ?? (1.959964 + 2.3723 / df + 2.8225 / df ** 2)
  const halfWidth = critical * sigmaAlpha / Math.sqrt(alpha.length)
  const rel = meanAlpha ? halfWidth / Math.abs(meanAlpha) : null
  const snr = sigmaI ? Math.abs(meanI) / sigmaI : null
  const byOrder = ['accel_first', 'decel_first'].map((order, i) => {
    const group = repetition.filter(r => r.firstRole === ['accel', 'decel'][i])
    return { order, pairs: group.length, meanDeltaI: group.length ? average(group.map(r => r.di)) : null,
      meanAlpha: group.length ? average(group.map(r => r.alpha)) : null }
  })
  return { status: snr != null && rel != null && snr >= p.snr && rel <= p.half
    ? 'passes_candidate_thresholds' : 'does_not_pass_or_is_indeterminate',
  repeatPairs: alpha.length, meanI, sigmaI, snr, meanAlpha, sigmaAlpha,
  ci95: [meanAlpha - halfWidth, meanAlpha + halfWidth], relativeHalfWidth: rel, byOrder,
  zeroVarianceInterpretation: sigmaI === 0 ? 'sample SD is zero; finite SNR unavailable' : null }
}

const csvCell = x => x == null ? '' : /[,"\r\n]/u.test(String(x)) ? `"${String(x).replaceAll('"', '""')}"` : x
const csv = rows => rows.map(r => r.map(csvCell).join(',')).join('\n') + '\n'

function output(root, sessions, rows, p, model, diagnostic) {
  const prefix = diagnostic
    ? join(root, 'results', 'a1', 'calibration', `${sessions[0]}_eval_${p.xEval.toFixed(3)}`)
    : join(root, 'results', 'a1', sessions[0])
  mkdirSync(dirname(prefix), { recursive: true })
  const pairs = [['session_id','pair_id','valid','reason','delta_x_mm','delta_v_mm_s','delta_a_mm_s2',
    'delta_I_raw','delta_I_corr_raw','alpha_raw_per_mm_s2','x0_plan_mm','x_match_eval_mm','diagnostic_checks_passed','first_role']]
  const fits = [['session_id','pair_id','run_id','role','window_ms','crossing_since_first_frame_ms','planned_match_since_first_frame_ms',
    'platform_begin_card_ms','platform_end_card_ms','x_mm','v_mm_s','a_mm_s2','rmse_mm','frames',
    'planned_eval_since_first_frame_ms']]
  const currents = [['session_id','pair_id','window_ms','accel_raw','decel_raw','delta_raw',
    'accel_corr','decel_corr','delta_corr','frames_accel','frames_decel','alpha_using_21ms_fit']]
  const contrasts = [['session_id','pair_id','fit_ms','delta_x_mm','delta_v_mm_s','accel_a_mm_s2',
    'decel_a_mm_s2','delta_a_mm_s2','alpha_using_5ms_current']]
  for (const r of rows) {
    const reason = [diagnostic ? 'calibration diagnostic only; ineligible for qualification' : '',
      ...r.issues].filter(Boolean).join(' | ')
    pairs.push([r.id, r.pair, diagnostic ? 0 : Number(r.valid), reason, r.dx, r.dv, r.da, r.raw, r.di,
      diagnostic ? null : r.alpha, p.xPlan, p.xEval, diagnostic ? Number(r.valid) : null, r.firstRole])
    for (const run of [r.accel, r.decel].filter(Boolean)) {
      for (const [width, f] of run.position) {
        fits.push([r.id, r.pair, run.key, run.role, width, run.center, run.plannedMatch, run.begin,
          run.finish, f.x, f.v, f.a, f.residual, f.frames, run.plannedEval])
      }
    }
    for (const width of currentWidths) {
      const a = r.accel?.current.get(width), b = r.decel?.current.get(width)
      if (a && b) currents.push([r.id, r.pair, width, a.raw, b.raw, a.raw - b.raw, a.corrected,
        b.corrected, a.corrected - b.corrected, a.frames, b.frames,
        r.valid && !diagnostic ? (a.corrected - b.corrected) / r.da : null])
    }
    for (const width of fitWidths) {
      const a = r.accel?.position.get(width), b = r.decel?.position.get(width)
      if (a && b) contrasts.push([r.id,r.pair,width,a.x - b.x,a.v - b.v,a.a,b.a,a.a - b.a,
        r.valid && !diagnostic ? r.di / (a.a - b.a) : null])
    }
  }
  for (const [name, data] of [['pairs', pairs], ['fits', fits], ['current_windows', currents], ['fit_contrasts', contrasts]]) {
    writeFileSync(`${prefix}_${name}.csv`, csv(data))
  }
  const drawable = rows.find(r => r.valid) ?? rows.find(r =>
    [r.accel, r.decel].every(run => run?.position.has(31) && run.current.has(primaryCurrent)))
  if (drawable) writeFileSync(prefix + '_alignment.svg', alignment(drawable, p, model, diagnostic))
  return prefix
}

function alignment(row, p, model, diagnostic) {
  const panels = ['位置 (mm)', '局部速度 (mm/s)', '局部加速度 (mm/s²)', '修正电流 (raw)']
  const half = 15.5, colors = ['#115b8c', '#c35326']
  const xp = t => 103 + (t + half) / (2 * half) * 852
  const values = [row.accel, row.decel].map(run => {
    const points = []
    for (let i = 0; i <= 124; ++i) {
      const t = -half + i / 4, sample = run.center + t
      const idx = Math.min(lowerBound(run.data.t, sample), run.data.t.length - 1)
      const x = run.data.x[idx]
      const baseline = model.at(x, p.scale, true)
      const current = baseline == null ? null : run.data.current[idx] - baseline
      let speed = null, acceleration = null
      if (sample - primaryFit / 2 >= run.begin && sample + primaryFit / 2 <= run.finish) {
        const q = fit(run.data, sample, primaryFit, run.begin, run.finish, j => run.data.x[j], 2)
        speed = q.v
        acceleration = q.a
      }
      points.push({ t, ys: [x, speed, acceleration, current] })
    }
    return points
  })
  const rows = [`<svg xmlns="http://www.w3.org/2000/svg" width="990" height="770" viewBox="0 0 990 770">
<title>A1 pair ${row.id} / ${row.pair}</title><rect width="990" height="770" fill="#fff"/>
<g font-family="Microsoft YaHei, sans-serif" fill="#203345" font-size="13">
<text x="103" y="34" font-size="21" font-weight="bold">A1 匹配点对齐 · ${row.id} / ${row.pair}</text>
<text x="955" y="34" text-anchor="end" fill="${diagnostic ? '#88530e' : row.valid ? '#21613c' : '#a52d28'}" font-weight="bold">${diagnostic ? '标定回放 · 不计资格' : row.valid ? '配对通过' : '配对未通过 · 诊断'}</text>
<text x="105" y="60" fill="${colors[0]}">加速匹配</text><text x="200" y="60" fill="${colors[1]}">减速匹配</text>
<text x="300" y="60">浅色区为 5 ms 电流主窗口，速度与加速度采用 21 ms 局部拟合</text>`]
  for (const [index, label] of panels.entries()) {
    const top = 86 + index * 159, height = 125
    const y = values.flatMap(points => points.map(v => v.ys[index])).filter(Number.isFinite)
    const min = Math.min(...y), max = Math.max(...y), pad = (max - min) * .08 || 1
    const yp = x => top + height * ((max + pad) - x) / (max - min + 2 * pad)
    rows.push(`<rect x="103" y="${top}" width="852" height="${height}" fill="#f5f7f8"/>
<rect x="${xp(-2.5)}" y="${top}" width="${xp(2.5) - xp(-2.5)}" height="${height}" fill="#e5eef2"/>
<path d="M${xp(0)} ${top}v${height}" stroke="#647b89" stroke-dasharray="3 4"/>`)
    for (let j = 0; j <= 3; ++j) {
      const level = min + (max - min) * j / 3
      if (j && max === min) continue
      rows.push(`<path d="M103 ${yp(level)}H955" stroke="#d4dde2"/>
<text x="96" y="${yp(level) + 4}" text-anchor="end">${Number(level.toPrecision(5))}</text>`)
    }
    for (const [k, points] of values.entries()) {
      let connected = false
      let path = ''
      for (const v of points) {
        const y = v.ys[index]
        if (y == null) { connected = false; continue }
        path += `${connected ? 'L' : 'M'}${xp(v.t).toFixed(2)},${yp(y).toFixed(2)}`
        connected = true
      }
      rows.push(`<path d="${path}" fill="none" stroke="${colors[k]}" stroke-width="1.6"/>`)
    }
    rows.push(`<text transform="translate(23,${top + height / 2}) rotate(-90)" text-anchor="middle">${label}</text>`)
  }
  for (const t of [-15, -10, -5, 0, 5, 10, 15]) {
    rows.push(`<text x="${xp(t)}" y="708" text-anchor="middle">${t}</text>`)
  }
  rows.push(`<text x="529" y="738" text-anchor="middle">相对各自经过评价点 ${p.xEval.toFixed(3)} mm 的卡内时间 (ms)</text></g></svg>`)
  return rows.join('\n')
}

async function passiveRecording(root, id) {
  const meta = metadata(join(root, 'manifest', id + '.txt'))
  const raw = await frames(join(root, 'data', 'raw', id + '.csv'),
    number(meta, 'core_period_ns') / 1e6, 1000)
  const end = raw.t.at(-1)
  const span = Math.min(end, 20)
  const i = lowerBound(raw.t, end - span)
  const speed = (raw.x.at(-1) - raw.x[i]) * 1000 / (end - raw.t[i])
  const reference = Number(meta['reference_after.velocity_pulse_ms'])
  const moving = Math.abs(speed) > .05 || Number.isFinite(reference) && Math.abs(reference) > .05
  console.log(JSON.stringify({ id, eligible_for_pair: false,
    reason: moving ? 'passive recording ended while axis was moving' : 'passive recording lacks A1 motion events',
    tailSpeedMmS: speed, finalPositionMm: raw.x.at(-1) }, null, 2))
}

function completedPrefix(root, id, count, session, entries) {
  if (session.outcome === 'complete') return count
  // Salvage only a leading sequence from an interrupted repeat that closed the
  // card and restored the Watch group cleanly. Each run still gets full replay.
  if (count < 2 || session.outcome !== 'incomplete' ||
      session.operator_field_confirmation !== 'START' ||
      session['GTN_Close.rc'] !== '0' || session['GTN_SetWatchGroup.restore.rc'] !== '0') return 0
  let leadingRuns = 0, ended = false
  for (let i = 0; i < count * 2; ++i) {
    const key = `${id}_p${Math.floor(i / 2) + 1}_r${i % 2 + 1}.txt`
    const run = entries.has(key) ? metadata(join(root, 'manifest', key)) : null
    if (!run || run.outcome !== 'complete') {
      ended = true
    } else if (ended) {
      return 0
    } else {
      ++leadingRuns
    }
  }
  return Math.floor(leadingRuns / 2)
}

async function main() {
  const args = process.argv.slice(2)
  const rootFlag = args.indexOf('--root')
  const rootArg = rootFlag < 0 ? project : args.splice(rootFlag, 2)[1]
  check(rootArg, '--root requires a directory')
  const root = resolve(rootArg)
  const positionFlag = args.indexOf('--calibrate-position')
  const diagnostic = positionFlag >= 0
  const requestedPosition = diagnostic ? Number(args.splice(positionFlag, 2)[1]) : null
  check(args.length && args.every(id => /^[A-Za-z0-9][A-Za-z0-9_-]*$/u.test(id)) &&
    new Set(args).size === args.length,
  'usage: node analyze_a1.mjs <session_id> [more IDs] [--root <dir>] [--calibrate-position <mm>]')
  check(!diagnostic || (args.length === 1 && !args[0].startsWith('Z-HS_') &&
    requestedPosition >= 1 && requestedPosition <= 31 &&
    Math.abs(requestedPosition * 1000 - Math.round(requestedPosition * 1000)) < 1e-8),
  'calibration requires one A1 session and a position in 1..31 mm on the pulse grid')
  if (args.length === 1 && args[0].startsWith('Z-HS_')) return passiveRecording(root, args[0])
  const model = loadModel()
  const results = [], repeats = new Set(), sessionOutcomes = []
  let base
  for (const id of args) {
    const meta = metadata(join(root, 'manifest', id + '.txt'))
    check(meta.session_id === id, 'session identity mismatch')
    const p = config(meta)
    const firstRole = meta.repeat_start_role ?? 'accel'
    check(['accel', 'decel'].includes(firstRole), 'invalid declared execution order')
    if (diagnostic) {
      check(p.count === 1, 'calibration is limited to a first-pair session')
      p.xEval = requestedPosition
    }
    const { count, ...condition } = p
    if (!base) base = condition
    check(JSON.stringify(condition) === JSON.stringify(base), 'do not pool different directions, recipes or rules')
    const entries = new Set(readdirSync(join(root, 'manifest')).filter(f => f.startsWith(id + '_p') && f.endsWith('.txt')))
    check(entries.size <= count * 2, 'more runs than the predeclared number')
    const qualification = meta.operator_qualification_session
    let validQualification = false
    if (count > 1 && /^[A-Za-z0-9][A-Za-z0-9_-]*$/u.test(qualification ?? '') && qualification !== id) {
      try {
        const gate = metadata(join(root, 'results', 'a1', qualification + '_qualification.txt'))
        const legacy = JSON.stringify({ x0: condition.xPlan,
          ...Object.fromEntries(Object.entries(condition).filter(([field]) => field !== 'xPlan' && field !== 'xEval')) })
        validQualification = gate.status === 'accepted' && gate.model_sha256 === model.hash &&
          (gate.condition_json === JSON.stringify(condition) ||
            (condition.xEval === condition.xPlan && gate.condition_json === legacy)) &&
          gate.rules === 'a1_watch_pair_v1'
      } catch {}
    }
    const completedPairs = completedPrefix(root, id, count, meta, entries)
    sessionOutcomes.push({ id, outcome: meta.outcome, plannedPairs: count, completedPairs })
    if (validQualification && completedPairs > 0) repeats.add(id)
    for (let pair = 1; pair <= count; ++pair) {
      const runs = {}, errors = []
      const accelFirst = Boolean(pair % 2) === (firstRole === 'accel')
      const order = accelFirst ? ['accel', 'decel'] : ['decel', 'accel']
      for (const [i, role] of order.entries()) {
        try {
          check(entries.has(`${id}_p${pair}_r${i + 1}.txt`), 'run was not recorded')
          runs[role] = await analyzeRun(root, id, pair, i + 1, role, p, model, diagnostic)
        }
        catch (error) { errors.push(`${role}: ${error.message}`) }
      }
      if (pair > completedPairs) errors.push('pair is outside the complete portion of the session')
      if (count > 1 && !validQualification) errors.push('missing or incompatible first-pair qualification')
      results.push(pairResult(id, pair, order[0], runs.accel, runs.decel, p, errors))
    }
    if (count === 1 && !diagnostic) {
      const folder = join(root, 'results', 'a1')
      mkdirSync(folder, { recursive: true })
      writeFileSync(join(folder, id + '_qualification.txt'), [
        `status=${results.at(-1).valid ? 'accepted' : 'rejected'}`, 'rules=a1_watch_pair_v1',
        `x0_mm=${p.xPlan}`, `x0_plan_mm=${p.xPlan}`, `x_match_eval_mm=${p.xEval}`,
        `acc_mm_s2=${p.acc}`, `peak_mm_s=${p.peak}`,
        `match_mm_s=${p.speed}`, `direction=${p.dir}`, `model_sha256=${model.hash}`,
        `condition_json=${JSON.stringify(condition)}`, ''
      ].join('\n'))
    }
  }
  const prefix = output(root, args, results, base, model, diagnostic)
  const report = { sessions: args, sessionOutcomes, model: modelId, model_sha256: model.hash, condition: base,
    mode: diagnostic ? 'calibration_diagnostic' : 'predeclared_evaluation',
    validPairs: diagnostic ? 0 : results.filter(r => r.valid).length,
    diagnosticChecksPassed: diagnostic ? results.filter(r => r.valid).length : null,
    primary: { motionFitMs: primaryFit, currentWindowMs: primaryCurrent, timeShiftMs: 0 },
    repeat: diagnostic ? { status: 'ineligible_for_qualification_or_repeat_statistics' } : evaluate(results, base, repeats),
    directionCheck: 'independent reverse-direction study pending',
    historyBias: 'execution-order groups are descriptive; independent history-bias test pending' }
  writeFileSync(prefix + '_summary.json', JSON.stringify(report, null, 2) + '\n')
  const counts = diagnostic ? `${report.diagnosticChecksPassed}/${results.length} diagnostic pairs within matching limits`
    : `${report.validPairs}/${results.length} valid pairs`
  console.log(`${counts}; ${report.repeat.status}\n${prefix}`)
}

export { analyzeRun, average, check, completedPrefix, config, csv, evaluate, loadModel,
  metadata, number, pairResult, sd, t975, frames, fit, lowerBound, clockDelta, checkMotion }

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  main().catch(error => { console.error(error.message); process.exitCode = 1 })
}

import { closeSync, createReadStream, mkdirSync, openSync, readFileSync, writeFileSync, writeSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { createInterface } from 'node:readline'
import { fileURLToPath } from 'node:url'

const header = 'frame,card_tick_raw,encoder_position_raw,current_feedback_raw'
const widths = [5, 15]
const nsPerMs = 1e6
const wrap = 2 ** 32
const here = dirname(fileURLToPath(import.meta.url))
const phaseNames = {
  stationary: '静止', acceleration: '加速', cruise: '匀速', deceleration: '减速',
  reverse: '折返', transition: '过渡'
}
const colors = {
  stationary: '#e9edf1', acceleration: '#f9d6a7', cruise: '#cae5fb',
  deceleration: '#dacdf5', reverse: '#f8ccd3', transition: '#f1f4f6'
}

function check(condition, message) {
  if (!condition) throw new Error(message)
}

function readConfig(path) {
  const values = Object.fromEntries(readFileSync(path, 'utf8').split(/\r?\n/)
    .filter(line => line.includes('=')).map(line => {
      const split = line.indexOf('=')
      return [line.slice(0, split), line.slice(split + 1)]
    }))
  const number = key => values[key]?.trim() ? Number(values[key]) : NaN
  const tick = number('core_period_ns')
  check(Number.isSafeInteger(tick) && tick > 0 && tick <= 1e9, '会话缺少有效的 core_period_ns')
  check(!['incomplete', 'cleanup_failed'].includes(values.outcome),
    '会话采集未通过，先核对 manifest 与批次返回码')
  const scale = number('encoder_pulse_per_mm') || 1000
  check(Number.isFinite(scale) && scale > 0, '编码器比例无效')
  const expected = {
    start: number('expected_start_mm'), turn: number('expected_turnaround_mm'),
    speed: number('expected_speed_mm_s'), acceleration: number('expected_accel_decel_mm_s2')
  }
  const hasReference = Object.values(expected).every(Number.isFinite)
  check(!hasReference || (expected.start !== expected.turn && expected.speed > 0 &&
    expected.acceleration > 0), '预期工况无效')
  return { tick, scale, expected: hasReference ? expected : null, outcome: values.outcome }
}

async function readFrames(path, config) {
  const t = [], x = [], current = [], block = [], breaks = []
  const input = createInterface({ input: createReadStream(path), crlfDelay: Infinity })
  let previous, lineNumber = 0
  for await (const line of input) {
    ++lineNumber
    if (lineNumber === 1) {
      check(line.replace(/^\uFEFF/u, '') === header, '逐帧 CSV 表头不匹配')
      continue
    }
    const fields = line.split(',')
    const [index, rawTick, position, rawCurrent] = fields.map(Number)
    check(fields.length === 4 && fields.every(value => value.trim() !== '') &&
      index === t.length && Number.isSafeInteger(rawTick) && rawTick >= -(2 ** 31) &&
      rawTick < wrap && Number.isFinite(position) && Number.isInteger(rawCurrent) &&
      rawCurrent >= -32768 && rawCurrent <= 32767, `CSV 第 ${lineNumber} 行无效`)
    const tick = rawTick >>> 0
    if (previous === undefined) {
      t.push(0)
    } else {
      const delta = (tick - previous + wrap) % wrap
      const known = delta > 0 && delta < wrap / 2
      // Keep an unknown discontinuity visible on the plot with one nominal tick.
      // Any window across it is explicitly excluded from usable timing statistics.
      const next = t.at(-1) + (known ? delta : 1) * config.tick
      check(Number.isSafeInteger(next), '卡内时间跨度超出可表示范围')
      t.push(next)
      if (delta !== 1) {
        breaks.push({ index, time: next, kind: known ? 'gap' : delta === 0 ? 'duplicate' : 'backwards',
          missing: known ? delta - 1 : null })
      }
    }
    block.push(breaks.length)
    x.push(position / config.scale)
    current.push(rawCurrent)
    previous = tick
  }
  check(t.length > 1, '至少需要两帧')
  return { t, x, current, block, breaks }
}

function parameters(config) {
  const motionSpeed = Math.max(0.08, 0.03 * (config.expected?.speed ?? 10))
  return {
    fitMs: 5, accelerationSpanMs: 5, motionSpeed, stationarySpeed: Math.min(0.15, motionSpeed / 2),
    acceleration: Math.max(8, 0.1 * (config.expected?.acceleration ?? 400)),
    minPhaseMs: 2, minLegMs: 8, minStationaryMs: 10, positionTolerance: 0.05
  }
}

function estimate(frames, tick, p) {
  const { x, block } = frames
  const v = new Float64Array(x.length).fill(NaN)
  const a = new Float64Array(x.length).fill(NaN)
  const fitting = Math.max(1, Math.round(p.fitMs * nsPerMs / (2 * tick)))
  const offset = Math.max(1, Math.round(p.accelerationSpanMs * nsPerMs / (2 * tick)))
  const xSum = new Float64Array(x.length + 1)
  const ixSum = new Float64Array(x.length + 1)
  for (let i = 0; i < x.length; ++i) {
    xSum[i + 1] = xSum[i] + x[i]
    ixSum[i + 1] = ixSum[i] + i * x[i]
  }
  const denominator = fitting * (fitting + 1) * (2 * fitting + 1) / 3
  for (let i = fitting; i + fitting < x.length; ++i) {
    const left = i - fitting
    const right = i + fitting
    if (block[left] !== block[right]) continue
    const centered = (ixSum[right + 1] - ixSum[left]) - i * (xSum[right + 1] - xSum[left])
    v[i] = centered / denominator * 1e9 / tick
  }
  for (let i = offset; i + offset < x.length; ++i) {
    if (block[i - offset] !== block[i + offset]) continue
    a[i] = (v[i + offset] - v[i - offset]) * 1e9 / (2 * offset * tick)
  }
  return { v, a, fitMs: 2 * fitting * tick / nsPerMs, accelerationSpanMs: 2 * offset * tick / nsPerMs }
}

function classify(frames, fit, p, tick) {
  const { t, block } = frames
  const phase = Array(t.length)
  const direction = new Int8Array(t.length)
  const legs = []
  for (let i = 0; i < t.length; ++i) {
    const { v, a } = fit
    direction[i] = Math.abs(v[i]) >= p.motionSpeed ? Math.sign(v[i]) : 0
    const speedChange = a[i] * direction[i]
    phase[i] = 'transition'
    if (!Number.isFinite(a[i])) continue
    if (Math.abs(v[i]) <= p.stationarySpeed && Math.abs(a[i]) <= p.acceleration / 2) {
      phase[i] = 'stationary'
    } else if (direction[i]) {
      if (speedChange >= p.acceleration) phase[i] = 'acceleration'
      else if (speedChange <= -p.acceleration) phase[i] = 'deceleration'
      else if (Math.abs(speedChange) <= p.acceleration / 2) phase[i] = 'cruise'
    }
  }
  for (let i = 0; i < t.length;) {
    let end = i + 1
    while (end < t.length && direction[end] === direction[i] && block[end] === block[i]) ++end
    if (direction[i] && (end - i) * tick >= p.minLegMs * nsPerMs) {
      const last = legs.at(-1)
      if (last?.direction === direction[i]) last.end = end - 1
      else legs.push({ direction: direction[i], start: i, end: end - 1 })
    }
    i = end
  }
  for (let k = 1; k < legs.length; ++k) {
    const from = legs[k - 1].end
    const to = legs[k].start
    if (block[from] !== block[to]) continue
    for (let i = from + 1; i < to; ++i) {
      if (phase[i] === 'stationary' || phase[i] === 'transition') {
        phase[i] = 'reverse'
        direction[i] = 0
      }
    }
  }
  for (let i = 0; i < t.length;) {
    let end = i + 1
    while (end < t.length && phase[end] === phase[i] && direction[end] === direction[i] &&
      block[end] === block[i]) ++end
    if (phase[i] !== 'reverse' && (end - i) * tick < p.minPhaseMs * nsPerMs) {
      phase.fill('transition', i, end)
    }
    i = end
  }
  const segments = []
  const segmentAt = new Int32Array(t.length)
  for (let i = 0; i < t.length; ++i) {
    const last = segments.at(-1)
    if (!last || last.phase !== phase[i] || last.direction !== direction[i] || last.block !== block[i]) {
      segments.push({ phase: phase[i], direction: direction[i], block: block[i], first: i, last: i })
    } else last.last = i
    segmentAt[i] = segments.length - 1
  }
  return { legs, segments, segmentAt }
}

function motionCoverage(frames, phases, config, p) {
  const { x, t, block } = frames
  const { legs, segments } = phases
  if (!legs.length) return { status: '未检测到运动', start: x[0], end: x.at(-1) }
  const first = legs[0]
  const second = legs[1]
  const last = legs.at(-1)
  const stationary = segment => segment.phase === 'stationary' &&
    (segment.last - segment.first + 1) * config.tick >= p.minStationaryMs * nsPerMs
  const before = segments.findLast(s => s.last < first.start && s.block === block[first.start] && stationary(s))
  const after = segments.find(s => s.first > last.end && s.block === block[last.end] && stationary(s))
  const start = x[before?.last ?? 0]
  const end = x[after?.first ?? x.length - 1]
  let turnIndex = first.end
  if (second) {
    for (let i = first.end + 1; i <= second.start; ++i) {
      if (first.direction * (x[i] - x[turnIndex]) > 0) turnIndex = i
    }
  }
  const turn = second ? x[turnIndex] : null
  const expected = config.expected
  const tolerance = p.positionTolerance
  const returned = Math.abs(end - start) <= tolerance
  const routeMatches = expected ? turn !== null &&
    first.direction === Math.sign(expected.turn - expected.start) &&
    Math.abs(start - expected.start) <= tolerance && Math.abs(turn - expected.turn) <= tolerance &&
    Math.abs(end - expected.start) <= tolerance : null
  const continuous = block[before?.last ?? first.start] === block[after?.first ?? last.end]
  const capturedBoth = legs.length === 2 && Boolean(before && after) && returned
  let status = '往返覆盖不完整'
  if (capturedBoth && !continuous) status = '记录到往返范围，运动覆盖有时钟断点'
  else if (capturedBoth && routeMatches === false) status = '记录到往返，端点与预期工况有偏差'
  else if (capturedBoth) status = '完整往返'
  return { status, start, turn, end, turnTime: turn === null ? null : t[turnIndex] / nsPerMs,
    outward: turn === null ? null : turn - start, return: turn === null ? null : end - turn,
    directions: legs.map(leg => leg.direction), before: Boolean(before), after: Boolean(after),
    returned, continuous, routeMatches }
}

function writeWindows(path, frames, phases, config) {
  const { t, current, breaks, block } = frames
  const { segments, segmentAt } = phases
  const groups = new Map()
  const exclusions = {}
  const fd = openSync(path, 'w')
  try {
    writeSync(fd, 'width_ms,start_ms,end_ms,first_frame,last_frame,first_segment,phase,direction,status,frames,observable_changes,longest_equal_ms,current_range_raw\n')
    let rows = []
    for (const width of widths) {
      let right = 0
      for (let left = 0; left < t.length && t[left] + width * nsPerMs <= t.at(-1); ++left) {
        const end = t[left] + width * nsPerMs
        while (right < t.length && t[right] < end) ++right
        const last = right - 1
        const segment = segments[segmentAt[left]]
        // The very end of a window can lie inside missing time even if its last
        // observed frame still belongs to the left-hand block.
        const nextBreak = breaks[block[left]]
        const laterBlock = block[left] !== block[last]
        const clockBreak = nextBreak?.kind === 'gap'
          ? end > t[nextBreak.index - 1] + config.tick : laterBlock
        const crossPhase = segmentAt[left] !== segmentAt[last]
        const status = clockBreak ? 'cross_clock_break' : crossPhase ? 'cross_motion_segment' : 'within_segment'
        let changes = 0
        let run = 1, longest = 1
        let min = current[left], max = min
        for (let i = left + 1; i < right; ++i) {
          const adjacent = block[i - 1] === block[i]
          const changed = current[i] !== current[i - 1]
          if (adjacent && changed) ++changes
          run = adjacent && !changed ? run + 1 : 1
          longest = Math.max(longest, run)
          min = Math.min(min, current[i])
          max = Math.max(max, current[i])
        }
        if (status === 'within_segment') {
          const key = `${width},${segment.direction},${segment.phase}`
          if (!groups.has(key)) groups.set(key, { changes: [], framesMin: Infinity, framesMax: 0,
            rangeMin: Infinity, rangeMax: 0, zero: 0 })
          const group = groups.get(key)
          group.changes.push(changes)
          group.framesMin = Math.min(group.framesMin, right - left)
          group.framesMax = Math.max(group.framesMax, right - left)
          group.rangeMin = Math.min(group.rangeMin, max - min)
          group.rangeMax = Math.max(group.rangeMax, max - min)
          group.zero += Number(changes === 0)
        } else exclusions[`${width},${status}`] = (exclusions[`${width},${status}`] ?? 0) + 1
        rows.push([width, t[left] / nsPerMs, end / nsPerMs, left, last, segmentAt[left] + 1,
          segment.phase, segment.direction, status, right - left, changes,
          longest * config.tick / nsPerMs, max - min].join(','))
        if (rows.length === 4096) {
          writeSync(fd, rows.join('\n') + '\n')
          rows = []
        }
      }
    }
    if (rows.length) writeSync(fd, rows.join('\n') + '\n')
  } finally {
    closeSync(fd)
  }
  return { groups, exclusions }
}

function segmentsCsv(frames, phases, tick) {
  const { t, x } = frames
  const rows = ['segment,clock_block,phase,direction,start_ms,end_ms,duration_ms,frames,start_mm,end_mm']
  for (const [index, s] of phases.segments.entries()) {
    const end = Math.min(t[s.last] + tick, t.at(-1))
    rows.push([index + 1, s.block + 1, s.phase, s.direction, t[s.first] / nsPerMs,
      end / nsPerMs, (end - t[s.first]) / nsPerMs, s.last - s.first + 1,
      x[s.first], x[s.last]].join(','))
  }
  return rows.join('\n') + '\n'
}

function summary(id, config, p, frames, fit, phases, coverage, windows) {
  const f = n => n == null ? '—' : Number(n.toFixed(5))
  const yesNo = value => value == null ? '—' : value ? '是' : '否'
  const rows = [
    `# Watch 运动回放 — ${id}`, '', `**${coverage.status}**`, '',
    `- ${frames.t.length} 帧，核周期 ${config.tick / 1000} µs，时间轴 ${f(frames.t.at(-1) / nsPerMs)} ms，断点 ${frames.breaks.length} 处。`,
    `- 实际起点 / 折返点 / 终点（mm） ${f(coverage.start)} / ${f(coverage.turn)} / ${f(coverage.end)}。`,
    `- 去程 / 回程有符号位移（mm） ${f(coverage.outward)} / ${f(coverage.return)}，折返位置时间 ${f(coverage.turnTime)} ms。`,
    `- 持续运动方向 ${coverage.directions?.join(' → ') ?? '—'}。录到起步前静止 ${yesNo(coverage.before)}，录到末端静止 ${yesNo(coverage.after)}，返回起点 ${yesNo(coverage.returned)}，运动覆盖连续 ${yesNo(coverage.continuous)}，与预期端点匹配 ${yesNo(coverage.routeMatches)}。`,
    `- 预期工况 ${config.expected ? `${config.expected.start} → ${config.expected.turn} → ${config.expected.start} mm，${config.expected.speed} mm/s，${config.expected.acceleration} mm/s²` : '该会话未记录'}。实际运动按编码器判定，换算比例 ${config.scale} pulse/mm。`,
    `- 位置线性拟合窗 ${fit.fitMs} ms，加速度差分跨度 ${fit.accelerationSpanMs} ms。运动 / 静止速度阈值 ${p.motionSpeed} / ${p.stationarySpeed} mm/s，加速阈值 ${p.acceleration} mm/s²，匀速和静止加速度上限 ${p.acceleration / 2} mm/s²。`,
    `- 短于 ${p.minPhaseMs} ms 的分段标为过渡。方向最短持续 ${p.minLegMs} ms，端点静止最短 ${p.minStationaryMs} ms，位置容差 ±${p.positionTolerance} mm。`,
    '', '## 运动分段', '',
    '| 方向 | 运动段 | 起始 ms | 时长 ms |', '| --- | --- | ---: | ---: |'
  ]
  for (const s of phases.segments) {
    if (s.phase === 'transition') continue
    const end = Math.min(frames.t[s.last] + config.tick, frames.t.at(-1))
    rows.push(`| ${s.direction > 0 ? '+' : s.direction < 0 ? '−' : '—'} | ${phaseNames[s.phase]} | ${f(frames.t[s.first] / nsPerMs)} | ${f((end - frames.t[s.first]) / nsPerMs)} |`)
  }
  rows.push('', '过渡段和各段帧号范围见 `segments.csv` 中的分段行。',
    '', '## 窗口内可观测的电流变化', '',
    '每个已记录帧作为一个完整半开窗口的起点。变化次数是窗口内连续卡时钟上的相邻帧电流值不同的次数。100 帧最多比较 99 对。最长同值时长按连续同值帧数乘核周期计算。驱动器刷新后可能仍输出同一量化值，反馈刷新周期和相对位置时延仍待实测。',
    '', '| 窗口 ms | 方向 | 运动段 | 窗数 | 帧数范围 | 变化次数中位数 | 变化次数范围 | 零变化 | 极差范围 raw |',
    '| ---: | --- | --- | ---: | --- | ---: | --- | ---: | --- |')
  const ordered = [...windows.groups.entries()].sort(([a], [b]) =>
    Number(a.split(',', 1)[0]) - Number(b.split(',', 1)[0]) || a.localeCompare(b))
  for (const [key, g] of ordered) {
    const [width, direction, phase] = key.split(',')
    const a = g.changes.sort((a, b) => a - b)
    const median = (a[Math.floor(a.length / 2)] + a[Math.floor((a.length - 1) / 2)]) / 2
    rows.push(`| ${width} | ${direction === '1' ? '+' : direction === '-1' ? '−' : '—'} | ${phaseNames[phase]} | ${a.length} | ${g.framesMin}–${g.framesMax} | ${median} | ${a[0]}–${a.at(-1)} | ${(100 * g.zero / a.length).toFixed(2)}% | ${g.rangeMin}–${g.rangeMax} |`)
  }
  rows.push('', ...widths.map(w => `- ${w} ms 排除窗口。跨时钟断点 ${windows.exclusions[`${w},cross_clock_break`] ?? 0}，跨运动段 ${windows.exclusions[`${w},cross_motion_segment`] ?? 0}。`))
  if (frames.breaks.length) {
    rows.push('', '## 时钟断点', '',
      '倒序和重复的真实间隔未知，图中用一个标称 tick 连接横轴。断点两侧独立拟合，跨断点窗口只保留作诊断。')
    for (const b of frames.breaks) {
      rows.push(`- 帧 ${b.index}，${f(b.time / nsPerMs)} ms，${b.kind}，缺失 tick ${b.missing ?? '未知'}。`)
    }
  }
  return rows.join('\n') + '\n'
}

function plot(frames, phases, id) {
  const width = 1160, left = 105, right = 1120, panelHeight = 168
  const { t, x, current, block, breaks } = frames
  const total = t.at(-1)
  const px = time => left + time / total * (right - left)
  const ramps = [1, -1].flatMap(direction => ['acceleration', 'deceleration'].map(phase =>
    phases.segments.filter(s => s.phase === phase && s.direction === direction)
      .sort((a, b) => (b.last - b.first) - (a.last - a.first))[0]))
  const showDetails = ramps.some(Boolean)
  const height = showDetails ? 1040 : 530
  const rows = [`<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}" viewBox="0 0 ${width} ${height}">
<title>Watch 位置和电流 ${id}</title><rect width="100%" height="100%" fill="white"/>
<g font-family="Microsoft YaHei, sans-serif" fill="#162b3d" font-size="13">
<text x="105" y="35" font-size="20" font-weight="bold">Watch 位置与电流 · ${id}</text>`]
  for (const [data, top, label, color] of [[x, 65, '位置 (mm)', '#155682'],
    [current, 270, '电流 (raw)', '#934719']]) {
    let min = Infinity, max = -Infinity
    for (const value of data) {
      min = Math.min(min, value)
      max = Math.max(max, value)
    }
    const pad = max === min ? (data === x ? 0.1 : 1) : (max - min) * 0.06
    min -= pad
    max += pad
    const py = value => top + panelHeight * (max - value) / (max - min)
    for (const s of phases.segments) {
      const l = px(t[s.first])
      const r = px(s.last + 1 < t.length && block[s.last + 1] === s.block ? t[s.last + 1] : t[s.last])
      rows.push(`<rect x="${l.toFixed(2)}" y="${top}" width="${(r - l).toFixed(2)}" height="${panelHeight}" fill="${colors[s.phase]}"/>`)
      if (data === x && s.direction && r - l > 35) {
        rows.push(`<text x="${((l + r) / 2).toFixed(2)}" y="${top + 16}" text-anchor="middle">${s.direction > 0 ? '+' : '−'}</text>`)
      }
    }
    for (let j = 0; j <= 4; ++j) {
      const value = min + (max - min) * j / 4
      const y = py(value)
      rows.push(`<path d="M${left} ${y}H${right}" stroke="#adb9c3" opacity=".65"/><text x="${left - 10}" y="${y + 4}" text-anchor="end">${Number(value.toPrecision(4))}</text>`)
    }
    for (let j = 0; j <= 5; ++j) {
      const x = px(total * j / 5)
      rows.push(`<path d="M${x} ${top}v${panelHeight}" stroke="#adb9c3" opacity=".65"/>`)
      if (data === current) rows.push(`<text x="${x}" y="${top + panelHeight + 20}" text-anchor="middle">${Number((total / 1e9 * j / 5).toPrecision(4))}</text>`)
    }
    const d = []
    // Keep endpoint and extrema per pixel within each connected clock block.
    for (let i = 0; i < data.length;) {
      const pixel = Math.floor(px(t[i]))
      let end = i + 1, low = i, high = i
      while (end < data.length && block[end] === block[i] && Math.floor(px(t[end])) === pixel) {
        if (data[end] < data[low]) low = end
        if (data[end] > data[high]) high = end
        ++end
      }
      const indices = [...new Set([i, low, high, end - 1])].sort((a, b) => a - b)
      for (const index of indices) {
        const command = index === i && (i === 0 || block[i] !== block[i - 1]) ? 'M' : 'L'
        d.push(`${command}${px(t[index]).toFixed(2)},${py(data[index]).toFixed(2)}`)
      }
      i = end
    }
    rows.push(`<path d="${d.join('')}" fill="none" stroke="${color}" stroke-width="1.35"/>`)
    for (const b of breaks) {
      rows.push(`<path d="M${px(b.time)} ${top}v${panelHeight}" stroke="#ad2632" stroke-dasharray="4 3"/>`)
    }
    rows.push(`<text x="22" y="${top + panelHeight / 2}" transform="rotate(-90 22 ${top + panelHeight / 2})" text-anchor="middle">${label}</text>`)
  }
  rows.push('<text x="612" y="478" text-anchor="middle">卡内相对时间 (s)</text>')
  for (const [i, [phase, label]] of Object.entries(phaseNames).entries()) {
    const x = left + i * 116
    rows.push(`<rect x="${x}" y="497" width="16" height="12" fill="${colors[phase]}"/><text x="${x + 22}" y="508">${label}</text>`)
  }
  rows.push('<path d="M801 503h18" stroke="#ad2632" stroke-dasharray="4 3"/><text x="826" y="508">时钟断点</text>')
  if (showDetails) {
    rows.push('<text x="105" y="552" font-size="16" font-weight="bold">各方向最长加减速段 · 卡内相对时间 (ms)</text>')
    for (const [i, s] of ramps.entries()) {
      if (!s) continue
      const l = left + i % 2 * 530
      const r = l + 480
      const top = 584 + Math.floor(i / 2) * 225
      const first = t[s.first]
      const last = t[s.last]
      if (first === last) continue
      const toX = index => l + (t[index] - first) / (last - first) * (r - l)
      rows.push(`<text x="${l}" y="${top}" font-weight="bold">${s.direction > 0 ? '正向' : '反向'}${phaseNames[s.phase]}</text>`)
      for (const [data, offset, label, color] of [[x, 12, 'mm', '#155682'],
        [current, 96, 'raw', '#934719']]) {
        let low = data[s.first], high = low
        for (let j = s.first + 1; j <= s.last; ++j) {
          low = Math.min(low, data[j])
          high = Math.max(high, data[j])
        }
        const y = top + offset
        const panel = 67
        const span = high - low
        const toY = index => y + (span ? 5 + (high - data[index]) / span * (panel - 10) : panel / 2)
        const d = []
        for (let j = s.first; j <= s.last; ++j) {
          d.push(`${j === s.first ? 'M' : 'L'}${toX(j).toFixed(2)},${toY(j).toFixed(2)}`)
        }
        rows.push(`<rect x="${l}" y="${y}" width="${r - l}" height="${panel}" fill="${colors[s.phase]}"/>
<path d="${d.join('')}" fill="none" stroke="${color}" stroke-width="1.3"/>
<text x="${l - 7}" y="${y + 13}" text-anchor="end">${Number(high.toPrecision(5))}</text>
<text x="${l - 7}" y="${y + panel - 2}" text-anchor="end">${Number(low.toPrecision(5))}</text>
<text x="${l + 7}" y="${y + 17}">${label}</text>`)
      }
      rows.push(`<text x="${l}" y="${top + 181}">${first / nsPerMs}</text>
<text x="${r}" y="${top + 181}" text-anchor="end">${last / nsPerMs}</text>`)
    }
  }
  rows.push('</g></svg>')
  return rows.join('\n')
}

async function main() {
  const [id, root = join(here, '..')] = process.argv.slice(2)
  check(process.argv.length <= 4 && id && /^[A-Za-z0-9][A-Za-z0-9_-]*$/u.test(id),
    '用法  node scripts/analyze_watch_motion.mjs <session_id> [会话根目录]')
  const config = readConfig(join(root, 'manifest', id + '.txt'))
  const frames = await readFrames(join(root, 'data', 'raw', id + '.csv'), config)
  const p = parameters(config)
  const fit = estimate(frames, config.tick, p)
  const phases = classify(frames, fit, p, config.tick)
  const coverage = motionCoverage(frames, phases, config, p)
  const dest = join(root, 'results', 'watch_motion', id)
  mkdirSync(dest, { recursive: true })
  const windows = writeWindows(join(dest, 'windows.csv'), frames, phases, config)
  writeFileSync(join(dest, 'segments.csv'), segmentsCsv(frames, phases, config.tick))
  writeFileSync(join(dest, 'summary.md'), summary(id, config, p, frames, fit, phases, coverage, windows))
  writeFileSync(join(dest, 'diagnostic.svg'), plot(frames, phases, id))
  console.log(coverage.status)
  console.log(dest)
}

main().catch(error => {
  console.error(error.message)
  process.exitCode = 1
})

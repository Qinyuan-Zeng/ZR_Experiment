import { spawnSync } from 'node:child_process'
import { appendFileSync, mkdtempSync } from 'node:fs'
import { join } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

// One invocation attempts one batch. A new invocation reads the accepted slots again.
export function batch(id, count, run) {
  const pending = () => {
    const rows = run('pending', ['--pending', id]).trim().split(/\r?\n/).filter(Boolean)
    return rows.map(row => {
      const [slot, condition, history, extra] = row.split(/\s+/)
      // The native pending command has already validated the frozen protocol and schedule.
      if (!slot.startsWith(id + '_') || !(/^(?:Z-DP_)/.test(id) ? condition === 'D' : /^(?:B[1-4]|X\d{3}V(?:16|20))$/.test(condition)) ||
          !/^H[12]$/.test(history) || extra) throw Error('Unexpected pending output: ' + row)
      return slot
    })
  }
  const before = pending()
  if (!before.length) return 'Protocol complete'
  const selected = before.slice(0, count)
  const status = phase => {
    if (!/^ready=1\r?$/m.test(run(phase, ['--status']))) throw Error('Status unavailable: ' + phase)
  }
  status('before')
  const errors = []
  for (const action of [
    () => run('acquire', [], ['F', id, selected.length, '5000,500', 'START', ''].join('\n')),
    () => status('after'),
    () => run('replay', [id])
  ]) {
    try { action() } catch (error) { errors.push(error.message) }
  }
  if (errors.length) throw Error(errors.join('\n'))
  const remaining = pending()
  if (JSON.stringify(remaining) !== JSON.stringify(before.slice(selected.length))) {
    throw Error('Acceptance differs from planned batch. Inspect replay before resuming.')
  }
  return 'Accepted ' + selected.length + ', pending ' + remaining.length
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const [id, confirmation, size = '4', ...extra] = process.argv.slice(2)
  if (!/^Z-[BD]P_\d{8}-\d{6}_\d+_\d+$/.test(id ?? '') || confirmation !== '--field-confirmed' ||
      !/^[1-4]$/.test(size) || extra.length) {
    throw Error('Usage: node scripts/run_b_batch.mjs <authorized protocol ID> --field-confirmed [1..4]')
  }
  const root = fileURLToPath(new URL('../', import.meta.url))
  const isD = id.startsWith('Z-DP_')
  const entry = isD ? 'd_validate_inertia' : 'b_matched_inertia'
  const analyzer = isD ? 'scripts/analyze_d.mjs' : 'scripts/analyze_b.mjs'
  const exe = join(root, 'bin/x64/Release', entry, 'ZRInertiaCurrent.exe')
  const log = join(mkdtempSync(join(root, isD ? 'manifest/d' : 'manifest/b', id + '_batch_')), 'workflow.log')
  console.log(log)
  function record(message) { appendFileSync(log, new Date().toISOString() + ' ' + message + '\n') }
  function run(phase, args, input) {
    record('begin ' + phase)
    const replay = phase === 'replay'
    const result = spawnSync(replay ? process.execPath : exe,
      replay ? [join(root, analyzer), ...args] : args,
      { cwd: root, input, encoding: 'utf8', maxBuffer: 32 * 1024 * 1024 })
    record([phase, 'exit=' + result.status, result.stdout, result.stderr].join('\n'))
    if (result.error || result.status !== 0) throw Error(phase + ': ' + (result.error?.message ?? result.stderr))
    return result.stdout
  }
  try {
    record('field_confirmed=1 protocol=' + id + ' max_pairs=' + size + ' reserve_pulse=5000 follow_limit_pulse=500')
    const outcome = batch(id, Number(size), run)
    record(outcome)
    console.log(outcome)
  } catch (error) {
    record(error.message)
    console.error(error.message)
    process.exitCode = 1
  }
}

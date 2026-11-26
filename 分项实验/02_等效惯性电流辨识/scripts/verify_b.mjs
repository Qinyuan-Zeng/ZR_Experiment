import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { copyFileSync, mkdirSync, readFileSync, readdirSync, unlinkSync, writeFileSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { regression, selectAttempts, validateAccelerationTiers, continuousRangeValidation } from './analyze_b.mjs'
import { loadModel } from './analyze_a1.mjs'

const project = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const root = join(project, 'verification/b')
const analyzer = join(project, 'scripts/analyze_b.mjs')
const model = loadModel()
const protocol = 'Z-BP_synthetic'
const cells = [[400000,20000],[400000,16000],[600000,20000],[600000,16000]]
const names = ['B1','B2','B3','B4']
const gateIds = names.map(c => 'synthetic_gate_' + c)
const frozen = {
  rules: 'b_matched_inertia_v1', model: 'position_B_primary_v1', model_sha256: model.hash,
  x0_plan_pulse: 16000, peak_pulse_s: 40000, scale: 1000, fit_windows_ms: '15,21,31',
  current_windows_ms: '5,9,15', current_shift_ms: 0, position_tol_mm: 0.01, speed_target_tol_mm_s: 0.5,
  speed_pair_tol_mm_s: 0.5, fit_stability_fraction: 0.1, repeat_min_pairs: 5, snr_min: 5, ci_rel_half_max: 0.30,
  history_x_tol_pulse: 10, history_speed_tol_pulse_s: 500, history_acc_fraction: 0.10, preparation_hold_ms: 250
}
const slotHeader = 'planned_pair_id,condition,round,series,history_condition,first_role'
const alpha = 2.8e-5, intercept = 4

function save(name, text) {
  const path = join(root, name)
  mkdirSync(dirname(path), { recursive: true })
  writeFileSync(path, text)
}
function meta(name, data) {
  save(name + '.txt', Object.entries(data).map(([k,v]) => `${k}=${v}`).join('\n') + '\n')
}
function run(exe, args, input) {
  const r = spawnSync(exe, args, { cwd: project, encoding: 'utf8', input, maxBuffer: 4e6 })
  assert.equal(r.status, 0, r.stderr + r.stdout)
  return r.stdout
}
function report(id) {
  return JSON.parse(readFileSync(join(root, 'results/b', id + '_summary.json'), 'utf8'))
}
function analyze(id, ...args) {
  return run(process.execPath, [analyzer, id, '--root', root, ...args])
}

const exact = [0.78e6,0.82e6,1.16e6,1.22e6].map(x => ({x,y:alpha*x+intercept}))
const zero = regression(exact, false)
const free = regression(exact, true)
const constrained = alpha + intercept * exact.reduce((s,p)=>s+p.x,0)/exact.reduce((s,p)=>s+p.x*p.x,0)
assert(Math.abs(zero.alpha-constrained) < 1e-14)
assert(Math.abs(free.alpha-alpha) < 1e-14)
assert(Math.abs(free.intercept-intercept) < 1e-10)
const reverse = regression(exact.map(p => ({x:-p.x,y:-p.y})), true)
assert(Math.abs(reverse.alpha-alpha)<1e-14 && Math.abs(reverse.intercept+intercept)<1e-10)
const conflictA = {valid:true,slot:{id:'s1'},accel:{key:'a'},decel:{key:'d'},issues:[]}
const conflictB = {valid:true,slot:{id:'s2'},accel:{key:'a'},decel:{key:'d2'},issues:[]}
assert.deepEqual([...selectAttempts([conflictA,conflictB])],['s1'])
assert.equal(conflictB.valid,false)

const slots = []
for (let i=1;i<=5;i++) for (let j=0;j<4;j++) {
  const c=names[(i-1+j)%4]
  slots.push([`${protocol}_M${i}_${c}`,c,i,'matrix','H1',i%2?'accel':'decel'])
}
for(let i=1;i<=2;i++) for(let j=0;j<2;j++){
  const h=(i+j)%2?'H1':'H2'
  slots.push([`${protocol}_H${i}_${h}`,'B1',i,'history',h,i%2?'accel':'decel'])
}
const m = { protocol_id: protocol, direction: 1, rounds: 5, history_blocks: 2,
  history_approach_pulse: 500, history_excursion_pulse: 1000, history_speed_mm_s: 2,
  d_speed_pulse_s: 16000, d_acc_pulse_s2: 500000, d_plan_pulse: 18000, d_eval_pulse: 18000, d_time:'synthetic',
  d_max_abs_bias_raw:2,d_max_rmse_raw:4,d_min_pairs:5, ...frozen }
for (let i=0;i<4;i++) {
  const c=names[i]
  m[c+'_eval_pulse']=16000
  m[c+'_acc_pulse_s2']=cells[i][0]
  m[c+'_match_pulse_s']=cells[i][1]
  m[c+'_gate']=gateIds[i]
  meta('results/b/'+gateIds[i]+'_gate', { status:'accepted',rules:frozen.rules,model_sha256:model.hash,
    condition:c,level:c==='B3'?'repeat':'independent',direction:1,x0_plan_pulse:16000,
    eval_pulse:16000,peak_pulse_s:40000,acc_pulse_s2:cells[i][0],match_pulse_s:cells[i][1],evidence:'synthetic' })
}
meta('manifest/b/'+protocol,m)
save('manifest/b/'+protocol+'.plan.csv',slotHeader+'\n'+slots.map(s=>s.join(',')).join('\n')+'\n')

const release = join(project,'bin/x64/Release/b_matched_inertia')
mkdirSync(join(root,'bin'),{recursive:true})
copyFileSync(join(release,'ZRInertiaCurrent.exe'),join(root,'bin','ZRInertiaCurrent.exe'))
copyFileSync(join(release,'gts.dll'),join(root,'bin','gts.dll'))
save('ZRInertiaCurrent.vcxproj','<!-- offline test root marker -->\n')
const exe=join(root,'bin','ZRInertiaCurrent.exe')
for(const configuration of ['Debug','Release']){
  const binary=join(project,'bin','x64',configuration,'b_matched_inertia','ZRInertiaCurrent.exe')
  const plan=run(binary,['--plan'])
  assert.match(plan,/B1 matrix H1 eval=15976/u)
  assert.match(plan,/B3 matrix H1 eval=16000 pulse, A=600000 pulse\/s\^2, v0=20000/u)
  assert.match(plan,/B4 matrix H1 eval=16000 pulse, A=600000 pulse\/s\^2, v0=16000/u)
  assert.equal([...plan.matchAll(/YES YES YES/gu)].length,8)
  assert.match(plan,/accel_pulse=15667 -> 18333/u)
  assert.match(plan,/decel_pulse=13667 -> 16333/u)
  const bad=spawnSync(binary,['--plan','--input'],{input:'0,5000,995000\n',encoding:'utf8'})
  assert.equal(bad.status,1)
  assert.match(run(binary,['--plan','--input'],'16000,5000,995000\n'),/\[OK\]/u)
}

const expandedTable = readFileSync(join(project, 'b_expanded_conditions.csv'), 'utf8')
const expandedCells = expandedTable.trimEnd().split(/\r?\n/u).slice(1).map(line => {
  const [condition, acc, speed] = line.split(',')
  return { condition, acc: Number(acc), speed: Number(speed), gate: 'synthetic_expanded_' + condition }
})
const draft = run(join(release, 'ZRInertiaCurrent.exe'), ['--draft-expanded']).trim().split(/\r?\n/u).slice(1)
assert.equal(draft.length, 120)
assert.equal(new Set(draft.map(line => line.split(',')[0])).size, 120)
for (const {condition} of expandedCells) assert.equal(draft.filter(row => row.split(',')[1] === condition).length, 5)
const expandedPairs = expandedCells.flatMap(({acc,speed}) => Array.from({length: 5}, (_, i) => ({
  p: {acc:acc / 1000, speed:speed / 1000}, da:acc * 2 / 1000 + i, di:alpha * (acc * 2 + i * 1000)
})))
const folds = validateAccelerationTiers(expandedPairs)
assert.equal(folds.length, 96)
for (const fold of folds) {
  assert.equal(fold.trainHeldOutPairs, 0)
  assert.equal(fold.testPairs, 5)
  if (fold.method === 'leave_tier_out') {
    assert.equal(fold.trainingPairs, fold.trainingScope === 'pooled' ? 110 : 55)
    assert(fold.rmse < 1e-10)
  }
}
const interiorRanges = continuousRangeValidation(expandedPairs.filter(r => r.p.acc >= 300 && r.p.acc <= 650))
assert.equal(interiorRanges.length,28)
const allInterior = interiorRanges.find(([first,last]) => first === 300000 && last === 650000)
assert.deepEqual(allInterior.slice(2,4),[8,80])
assert(Math.abs(allInterior[4]-alpha)<1e-14 && allInterior[5]<1e-10)
save('b_expanded_conditions.csv', expandedTable)
for (const {condition, acc, speed, gate} of expandedCells) {
  meta('results/b/' + gate + '_gate', {status:'accepted',rules:frozen.rules,model_sha256:model.hash,
    condition,level:'independent',direction:1,x0_plan_pulse:16000,eval_pulse:16000,peak_pulse_s:40000,
    acc_pulse_s2:acc,match_pulse_s:speed,evidence:'synthetic'})
}
let expandedId, rangeId
try {
  const input = expandedCells.map(c => c.gate).concat(['1','18000,500000,18000,18000','synthetic','4,6,5','SAVE',''])
  expandedId = run(exe, ['--protocol-expanded'], input.join('\n')).match(/Frozen protocol: (Z-BP_\S+)/u)?.[1]
  assert.ok(expandedId)
  assert.equal(run(exe, ['--pending',expandedId]).trim().split(/\r?\n/u).length, 120)
  analyze(expandedId)
  const initial = report(expandedId)
  assert.equal(initial.matrixDesign, 'expanded_12x2')
  assert.equal(initial.accepted, 0)
  assert.equal(initial.pending.length, 120)
  assert.equal(Object.keys(initial.byCondition).length, 24)

  const rangeCells = expandedCells.filter(c => c.acc >= 300000 && c.acc <= 650000)
  const rangeDraft = run(exe,['--draft-range']).trim().split(/\r?\n/u).slice(1)
  assert.equal(rangeDraft.length,80)
  assert.deepEqual(rangeDraft.slice(0,4).map(row => row.split(',')[1]),['X300V16','X650V20','X300V20','X650V16'])
  assert.equal(new Set(rangeDraft.map(row => row.split(',')[0])).size,80)
  for (const c of rangeCells) assert.equal(rangeDraft.filter(row => row.split(',')[1] === c.condition).length,5)
  const inputs = rangeCells.map(c => ({X400V20:gateIds[0], X600V20:gateIds[2]})[c.condition] ?? c.gate)
  const wrong = inputs.slice()
  wrong[rangeCells.findIndex(c => c.condition === 'X400V16')] = gateIds[0]
  const rejected = spawnSync(exe,['--protocol-range'],{encoding:'utf8',input:['',...wrong,'1',''].join('\n')})
  assert.equal(rejected.status,1)
  assert.match(rejected.stderr,/original B qualification does not match this cell/u)
  rangeId = run(exe,['--protocol-range'],['',...inputs,'1','18000,500000,18000,18000',
    'synthetic','4,6,5','SAVE',''].join('\n')).match(/Frozen protocol: (Z-BP_\S+)/u)?.[1]
  assert.ok(rangeId)
  const rangeProtocol = readFileSync(join(root,'manifest/b',rangeId+'.txt'),'utf8')
  assert.match(rangeProtocol,/X400V20_gate_condition=B1/u)
  assert.match(rangeProtocol,/X600V20_gate_condition=B3/u)
  assert.match(rangeProtocol,/max_batch_pairs=4/u)
  assert.equal(run(exe,['--pending',rangeId]).trim().split(/\r?\n/u).length,80)
  // Native input validation rejects 5 before any status read, card open or START.
  const tooMany = spawnSync(exe,[],{encoding:'utf8',input:['F',rangeId,'5',''].join('\n')})
  assert.equal(tooMany.status,1)
  assert.match(tooMany.stderr,/number outside allowed integer range/u)
  assert.doesNotMatch(tooMany.stdout,/Field-confirmed|Session:/u)
  analyze(rangeId)
  const range = report(rangeId)
  assert.equal(range.matrixDesign,'expanded_range_v1')
  assert.equal(range.pending.length,80)
  assert.equal(Object.keys(range.byCondition).length,16)
} finally {
  unlinkSync(join(root, 'b_expanded_conditions.csv'))
  for (const {gate} of expandedCells) unlinkSync(join(root, 'results/b', gate + '_gate.txt'))
  for (const id of [expandedId, rangeId].filter(Boolean)) for (const dir of ['manifest/b','results/b']) {
    for (const name of readdirSync(join(root,dir)).filter(name => name.startsWith(id + '.') || name.startsWith(id + '_'))) {
      unlinkSync(join(root,dir,name))
    }
  }
}

function makeRun(id, pair, order, slot, role, direction) {
  const [planned_pair_id,condition,round,series,history_condition,first_role]=slot
  const [acc,match]=cells[names.indexOf(condition)]
  const accel=role==='accel'
  const dist=40000**2/acc, offset=match**2/(2*acc)
  const pre=accel?offset:dist-offset
  const start=Math.round(16000-direction*pre), finish=Math.round(16000+direction*(dist-pre))
  const length=Math.abs(finish-start), peak=Math.min(40000,Math.sqrt(length*acc))
  const ramp=peak/acc, cruise=(length-peak*ramp)/peak, duration=2*ramp+cruise
  const step=0.00005, startIndex=2500, settle=startIndex+Math.ceil(duration/step)+200
  const baseTick=2**32-2000, clock=n=>(baseTick+n)>>>0
  const key=`${id}_p${pair}_r${order}`
  const samples=['frame,card_tick_raw,encoder_position_raw,current_feedback_raw']
  for(let n=0;n<=settle+3300;n++){
    const t=(n-startIndex)*step
    const d=t<0?0:t<ramp?acc*t*t/2:t<ramp+cruise?peak*(t-ramp/2):
      t<duration?length-acc*(duration-t)**2/2:length
    const x=start+direction*Math.round(d)
    const phase=t>=0&&t<ramp?1:t>=ramp+cruise&&t<duration?-1:0
    const history=history_condition==='H2'?2:0
    const y=Math.round(model.at(x/1000,1000)+100+direction*phase*(alpha*acc+(intercept+history)/2))
    samples.push(`${n},${clock(n)},${x},${y}`)
  }
  let route=[start]
  if(series==='history'){
    const approach=start-direction*500
    route=history_condition==='H2'?[approach,approach-direction*1000,approach,start]:[approach,start]
  }
  save('data/raw/'+key+'.csv',samples.join('\n')+'\n')
  meta('manifest/'+key,{session_id:id,pair_id:pair,run_id:key,execution_order:order,role,
    direction,core_period_ns:50000,x0_plan_mm:16,x_match_eval_mm:16,start_mm:start/1000,
    end_mm:finish/1000,planned_platform_begin_ms:(accel?0:ramp+cruise)*1000,
    planned_platform_end_ms:(accel?ramp:duration)*1000,
    planned_match_ms:(accel?Math.sqrt(2*Math.abs(16000-start)/acc):
      duration-Math.sqrt(2*Math.abs(finish-16000)/acc))*1000,
    command_before_tick:clock(startIndex-2),command_after_tick:clock(startIndex+2),settled_tick:clock(settle),
    tail_stable_begin_tick:clock(settle+5),tail_stable_end_tick:clock(settle+3005),tail_span_limit_pulse:7.5,
    outcome:'complete',watch_aligned_contiguous:1,planned_pair_id,condition,round,series,history_condition,first_role,
    preparation_hold_ms:250,preparation_speed_mm_s:2,preparation_targets_pulse:route.join(',')})
}

function capture(id,purpose,requested, stopHalf=false,direction=1){
  meta('manifest/'+id,{schema:frozen.rules,session_id:id,purpose,protocol_id:purpose==='formal'?protocol:'',
    qualification_id:'',core:2,axis:1,config:'a33_core2_axis1',direction,slots_requested:requested.length,
    planned_slots:requested.map(s=>s[0]).join(','),operator_field_confirmation:'START',
    outcome:stopHalf?'incomplete':'complete',...frozen})
  for(const [i,slot] of requested.entries()){
    const [planned_pair_id,condition,round,series,history_condition,first_role]=slot
    const half=stopHalf&&i===requested.length-1
    meta('manifest/'+id+'_p'+(i+1),{session_id:id,pair_id:i+1,planned_pair_id,condition,round,series,
      history_condition,first_role,match_pulse_s:cells[names.indexOf(condition)][1],
      eval_pulse:16000,outcome:half?'incomplete':'complete'})
    makeRun(id,i+1,1,slot,first_role,direction)
    if(!half) makeRun(id,i+1,2,slot,first_role==='accel'?'decel':'accel',direction)
  }
}
const stopped='Z-B_20260101-120000_001_1', continued='Z-B_20260101-120001_002_2'
capture(stopped,'formal',slots.slice(0,2),true)
// Remove no files on reruns; omit the last attempt by testing with a fresh prefix
// only if the previous invocation already completed this exact fixture.
const continuingManifest=join(root,'manifest',continued+'.txt')
let previousContinuation
try{ previousContinuation=readFileSync(continuingManifest,'utf8') }catch(error){if(error.code!=='ENOENT')throw error}
if(previousContinuation) writeFileSync(continuingManifest,previousContinuation.replace('operator_field_confirmation=START','operator_field_confirmation=TEST_HIDDEN'))
try{
  analyze(protocol)
  const partial=report(protocol)
  assert.equal(partial.accepted,1)
  assert.equal(partial.pending[0],slots[1][0])
  assert.equal(partial.fitZero,null)
  const pending=run(exe,['--pending',protocol]).trim().split(/\r?\n/u)
  assert.equal(pending.length,23)
  assert(pending[0].startsWith(slots[1][0]+' '))
}finally{
  if(previousContinuation) writeFileSync(continuingManifest,previousContinuation)
}
capture(continued,'formal',slots)
analyze(protocol)
const result=report(protocol)
assert.equal(result.accepted,24)
assert.equal(result.pending.length,0)
assert.equal(result.status,'matrix_complete')
assert.equal(result.fitZero.n,20)
assert.deepEqual(Object.keys(result.speedFits), ['16000','20000'])
assert.equal(result.excitation.separated,true)
assert.equal(result.history.matchedBlocks,2)
assert.equal(result.predictionStats.B3.n,5)
assert.equal(result.predictionStats.B4.n,5)
assert.match(readFileSync(join(root,'results/b',protocol+'_pairs.csv'),'utf8'),/duplicate planned slot/u)
assert.equal(run(exe,['--pending',protocol]).trim(),'')
assert(Math.abs(result.fitFree.alpha-alpha)<2e-6)
assert(Math.abs(result.fitFree.intercept-intercept)<1.8)

const reversed='Z-B_20260101-120004_003_3'
capture(reversed,'independent',[['Q1','B3',1,'qualification','H1','accel']],false,-1)
analyze(reversed)
assert.equal(report(reversed).accepted,true)
const reversePair=readFileSync(join(root,'results/b',reversed+'_pairs.csv'),'utf8').trim().split('\n')[1].split(',')
assert(Number(reversePair[23])<0 && Number(reversePair[25])<0 && Number(reversePair[26])>0)

const baseline = JSON.parse(run(process.execPath,[analyzer,'--a1-baseline']))
assert.equal(baseline.pairs,5)
assert(Math.abs(baseline.snr-15.875419888)<1e-7)
assert(Math.abs(baseline.meanAlpha-2.758135446884754e-5)<1e-12)
for (const path of [exe, join(root,'bin','gts.dll'), join(root,'ZRInertiaCurrent.vcxproj')]) unlinkSync(path)
console.log('B geometry, full and ranged schedules, qualification reuse, batch limit, held-out tiers, signed replay, regressions, resume, and A1 baseline PASS')

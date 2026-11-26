import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, realpathSync, rmSync, writeFileSync } from 'node:fs'
import { dirname, join, resolve, sep } from 'node:path'
import { fileURLToPath } from 'node:url'
import { loadModel, metadata } from './analyze_a1.mjs'
import { prediction, summarize } from './analyze_d.mjs'
import { batch } from './run_b_batch.mjs'

const project = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const parent = realpathSync(join(project,'verification'))
const root = mkdtempSync(join(parent,'d_replay_'))
const source = 'Z-BP_20261125-195157_221_35296'
const analyzer = join(project,'scripts/analyze_d.mjs')
const model = loadModel()

function save(path, text) {
  const target=join(root,path)
  mkdirSync(dirname(target),{recursive:true})
  writeFileSync(target,text)
}
function meta(path, values) {
  save(path+'.txt', Object.entries(values).map(([k,v])=>k+'='+v).join('\n')+'\n')
}
function run(exe,args,expected=0) {
  const r=spawnSync(exe,args,{cwd:project,encoding:'utf8',maxBuffer:2e6})
  assert.equal(r.status,expected,r.stdout+r.stderr)
  return r.stdout
}
const replay=(...args)=>run(process.execPath,[analyzer,'--root',root,...args])

try {
  for(const config of ['Debug','Release']){
    const exe=join(project,'bin/x64',config,'d_validate_inertia','ZRInertiaCurrent.exe')
    const preview=run(exe,['--plan'])
    assert.match(preview,/accel_pulse=17676 -> 20876/u)
    assert.match(preview,/decel_pulse=15124 -> 18324/u)
    assert.match(preview,/36\/44 ms; full 31 ms window/u)
  }
  for(const rel of ['manifest/b/'+source+'.txt','results/b/'+source+'_summary.json',
    'results/b/'+source+'_continuous_range_validation.csv']){
    mkdirSync(dirname(join(root,rel)),{recursive:true})
    copyFileSync(join(project,rel),join(root,rel))
  }
  const id=replay('--protocol-d',source).match(/Frozen D protocol: (Z-DP_\S+)/u)?.[1]
  assert.ok(id)
  const frozen=metadata(join(root,'manifest/d',id+'.txt'))
  const alpha=Number(frozen.alpha_raw_per_pulse_s2)
  assert.equal(frozen.alpha_raw_per_pulse_s2,'0.000030306147541113107')
  assert(Math.abs(prediction(alpha,1e6,0).predicted-30.306147541113107)<1e-10)
  const known=summarize([-1,0,1,2,3].map(residual=>({observed:30+residual,residual})),5,4,6)
  assert.equal(known.bias,1)
  assert(Math.abs(known.rmse-Math.sqrt(3))<1e-12)
  assert.equal(known.status,'passed')
  const withErrors=errors=>summarize(errors.map(residual=>({observed:30+residual,residual})),5,4,6)
  assert.deepEqual(withErrors([7,-7,7,-7,0]).reasons,['RMSE exceeds the frozen limit'])
  assert.deepEqual(withErrors([5,5,5,5,5]).reasons,['model underpredicts on average'])
  assert.equal(withErrors([4,4,4,4,4]).status,'passed')
  assert.equal(JSON.parse(replay(id)).status,'insufficient_valid_pairs')
  const slots=readFileSync(join(root,'manifest/d',id+'.plan.csv'),'utf8').trim().split('\n').slice(1)
  assert.equal(new Set(slots).size,5)

  function makeRun(session,pair,order,slot,role,bias) {
    const [planned_pair_id,condition,round,series,history_condition,first_role]=slot.split(',')
    const accel=role==='accel', acc=500000
    const [start,end]=accel?[17676,20876]:[15124,18324]
    const step=0.00005, move=2500, stopped=5900
    const key=session+'_p'+pair+'_r'+order
    const frames=['frame,card_tick_raw,encoder_position_raw,current_feedback_raw']
    for(let n=0;n<=stopped+3300;n++){
      const t=(n-move)*step
      const position=start+Math.round(t<0?0:t<0.08?acc*t*t/2:t<0.16?3200-acc*(0.16-t)**2/2:3200)
      const phase=t<0||t>=0.16?0:t<0.08?1:-1
      const current=Math.round(model.at(position/1000,1000)+100+phase*(alpha*acc+bias/2))
      frames.push([n,n,position,current].join(','))
    }
    save('data/raw/'+key+'.csv',frames.join('\n')+'\n')
    meta('manifest/'+key,{session_id:session,pair_id:pair,run_id:key,execution_order:order,role,direction:1,core_period_ns:50000,
      x0_plan_mm:18,x_match_eval_mm:18,start_mm:start/1000,end_mm:end/1000,
      planned_platform_begin_ms:accel?0:80,planned_platform_end_ms:accel?80:160,planned_match_ms:accel?36:124,
      planned_eval_crossing_ms:accel?36:124,command_before_tick:move-2,command_after_tick:move+2,settled_tick:stopped,
      tail_stable_begin_tick:stopped+5,tail_stable_end_tick:stopped+3005,tail_span_limit_pulse:7.5,
      outcome:'complete',watch_aligned_contiguous:1,run_target_readback_pulse:end,run_speed_readback_pulse_ms:40,
      planned_pair_id,condition,round,series,history_condition,first_role,preparation_hold_ms:250,
      preparation_speed_mm_s:2,preparation_targets_pulse:start})
  }
  function capture(session,indexes,interrupted=false,bias=0) {
    meta('manifest/'+session,{session_id:session,protocol_id:id,schema:frozen.schema,purpose:'validation',direction:1,
      model_sha256:frozen.model_sha256,b_range_sha256:frozen.b_range_sha256,core:2,axis:1,config:'a33_core2_axis1',
      slots_requested:indexes.length,planned_slots:indexes.map(i=>slots[i].split(',')[0]).join(','),
      operator_field_confirmation:'START',outcome:interrupted?'incomplete':'complete'})
    for(const [i,index] of indexes.entries()){
      const row=slots[index], [planned_pair_id,condition,round,series,history_condition,first_role]=row.split(',')
      const half=interrupted&&i===indexes.length-1, pair=i+1
      meta('manifest/'+session+'_p'+pair,{session_id:session,pair_id:pair,planned_pair_id,condition,round,series,
        history_condition,first_role,eval_pulse:18000,match_pulse_s:18000,acc_pulse_s2:500000,
        outcome:half?'incomplete':'complete'})
      makeRun(session,pair,1,row,first_role,bias)
      if(!half) makeRun(session,pair,2,row,first_role==='accel'?'decel':'accel',bias)
    }
  }
  const stopped='Z-D_20261126-080000_001_1', repaired='Z-D_20261126-080001_002_1', duplicate='Z-D_20261126-080002_003_1'
  capture(stopped,[0,1],true)
  const partial=JSON.parse(replay(id))
  assert.equal(partial.accepted,1)
  const second=slots[1].split(',')[0]
  assert.equal(partial.pending[0],second)
  assert.match(partial.pendingReasons[second][0],/interrupted/u)
  run(process.execPath,[analyzer,'--root',root,'--check-pair',id,stopped,'2'],3)
  capture(repaired,[1,2,3,4],false,40)
  capture(duplicate,[0])
  // The hardware-side quality check must accept even a deliberately large residual.
  assert.equal(replay('--check-pair',id,repaired,'1'),'')
  const result=JSON.parse(replay(id))
  assert.equal(result.accepted,5)
  assert.deepEqual(result.pending,[])
  assert.equal(result.status,'failed')
  assert(result.bias>25 && result.rmse>30)
  const prefix=join(root,'results/d',id)
  assert.match(readFileSync(prefix+'_pairs.csv','utf8'),/duplicate planned slot/u)
  assert.equal(metadata(prefix+'_resume.txt').accepted_slots.split(',').length,5)
  const planned=slots.map(s=>s.split(',')[0])
  let pending=[...planned], phases=[]
  const batchResult=batch(id,4,(phase,args,input)=>{
    phases.push(phase)
    if(phase==='pending') return pending.map(slot=>slot+' D H1').join('\n')
    if(phase==='acquire') { assert.equal(input,['F',id,4,'5000,500','START',''].join('\n')); return '' }
    if(phase==='before'||phase==='after') return 'ready=1\n'
    pending=pending.slice(4)
    return ''
  })
  assert.equal(batchResult,'Accepted 4, pending 1')
  assert.deepEqual(phases,['pending','before','acquire','after','replay','pending'])
  if(process.argv[2]){
    const matlab=process.argv[2]
    run(matlab,['-batch',"addpath('scripts'); plot_d_results('"+id+"','"+root.replaceAll("'","''")+"')"])
    for(const stem of ['01_observed_prediction','02_residuals','03_acceptance']) for(const format of ['svg','png','fig']){
      assert(readFileSync(join(root,'data/figures/D',id,id+'_'+stem+'.'+format)).length>0)
    }
  }
  const sourceTable=join(root,'results/b',source+'_continuous_range_validation.csv')
  writeFileSync(sourceTable,readFileSync(sourceTable,'utf8').replace(frozen.alpha_raw_per_pulse_s2,'0.0000309'))
  run(process.execPath,[analyzer,'--root',root,'--verify',id],1)
  console.log('D endpoints, exact freeze, quality-only acceptance, thresholds, interrupted/duplicate plan slots and batch routing PASS')
} finally {
  const target=realpathSync(root)
  assert(target.startsWith(parent+sep) && /^d_replay_[A-Za-z0-9]+$/.test(target.slice(parent.length+1)))
  rmSync(target,{recursive:true,force:false})
}

import { createHash } from 'node:crypto'
import { existsSync, mkdirSync, readFileSync, readdirSync, renameSync, statSync, writeFileSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { average, check, csv, loadModel, metadata, number } from './analyze_a1.mjs'
import { replay, selectAttempts } from './analyze_b.mjs'

const project = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const bIdPattern = /^Z-BP_\d{8}-\d{6}_\d+_\d+$/u
const dIdPattern = /^Z-DP_\d{8}-\d{6}_\d+_\d+$/u
const sessionPattern = /^Z-D_\d{8}-\d{6}_\d+_\d+$/u
const schema = 'd_validate_inertia_v1'
const panel = 'planned_pair_id,condition,round,series,history_condition,first_role'
const protocolKeys = {
  direction: 1, peak_pulse_s: 40000, scale: 1000, preparation_hold_ms: 250,
  fit_windows_ms: '15,21,31', current_windows_ms: '5,9,15', current_shift_ms: 0,
  position_tol_mm: 0.01, speed_target_tol_mm_s: 0.5, speed_pair_tol_mm_s: 0.5,
  fit_stability_fraction: 0.1, model: 'position_B_primary_v1', rules: 'b_matched_inertia_v1'
}
const endpoints = { accel: [17676, 20876], decel: [15124, 18324] }
const hash = path => createHash('sha256').update(readFileSync(path)).digest('hex')
const fields = values => Object.entries(values).map(([key,value]) => key+'='+value).join('\n')+'\n'
const file = (root,id) => join(root,'manifest/d',id+'.txt')
const planPath = (root,id) => join(root,'manifest/d',id+'.plan.csv')
const slotsFor = id => Array.from({length:5}, (_,i) => ({
  id: id+'_P'+(i+1), condition: 'D', round:i+1, series:'validation', history:'H1',
  first:i%2===0?'accel':'decel'
}))
const planText = slots => panel+'\n'+slots.map(slot=>Object.values(slot).join(',')).join('\n')+'\n'

function source(root,bId,model) {
  check(bIdPattern.test(bId), 'invalid source B protocol ID')
  const bPath = join(root,'manifest/b',bId+'.txt')
  const table = join(root,'results/b',bId+'_continuous_range_validation.csv')
  const summaryPath = join(root,'results/b',bId+'_summary.json')
  const b = metadata(bPath), summary = JSON.parse(readFileSync(summaryPath,'utf8'))
  check(b.protocol_id===bId && b.matrix_design==='expanded_range_v1' && b.model_sha256===model.hash &&
    Object.entries(protocolKeys).every(([key,value])=>b[key]===String(value)), 'B protocol, model or quality rules mismatch')
  check(summary.protocol===bId && summary.status==='matrix_complete' && summary.accepted===80 &&
    summary.pending.length===0, 'source B matrix is incomplete')
  const [header,...lines] = readFileSync(table,'utf8').trim().split(/\r?\n/u)
  const names = header.split(',')
  check(['first_acc_pulse_s2','last_acc_pulse_s2','tiers','pairs','alpha_raw_per_pulse_s2'].every(k=>names.includes(k)),
    'missing source columns')
  const rows = lines.map(line=>Object.fromEntries(line.split(',').map((value,i)=>[names[i],value])))
    .filter(row=>row.first_acc_pulse_s2==='400000' && row.last_acc_pulse_s2==='600000')
  const alpha = rows[0]?.alpha_raw_per_pulse_s2
  check(rows.length===1 && rows[0].tiers==='5' && rows[0].pairs==='50' &&
    Number.isFinite(Number(alpha)) && Number(alpha)>0, 'missing exact B model interval or coefficient')
  const reserved = {plan_pulse:'d_plan_pulse',eval_pulse:'d_eval_pulse',match_pulse_s:'d_speed_pulse_s',
    acc_pulse_s2:'d_acc_pulse_s2',planned_pairs:'d_min_pairs',max_abs_bias_raw:'d_max_abs_bias_raw',
    max_rmse_raw:'d_max_rmse_raw'}
  const d = Object.fromEntries(Object.entries(reserved).map(([k,v])=>[k,number(b,v)]))
  check(d.plan_pulse===18000 && d.eval_pulse===18000 && d.match_pulse_s===18000 &&
    d.acc_pulse_s2===500000 && d.planned_pairs===5 && d.max_abs_bias_raw===4 && d.max_rmse_raw===6 &&
    Boolean(b.d_time), 'source D reservation does not match this validated planner')
  return { schema, b_protocol_id:bId, b_protocol_sha256:hash(bPath), b_range_sha256:hash(table),
    b_summary_sha256:hash(summaryPath), model_sha256:model.hash, training_first_acc_pulse_s2:400000,
    training_last_acc_pulse_s2:600000, training_pairs:50, alpha_raw_per_pulse_s2:alpha, intercept_raw:0,
    ...protocolKeys,...d, independent_session_label:b.d_time, preparation:'H1', max_batch_pairs:4,
    primary_fit_ms:21, primary_current_ms:5, residual_definition:'observed_corrected_minus_predicted' }
}

function nextId() {
  const now = new Date()
  const parts = Object.fromEntries(new Intl.DateTimeFormat('en-GB', {timeZone:'Asia/Shanghai',
    year:'numeric',month:'2-digit',day:'2-digit',hour:'2-digit',minute:'2-digit',second:'2-digit',hourCycle:'h23'})
    .formatToParts(now).map(p=>[p.type,p.value]))
  return ['Z-DP_'+parts.year+parts.month+parts.day+'-'+parts.hour+parts.minute+parts.second,
    String(now.getMilliseconds()).padStart(3,'0'),process.pid].join('_')
}
function freeze(root,bId,model,revision={}) {
  const values=source(root,bId,model), id=nextId(), slots=slotsFor(id)
  mkdirSync(dirname(file(root,id)),{recursive:true})
  writeFileSync(planPath(root,id),planText(slots),{flag:'wx'})
  writeFileSync(file(root,id),fields({protocol_id:id,...values,...revision,plan_sha256:hash(planPath(root,id))}),{flag:'wx'})
  console.log('Frozen D protocol: '+id)
  console.log('Fixed alpha: '+values.alpha_raw_per_pulse_s2+' raw/(pulse/s^2); 5 pairs, batch maximum 4')
}
function load(root,id,model) {
  check(dIdPattern.test(id), 'invalid D protocol ID')
  const f=metadata(file(root,id))
  const expected=source(root,f.b_protocol_id,model)
  const slots=slotsFor(id), path=planPath(root,id)
  check(f.protocol_id===id && Object.entries(expected).every(([key,value])=>f[key]===String(value)),
    'D snapshot, B provenance or quality rules changed')
  check(f.plan_sha256===hash(path) && readFileSync(path,'utf8').replace(/\r\n/gu,'\n')===planText(slots),
    'D frozen point table mismatch')
  const nominal=number(f,'plan_pulse')
  let center=nominal
  if(f.parent_d_protocol_id!==undefined){
    const parentId=f.parent_d_protocol_id, shift=number(f,'trajectory_shift_pulse')
    check(dIdPattern.test(parentId) && parentId!==id && sessionPattern.test(f.revision_basis_session) &&
      Number.isInteger(shift) && Math.abs(shift)<=50 && shift!==0 &&
      f.parent_d_protocol_sha256===hash(file(root,parentId)), 'D revision provenance mismatch')
    const parent=metadata(file(root,parentId))
    check(parent.b_protocol_id===f.b_protocol_id && parent.eval_pulse===f.eval_pulse &&
      f.revision_basis_sha256===revisionDigest(root,f.revision_basis_session), 'D revision evidence changed')
    center=Number(parent.trajectory_plan_pulse ?? parent.plan_pulse)+shift
    check(number(f,'trajectory_plan_pulse')===center && Math.abs(center-nominal)<=50,
      'D translation outside the frozen revision')
  } else {
    check(f.trajectory_plan_pulse===undefined,'D translation requires a parent protocol')
  }
  const p={xPlan:center/1000,xEval:number(f,'eval_pulse')/1000,acc:number(f,'acc_pulse_s2')/1000,
    speed:number(f,'match_pulse_s')/1000,peak:number(f,'peak_pulse_s')/1000,dir:number(f,'direction'),scale:number(f,'scale'),
    xTol:number(f,'position_tol_mm'),targetTol:number(f,'speed_target_tol_mm_s'),speedTol:number(f,'speed_pair_tol_mm_s'),
    stability:number(f,'fit_stability_fraction')}
  return {f,p,slots}
}

function readAttempt(root,id,protocol) {
  check(sessionPattern.test(id),'invalid D session ID')
  const path = join(root,'manifest',id+'.txt')
  const m = metadata(path)
  const {f,slots}=protocol
  check(m.schema===schema && m.session_id===id && m.protocol_id===f.protocol_id && m.purpose==='validation' &&
    m.config==='a33_core2_axis1' && m.core==='2' && m.axis==='1' && m.direction===f.direction &&
    m.model_sha256===f.model_sha256 && m.b_range_sha256===f.b_range_sha256 &&
    m.operator_field_confirmation==='START', 'D attempt identity, model or start confirmation mismatch')
  const planned=m.planned_slots.split(',').map(key=>slots.findIndex(s=>s.id===key))
  check(planned.length===number(m,'slots_requested') && planned.length>=1 && planned.length<=4 &&
    planned.every((n,i)=>n>=0 && (i===0 || n>planned[i-1])), 'D attempt violates plan or batch cap')
  return {id,m,path,planned:planned.map(i=>slots[i])}
}
async function quality(root,attempt,i,protocol,model) {
  const row=await replay(root,attempt.id,attempt.m,i,attempt.planned[i-1],protocol.p,model,protocol.f)
  const order=row.firstRole==='accel'?['accel','decel']:['decel','accel']
  row.recordedIds={}
  for(const [i,role] of order.entries()){
    const key=attempt.id+'_p'+row.pair+'_r'+(i+1)
    if(existsSync(join(root,'manifest',key+'.txt'))) row.recordedIds[role]=key
  }
  if(row.valid) for(const run of [row.accel,row.decel]){
    const m=metadata(join(root,'manifest',run.key+'.txt'))
    const shift=protocol.p.xPlan*1000-number(protocol.f,'plan_pulse')
    const [start,end]=endpoints[run.role].map(value=>Math.round(value+shift))
    if(Math.round(number(m,'start_mm')*1000)!==start || Math.round(number(m,'end_mm')*1000)!==end ||
        m.run_target_readback_pulse!==String(end) || !Number.isFinite(Number(m.run_speed_readback_pulse_ms)) ||
        Math.abs(Number(m.run_speed_readback_pulse_ms)-40)>0.01 ||
        !['tail_stable_begin_tick','tail_stable_end_tick','tail_span_limit_pulse'].every(key=>m[key]!==undefined)){
      row.issues.push(run.role+': frozen endpoints, peak readback or stationary tail evidence mismatch')
      row.valid=false
    }
  }
  return row
}
export function prediction(alpha, actualAccelerationDifference, actualCurrentDifference) {
  const predicted=alpha*actualAccelerationDifference
  return {predicted,residual:actualCurrentDifference-predicted}
}
export function summarize(observations, expectedPairs, biasLimit, rmseLimit) {
  const errors=observations.map(r=>r.residual)
  const n=errors.length, bias=n?average(errors):null
  const rmse=n?Math.sqrt(average(errors.map(e=>e*e))):null
  const baselineRmse=n?Math.sqrt(average(observations.map(r=>r.observed**2))):null
  const reasons=[]
  if(n===expectedPairs){
    if(Math.abs(bias)>biasLimit) reasons.push(bias>0?'model underpredicts on average':'model overpredicts on average')
    if(rmse>rmseLimit) reasons.push('RMSE exceeds the frozen limit')
  }
  return { status:n<expectedPairs?'insufficient_valid_pairs':reasons.length?'failed':'passed', reasons, bias, rmse, baselineRmse,
    residualScatterRmse:n?Math.sqrt(average(errors.map(e=>(e-bias)**2))):null,
    rmseImprovementRaw:n?baselineRmse-rmse:null }
}
const pulse = x => x===null || x===undefined ? null : x*1000
function output(root,protocol,rows,accepted,attempts,alpha) {
  const {f,slots}=protocol, prefix=join(root,'results/d',f.protocol_id)
  mkdirSync(dirname(prefix),{recursive:true})
  const pending=slots.filter(s=>!accepted.has(s.id))
  const observations=[]
  const pairs=[['planned_pair_id','session_id','pair_id','first_role','accel_run_id','decel_run_id',
    'accel_x_pulse','decel_x_pulse','accel_v_pulse_s','decel_v_pulse_s','accel_a_pulse_s2','decel_a_pulse_s2',
    'delta_x_pulse','delta_v_pulse_s','delta_a_pulse_s2','delta_I_raw','delta_I_corr_raw','predicted_raw','residual_raw','valid','reason']]
  const sensitivity=[['planned_pair_id','session_id','pair_id','fit_ms','current_ms','delta_a_pulse_s2','delta_I_corr_raw','predicted_raw','residual_raw']]
  for(const row of rows){
    const a=row.accel?.position.get(21),d=row.decel?.position.get(21)
    const p=row.valid?prediction(alpha,pulse(row.da),row.di):null
    if(p) observations.push({observed:row.di,...p})
    pairs.push([row.slot.id,row.id,row.pair,row.firstRole,row.recordedIds.accel,row.recordedIds.decel,pulse(a?.x),pulse(d?.x),pulse(a?.v),pulse(d?.v),
      pulse(a?.a),pulse(d?.a),pulse(row.dx),pulse(row.dv),pulse(row.da),row.raw,row.di,p?.predicted,p?.residual,Number(row.valid),
      row.issues.join(' | ')])
    if(!p) continue
    for(const [fit,win] of [[15,5],[21,5],[31,5],[21,9],[21,15]]){
      const da=pulse(row.accel.position.get(fit).a-row.decel.position.get(fit).a)
      const di=row.accel.current.get(win).corrected-row.decel.current.get(win).corrected
      const v=prediction(alpha,da,di)
      sensitivity.push([row.slot.id,row.id,row.pair,fit,win,da,di,v.predicted,v.residual])
    }
  }
  const result={protocol:f.protocol_id,bProtocol:f.b_protocol_id,alpha,accepted:accepted.size,planned:slots.length,
    sessions:attempts.map(a=>a.id),pending:pending.map(x=>x.id),
    pendingReasons:Object.fromEntries(pending.map(slot=>[slot.id,
      rows.filter(row=>row.slot.id===slot.id).at(-1)?.issues ?? ['not yet recorded']])),
    limits:{absBias:number(f,'max_abs_bias_raw'),rmse:number(f,'max_rmse_raw')},
    ...summarize(observations,slots.length,number(f,'max_abs_bias_raw'),number(f,'max_rmse_raw'))}
  writeFileSync(prefix+'_pairs.csv',csv(pairs))
  writeFileSync(prefix+'_sensitivity.csv',csv(sensitivity))
  writeFileSync(prefix+'_pending.csv',planText(pending))
  writeFileSync(prefix+'_summary.json',JSON.stringify(result,null,2)+'\n')
  const revision=attempts.map(a=>a.id+':'+a.size).join(',')
  const receipt=prefix+'_resume.txt'
  writeFileSync(receipt+'.tmp',fields({protocol_id:f.protocol_id,seen_revision:revision,
    accepted_slots:slots.filter(x=>accepted.has(x.id)).map(x=>x.id).join(',')}))
  renameSync(receipt+'.tmp',receipt)
  return result
}

async function formal(root,id,model) {
  const protocol=load(root,id,model)
  const lock=join(root,'manifest/d',id+'.lock')
  check(!existsSync(lock),'close active D acquisition before replay')
  const attempts=[],rows=[]
  for(const name of readdirSync(join(root,'manifest')).sort()){
    const sid=name.endsWith('.txt')?name.slice(0,-4):''
    if(!sessionPattern.test(sid)) continue
    const m=metadata(join(root,'manifest',name))
    if(m.protocol_id!==id || m.operator_field_confirmation!=='START') continue
    const a=readAttempt(root,sid,protocol)
    a.size=statSync(a.path).size
    attempts.push(a)
    for(let i=1;i<=a.planned.length;i++) rows.push(await quality(root,a,i,protocol,model))
  }
  const accepted=selectAttempts(rows)
  check(!existsSync(lock) && attempts.every(a=>statSync(a.path).size===a.size),
    'D acquisition changed during replay')
  return output(root,protocol,rows,accepted,attempts,number(protocol.f,'alpha_raw_per_pulse_s2'))
}

function revisionDigest(root,id) {
  const records=[join(root,'manifest',id+'.txt'), join(root,'manifest',id+'_p1.txt')]
  for(const i of [1,2]){
    const key=id+'_p1_r'+i
    records.push(join(root,'manifest',key+'.txt'),join(root,'data/raw',key+'.csv'))
  }
  return createHash('sha256').update(records.map(hash).join('\n')).digest('hex')
}

async function revise(root,parentId,sessionId,offset,model) {
  const parent=load(root,parentId,model), a=readAttempt(root,sessionId,parent)
  check(a.planned[0]?.round===1 && Number.isInteger(offset) && offset!==0 && Math.abs(offset)<=50,
    'D revision needs the first completed diagnostic pair and an integer translation')
  const row=await quality(root,a,1,parent,model)
  check(row.accel && row.decel && !row.valid && row.issues.some(s=>/speed outside|velocity mismatch/u.test(s)),
    'D revision requires evidence of first-pair velocity mismatch')
  const attempted=await formal(root,parentId,model)
  check(attempted.accepted===0,'an accepted D pair already exists in the parent protocol')
  const center=Math.round(parent.p.xPlan*1000)+offset
  check(Math.abs(center-number(parent.f,'plan_pulse'))<=50,'D translation exceeds the permitted preview range')
  freeze(root,parent.f.b_protocol_id,model,{parent_d_protocol_id:parentId,
    parent_d_protocol_sha256:hash(file(root,parentId)),revision_basis_session:sessionId,
    revision_basis_sha256:revisionDigest(root,sessionId),trajectory_shift_pulse:offset,trajectory_plan_pulse:center,
    revision_reason:'encoder_speed_match_common_translation'})
}

async function main() {
  const args=process.argv.slice(2)
  const index=args.indexOf('--root')
  const root=resolve(index<0?project:args.splice(index,2)[1])
  const model=loadModel()
  if(args[0]==='--protocol-d' && args.length===2) return freeze(root,args[1],model)
  if(args[0]==='--revise-d' && args.length===4) return revise(root,args[1],args[2],Number(args[3]),model)
  if(args[0]==='--verify' && args.length===2){ load(root,args[1],model); return }
  if(args[0]==='--check-pair' && args.length===4){
    const p=load(root,args[1],model), a=readAttempt(root,args[2],p), n=Number(args[3])
    check(Number.isInteger(n)&&n>=1&&n<=a.planned.length,'invalid pair ordinal')
    const row=await quality(root,a,n,p,model)
    if(!row.valid){ console.error('D quality failed; pause before next pair: '+row.issues.join(' | ')); process.exitCode=3 }
    return
  }
  check(args.length===1 && dIdPattern.test(args[0]),'use --protocol-d <B ID>, --verify <D ID>, or <D protocol ID>')
  console.log(JSON.stringify(await formal(root,args[0],model),null,2))
}
if(process.argv[1] && import.meta.url===pathToFileURL(resolve(process.argv[1])).href){
  main().catch(error=>{console.error(error.message);process.exitCode=1})
}

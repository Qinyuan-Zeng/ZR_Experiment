import { createHash } from 'node:crypto'
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { average, check, checkMotion, clockDelta, csv, fit, frames, loadModel, lowerBound, metadata, number,
  sd, t975 } from './analyze_a1.mjs'
import { loadSchedule } from './analyze_b.mjs'

const project = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const bands = [{ name:'main', low:12000, high:24000 }, { name:'sensitivity', low:16000, high:28000 }]
const settings = { bands, fitMs:11, speedGridMs:0.25, speedTimeReference:'included_frame_midpoint', binWidth:3000, totalDwellLimit:0.10,
  binDwellLimit:0.20, minimumDeltaV:10000, rawToMa:1.2, firstTier:400000, lastTier:600000 }
const hash = path => createHash('sha256').update(readFileSync(path)).digest('hex')
const writeJson = (path,obj) => writeFileSync(path,JSON.stringify(obj,null,2)+'\n')
const pct = (a,b) => 2*Math.abs(a-b)/(a+b)
const within = (a,b) => Math.abs(a-b)<1e-6

function readCsv(path) {
  const [first,...lines]=readFileSync(path,'utf8').trimEnd().split(/\r?\n/u)
  const columns=first.split(',')
  return lines.map(line=>{
    const values=[...line.matchAll(/(?:^|,)(?:"((?:[^"]|"")*)"|([^,]*))/gu)]
      .map(m=>(m[1]??m[2]).replaceAll('""','"'))
    check(values.length===columns.length,'malformed CSV row in '+path)
    return Object.fromEntries(columns.map((key,i)=>[key,values[i]]))
  })
}

function source(root,id,model) {
  check(/^Z-BP_\d{8}-\d{6}_\d+_\d+$/u.test(id),'use an expanded B protocol ID')
  const {protocol,conditions,slots}=loadSchedule(root,id,model)
  const pairsPath=join(root,'results/b',id+'_pairs.csv')
  const summaryPath=join(root,'results/b',id+'_summary.json')
  const protocolPath=join(root,'manifest/b',id+'.txt')
  const planPath=join(root,'manifest/b',id+'.plan.csv')
  const rangePath=join(root,'results/b',id+'_continuous_range_validation.csv')
  const summary=JSON.parse(readFileSync(summaryPath,'utf8'))
  check(protocol.matrix_design==='expanded_range_v1' && protocol.direction==='1' &&
    summary.protocol===id && summary.status==='matrix_complete' && summary.accepted===slots.length &&
    slots.every(s=>s.series==='matrix' && s.history==='H1'),'expected a complete positive expanded B matrix')
  const pairs=readCsv(pairsPath).filter(r=>r.valid==='1')
  const plan=new Map(slots.map(s=>[s.id,s]))
  const runKeys=new Set()
  check(pairs.length===slots.length && new Set(pairs.map(r=>r.planned_pair_id)).size===slots.length,
    'accepted rows do not cover all planned pairs exactly once')
  for(const r of pairs){
    const slot=plan.get(r.planned_pair_id), p=conditions.get(r.condition)
    check(slot && p && slot.condition===r.condition && slot.first===r.first_role && slot.round===number(r,'round') &&
      r.series==='matrix' && r.history_condition==='H1' && summary.sessions.includes(r.session_id) &&
      number(r,'eval_pulse')===Math.round(p.xEval*1000) && number(r,'nominal_A_pulse_s2')===p.acc*1000 &&
      number(r,'nominal_v_pulse_s')===p.speed*1000,'accepted pair disagrees with frozen slot')
    check(/^Z-B_\d{8}-\d{6}_\d+_\d+$/u.test(r.session_id) && Number.isInteger(number(r,'pair_id')),'invalid run identity')
    for(const role of ['accel','decel']){
      const run=r[role+'_run_id']
      check(run===r.session_id+'_p'+r.pair_id+'_r'+(role===slot.first?1:2) && !runKeys.has(run),'duplicate or mismatched run')
      runKeys.add(run)
    }
  }
  const modelRows=readCsv(rangePath).filter(r=>number(r,'first_acc_pulse_s2')===settings.firstTier &&
    number(r,'last_acc_pulse_s2')===settings.lastTier)
  check(modelRows.length===1 && number(modelRows[0],'pairs')===50,'frozen B middle-range model is missing')
  const alpha=number(modelRows[0],'alpha_raw_per_pulse_s2')
  const traceHash=createHash('sha256')
  for(const key of runKeys) for(const path of [join(root,'manifest',key+'.txt'),join(root,'data/raw',key+'.csv')]){
    traceHash.update(key+'\n'+(existsSync(path)?hash(path):'missing')+'\n')
  }
  return { pairs, conditions, alpha, provenance:{model:protocol.model,model_sha256:model.hash,protocol_sha256:hash(protocolPath),
    point_table_sha256:hash(planPath),accepted_pairs_sha256:hash(pairsPath),range_sha256:hash(rangePath),
    raw_and_run_manifests_sha256:traceHash.digest('hex')} }
}

function speedProfile(data, span, fitMs, gridMs) {
  const {begin,end}=span
  const samples=[]
  for(let i=0, t=begin+fitMs/2;t<=end-fitMs/2+1e-9;t=begin+fitMs/2+(++i)*gridMs){
    const result=fit(data,t,fitMs,begin,end,j=>data.x[j],1)
    samples.push({t:result.sampleCenter,v:result.v*1000})
  }
  return samples
}
function cross(samples,edge,direction) {
  const hits=[]
  for(let i=1;i<samples.length;i++){
    const a=samples[i-1],b=samples[i]
    const up=a.v<edge && b.v>=edge, down=a.v>edge && b.v<=edge
    if(up||down) hits.push({a,b,direction:up?1:-1})
  }
  check(hits.length!==0,'incomplete coverage at '+edge+' pulse/s')
  check(hits.length===1,'multiple crossings at '+edge+' pulse/s')
  const {a,b,direction:actual}=hits[0]
  check(actual===direction,'wrong crossing direction at '+edge+' pulse/s')
  const f=(edge-a.v)/(b.v-a.v)
  return {t:a.t+f*(b.t-a.t),v:a.v+f*(b.v-a.v)}
}
function interpolate(data,time,values) {
  const hi=lowerBound(data.t,time)
  check(hi<data.t.length && hi>0,'interpolation outside card time axis')
  const f=(time-data.t[hi-1])/(data.t[hi]-data.t[hi-1])
  return values(hi-1)+f*(values(hi)-values(hi-1))
}
function bandSegment(data,profile,edges,direction) {
  const cuts=edges.map(edge=>cross(profile,edge,direction))
  const bins=cuts.slice(1).map((v,i)=>direction*(v.t-cuts[i].t))
  check(bins.every(t=>t>0),'speed bins were not crossed in temporal order')
  const a=direction>0?cuts[0]:cuts.at(-1), b=direction>0?cuts.at(-1):cuts[0]
  check(profile.every(s=>s.t<a.t || s.t>b.t || s.v>0),'motion reverses inside the band')
  const startX=interpolate(data,a.t,i=>data.x[i]), endX=interpolate(data,b.t,i=>data.x[i])
  const from=lowerBound(data.t,a.t), to=lowerBound(data.t,b.t)
  let minX=Math.min(startX,endX), maxX=Math.max(startX,endX)
  for(let i=from;i<to;i++){minX=Math.min(minX,data.x[i]);maxX=Math.max(maxX,data.x[i])}
  check(minX>=1 && maxX<=31,'band position outside the frozen model')
  check(endX>startX,'encoder position direction mismatch')
  return {begin:a.t,end:b.t,beginSpeed:a.v,endSpeed:b.v,dt:b.t-a.t,dv:b.v-a.v,beginX:startX,endX,minX,maxX,bins}
}
export function geometry(data,spans,band,options=settings) {
  const edges=[]
  for(let v=band.low;v<=band.high;v+=options.binWidth) edges.push(v)
  check(edges.at(-1)===band.high,'band is not divisible into velocity bins')
  const profiles=spans.map(span=>speedProfile(data,span,options.fitMs,options.speedGridMs))
  const [acc,dec]=profiles.map((samples,i)=>bandSegment(data,samples,edges,i===0?1:-1))
  const totalMismatch=pct(acc.dt,dec.dt), binMismatches=acc.bins.map((t,i)=>pct(t,dec.bins[i]))
  const worstBin=Math.max(...binMismatches), denominator=acc.dv-dec.dv, reasons=[]
  if(totalMismatch>options.totalDwellLimit) reasons.push('total dwell mismatch')
  if(worstBin>options.binDwellLimit) reasons.push('velocity bin dwell mismatch')
  if(denominator<options.minimumDeltaV) reasons.push('velocity increment difference too small')
  return {acc,dec,profiles,totalMismatch,binMismatches,worstBin,denominator,reasons}
}

function integrate(data,seg,values) {
  let lastT=seg.begin,lastI=interpolate(data,lastT,values),sum=0
  for(let i=lowerBound(data.t,seg.begin);i<data.t.length && data.t[i]<seg.end;i++){
    const t=data.t[i], y=values(i)
    sum+=(t-lastT)*(y+lastI)/2
    lastT=t
    lastI=y
  }
  return (sum+(seg.end-lastT)*(interpolate(data,seg.end,values)+lastI)/2)/1000
}
export function integrateDifference(data,g,model,alpha,scale=1000) {
  const raw=i=>data.current[i], corrected=i=>raw(i)-model.at(data.x[i],scale)
  const rawAcc=integrate(data,g.acc,raw), rawDec=integrate(data,g.dec,raw)
  const corrAcc=integrate(data,g.acc,corrected), corrDec=integrate(data,g.dec,corrected)
  const rawDelta=rawAcc-rawDec,corrDelta=corrAcc-corrDec,predicted=alpha*g.denominator
  const residual=corrDelta-predicted, coefficient=corrDelta/g.denominator
  return {rawAcc,rawDec,corrAcc,corrDec,rawDelta,corrDelta,predicted,residual,coefficient,
    residualMa:residual*settings.rawToMa,eqMa:residual/((g.acc.dt+g.dec.dt)/2000)*settings.rawToMa}
}

async function trace(root,row,role,includeCurrent) {
  const key=row[role+'_run_id'], m=metadata(join(root,'manifest',key+'.txt'))
  check(m.run_id===key && m.session_id===row.session_id && m.planned_pair_id===row.planned_pair_id &&
    m.role===role && m.direction==='1' && m.outcome==='complete' && m.watch_aligned_contiguous==='1',
  'run manifest identity, quality or direction mismatch')
  const tick=number(m,'core_period_ns')
  check(tick>0 && Number.isInteger(tick),'invalid card tick')
  const data=await frames(join(root,'data/raw',key+'.csv'),tick/1e6,1000,includeCurrent)
  check(!data.breaks,'clock gap, repetition or backwards tick')
  const stamp=name=>{
    const value=number(m,name)
    check(Number.isInteger(value)&&value>=0&&value<2**32,'invalid card event tick')
    const ms=clockDelta(value,data.firstTick)*tick/1e6
    check(ms<=data.t.at(-1),'card event is outside the acquisition')
    return ms
  }
  const before=stamp('command_before_tick'),after=stamp('command_after_tick'),settled=stamp('settled_tick')
  check(before<=after && after<settled,'invalid motion event timing')
  const from=number(m,'start_mm'),to=number(m,'end_mm')
  checkMotion(data,from,to,1,before,settled,0.01)
  const acc=number(m,'run_trap_acc_pulse_ms2'),dec=number(m,'run_trap_dec_pulse_ms2')
  const distance=1000*(to-from), inverse=1/acc+1/dec
  check(distance>0 && acc>0 && dec>0 && number(m,'run_trap_smooth_ms')===0 &&
    number(m,'run_trap_vel_start_pulse_ms')===0,'expected a complete positive zero-smoothing TRAP trajectory')
  const top=Math.min(number(m,'run_speed_readback_pulse_ms'),Math.sqrt(2*distance/inverse))
  const rise=top/acc,fall=top/dec
  const coast=Math.max(0,(distance-top*(rise+fall)/2)/top), duration=rise+coast+fall
  const [nominalStart,nominalEnd]=role==='accel'?[0,rise]:[rise+coast,duration]
  check(within(nominalStart,number(m,'planned_platform_begin_ms')) &&
    within(nominalEnd,number(m,'planned_platform_end_ms')) &&
    within(duration,number(m,'planned_duration_ms')) && before+duration<settled,'TRAP phase timing disagrees with manifest')
  const spans=[{begin:after,end:before+rise},{begin:after+rise+coast,end:before+duration}]
  return {data,spans}
}

const segHeaders=['begin_ms','end_ms','begin_pulse_s','end_pulse_s','min_mm','max_mm','duration_ms','delta_v_pulse_s','bins_ms']
const segData=s=>s?[s.begin,s.end,s.beginSpeed,s.endSpeed,s.minX,s.maxX,s.dt,s.dv,JSON.stringify(s.bins)]:[]
const idHeaders=['planned_pair_id','run_id','run_role','condition','acc_pulse_s2','b_match_pulse_s','band','valid','reason']
const geometryHeaders=[...idHeaders,...['acc','dec'].flatMap(prefix=>segHeaders.map(k=>prefix+'_'+k)),
  'total_dwell_mismatch','max_bin_dwell_mismatch','bin_dwell_mismatches','delta_v_difference_pulse_s']
const integrals=['raw_acc_raw_s','raw_dec_raw_s','corrected_acc_raw_s','corrected_dec_raw_s','raw_delta_raw_s',
  'corrected_delta_raw_s','b_predicted_delta_raw_s','residual_raw_s','alpha_c_raw_per_pulse_s2',
  'residual_mA_s','equivalent_error_mA']

function keyRow(row,g) {
  const seg=s=>s?segData(s):Array(segHeaders.length).fill(null)
  return [row.slot,row.run,row.role,row.condition,row.acc,row.speed,row.band,Number(row.valid),row.reasons.join(' | '),
    ...seg(g?.acc),...seg(g?.dec),g?.totalMismatch,g?.worstBin,g?JSON.stringify(g.binMismatches):null,g?.denominator]
}
function meanCI(values) {
  const n=values.length, mean=n?average(values):null
  if(n<2) return {n,mean,ci95:null}
  const df=n-1, crit=t975[df]??(1.959964+2.3723/df+2.8225/df**2)
  const radius=crit*sd(values,mean)/Math.sqrt(n)
  return {n,mean,ci95:[mean-radius,mean+radius]}
}
const middle=acc=>acc>=settings.firstTier && acc<=settings.lastTier
function pairResults(inputs,rows) {
  const index=new Map(rows.map(r=>[r.run+'/'+r.band,r]))
  return bands.flatMap(b=>inputs.map(r=>{
    const a=index.get(r.accel_run_id+'/'+b.name),d=index.get(r.decel_run_id+'/'+b.name)
    const valid=a.valid&&d.valid, mean=k=>valid&&a.result&&d.result?(a.result[k]+d.result[k])/2:null
    return {slot:r.planned_pair_id,condition:r.condition,acc:number(r,'nominal_A_pulse_s2'),speed:number(r,'nominal_v_pulse_s'),
      band:b.name,valid,reason:[...a.reasons.map(s=>'accel role: '+s),...d.reasons.map(s=>'decel role: '+s)].join(' | '),
      runA:a.run,runD:d.run,aAlpha:a.result?.coefficient,dAlpha:d.result?.coefficient,
      bPairAlpha:number(r,'alpha_raw_per_pulse_s2'),alpha:mean('coefficient'),residualMa:mean('residualMa'),equivalentMa:mean('eqMa')}
  }))
}
function coverage(rows,pairs) {
  return Object.fromEntries(bands.map(b=>{
    const rr=rows.filter(r=>r.band===b.name),pp=pairs.filter(r=>r.band===b.name)
    const count=ar=>({total:ar.length,valid:ar.filter(x=>x.valid).length})
    return [b.name,{traces:count(rr),pairs:count(pp),primaryTraces:count(rr.filter(x=>middle(x.acc))),
      primaryPairs:count(pp.filter(x=>middle(x.acc))),
      byCondition:Object.fromEntries([...new Set(pp.map(x=>x.condition))].sort().map(c=>[c,count(pp.filter(x=>x.condition===c))])),
      reasons:Object.fromEntries([...new Set(rr.flatMap(x=>x.reasons))].map(reason=>[reason,rr.filter(x=>x.reasons.includes(reason)).length]))}]
  }))
}
function groups(pairs,alpha) {
  return bands.flatMap(b=>{
    const rows=pairs.filter(r=>r.band===b.name)
    const results=[]
    const add=(group,items)=>{
      const valid=items.filter(r=>r.valid)
      results.push({band:b.name,group,total:items.length,coefficient:meanCI(valid.map(r=>r.alpha)),
        differenceFromB:meanCI(valid.map(r=>r.alpha-alpha)),samePairDifference:meanCI(valid.map(r=>r.alpha-r.bPairAlpha)),
        residualMaS:meanCI(valid.map(r=>r.residualMa)),
        residualRmseMaS:valid.length?Math.sqrt(average(valid.map(r=>r.residualMa**2))):null,
        equivalentErrorMa:meanCI(valid.map(r=>r.equivalentMa))})
    }
    add('primary',rows.filter(r=>middle(r.acc)))
    add('outside_primary',rows.filter(r=>!middle(r.acc)))
    for(const speed of new Set(rows.map(r=>r.speed))) add('primary_speed_'+speed,rows.filter(r=>middle(r.acc)&&r.speed===speed))
    for(const tier of [...new Set(rows.map(r=>r.acc))].sort((a,b)=>a-b)){
      add('tier_'+tier,rows.filter(r=>r.acc===tier))
      for(const speed of new Set(rows.map(r=>r.speed))) add('tier_'+tier+'_speed_'+speed,rows.filter(r=>r.acc===tier&&r.speed===speed))
    }
    return results
  })
}

async function main() {
  const args=process.argv.slice(2), index=args.indexOf('--root')
  const root=resolve(index<0?project:args.splice(index,2)[1])
  const [id,mode,...extra]=args, onlyGeometry=mode==='--check'
  check(id && (!mode||onlyGeometry) && !extra.length,'use <B protocol ID> [--check]')
  const model=loadModel(), b=source(root,id,model)
  const prefix=join(root,'results/c',id)
  mkdirSync(dirname(prefix),{recursive:true})
  const snapshot={id,settings,bAlpha:b.alpha,provenance:b.provenance}
  if(!onlyGeometry){
    const checked=JSON.parse(readFileSync(prefix+'_check.json','utf8'))
    check(JSON.stringify(checked.snapshot)===JSON.stringify(snapshot),'run --check for the current frozen inputs and geometry rules')
  }
  const rows=[]
  let representative
  for(const row of b.pairs){
    for(const role of ['accel','decel']){
      let runData, unavailable
      try {runData=await trace(root,row,role,!onlyGeometry)} catch(error){unavailable=error.message}
      for(const band of bands){
        const r={slot:row.planned_pair_id,run:row[role+'_run_id'],role,condition:row.condition,
          acc:number(row,'nominal_A_pulse_s2'),speed:number(row,'nominal_v_pulse_s'),band:band.name,valid:false,reasons:[]}
        if(unavailable) r.reasons.push(unavailable)
        else {
          let g
          try {g=geometry(runData.data,runData.spans,band)} catch(error){r.reasons.push(error.message)}
          if(g){
            r.geometry=g
            r.reasons.push(...g.reasons)
            if(!onlyGeometry && !r.reasons.length) {
              r.result=integrateDifference(runData.data,g,model,b.alpha)
              if(band.name==='main' && row.condition==='X500V16' && role==='accel' && !representative){
                representative={data:runData.data,geometry:g,run:r.run}
              }
            }
          }
        }
        r.valid=!r.reasons.length
        rows.push(r)
      }
    }
  }
  const pairs=pairResults(b.pairs,rows), counts=coverage(rows,pairs)
  if(onlyGeometry){
    writeFileSync(prefix+'_geometry.csv',csv([geometryHeaders,...rows.map(r=>keyRow(r,r.geometry))]))
    const result={snapshot,coverage:counts}
    writeJson(prefix+'_check.json',result)
    console.log(JSON.stringify(result,null,2))
    return
  }
  const groupStats=groups(pairs,b.alpha)
  const sensitivityIndex=new Map(pairs.filter(r=>r.band==='sensitivity'&&r.valid).map(r=>[r.slot,r]))
  const pairedBands=pairs.filter(r=>r.band==='main'&&r.valid&&middle(r.acc)&&sensitivityIndex.has(r.slot))
    .map(r=>({slot:r.slot,main:r.alpha,sensitivity:sensitivityIndex.get(r.slot).alpha}))
  const summary={snapshot,coverage:counts,groups:groupStats,
    commonPrimaryPairs:meanCI(pairedBands.map(r=>r.sensitivity-r.main)),representative:representative?.run??null}
  writeJson(prefix+'_summary.json',summary)
  writeFileSync(prefix+'_trajectories.csv',csv([[...geometryHeaders,...integrals],
    ...rows.map(r=>[...keyRow(r,r.geometry),...(r.result?Object.values(r.result):Array(integrals.length).fill(null))])]))
  const pairColumns=['slot','condition','acc','speed','band','valid','reason','runA','runD','aAlpha','dAlpha',
    'bPairAlpha','alpha','residualMa','equivalentMa']
  writeFileSync(prefix+'_pairs.csv',csv([pairColumns,...pairs.map(r=>pairColumns.map(k=>k==='valid'?Number(r[k]):r[k]))]))
  writeFileSync(prefix+'_sensitivity.csv',csv([['planned_pair_id','main_alpha','sensitivity_alpha','sensitivity_minus_main'],
    ...pairedBands.map(r=>[r.slot,r.main,r.sensitivity,r.sensitivity-r.main])]))
  writeFileSync(prefix+'_groups.csv',csv([['band','group','total_pairs','valid_pairs','mean_alpha','ci_low','ci_high','mean_minus_b',
    'mean_residual_mA_s','rmse_mA_s','equivalent_error_mA'],...groupStats.map(r=>[r.band,r.group,r.total,
    r.coefficient.n,r.coefficient.mean,...(r.coefficient.ci95??[null,null]),r.differenceFromB.mean,r.residualMaS.mean,
    r.residualRmseMaS,r.equivalentErrorMa.mean])]))
  if(representative){
    const {data,geometry:g}=representative
    const points=g.profiles.flatMap((samples,phase)=>samples.map(s=>[
      s.t,s.v,interpolate(data,s.t,i=>data.current[i]-model.at(data.x[i],1000))*settings.rawToMa,
      phase===0?'acc':'dec',g.acc.begin,g.acc.end,g.dec.begin,g.dec.end]))
    writeFileSync(prefix+'_representative.csv',csv([['card_ms','speed_pulse_s','corrected_mA','phase','acc_begin_ms',
      'acc_end_ms','dec_begin_ms','dec_end_ms'],...points]))
  }
  console.log(JSON.stringify({id,coverage:counts,primary:groupStats.filter(r=>r.group==='primary'),sensitivity:summary.commonPrimaryPairs},null,2))
}

if(process.argv[1] && import.meta.url===pathToFileURL(resolve(process.argv[1])).href){
  main().catch(error=>{console.error(error.message);process.exitCode=1})
}

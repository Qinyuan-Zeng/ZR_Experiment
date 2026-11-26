import assert from 'node:assert/strict'
import {geometry, integrateDifference} from './analyze_c.mjs'

const alpha=3.0306147541113107e-5
const positionModel={at(x){ return 110+6*x }}
const band={low:12000,high:24000}
function simulated(riseAcceleration,fallAcceleration) {
  const t=[], x=[], current=[]
  const peak=40000, rise=peak/riseAcceleration, fall=peak/fallAcceleration, coast=25
  const accEnd=rise, decBegin=rise+coast, decEnd=decBegin+fall
  const accDistance=peak*rise/2, coastDistance=peak*coast
  for(let n=0;n<=Math.ceil((decEnd+5)/0.05);n++){
    const ms=n*0.05
    let v,a,distance
    if(ms<accEnd){
      v=riseAcceleration*ms
      a=riseAcceleration
      distance=v*ms/2
    } else if(ms<decBegin){
      v=peak
      a=0
      distance=accDistance+peak*(ms-accEnd)
    } else {
      const since=Math.min(ms-decBegin,fall)
      v=Math.max(0,peak-fallAcceleration*since)
      a=ms<decEnd?-fallAcceleration:0
      distance=accDistance+coastDistance+(peak+v)*since/2
    }
    const position=12+distance/1e6
    t.push(ms)
    x.push(position)
    current.push(positionModel.at(position)+30+0.004*v+alpha*a*1000)
  }
  return { data:{t,x,current}, spans:[{begin:0,end:accEnd},{begin:decBegin,end:decEnd}] }
}

const symmetric=simulated(400,400)
const balanced=geometry(symmetric.data,symmetric.spans,band)
assert.deepEqual(balanced.reasons,[])
assert(Math.abs(balanced.denominator-24000)<1e-8)
const result=integrateDifference(symmetric.data,balanced,positionModel,alpha)
assert(Math.abs(result.coefficient-alpha)<1e-10)
assert(Math.abs(result.residual)<1e-7)
assert(Math.abs(result.rawDelta-result.corrDelta)>0.1,'position term should matter to this fixture')

const asymmetric=simulated(400,600)
const unbalanced=geometry(asymmetric.data,asymmetric.spans,band)
assert(unbalanced.reasons.includes('total dwell mismatch'))
assert(unbalanced.reasons.includes('velocity bin dwell mismatch'))
console.log('C symmetric inertia with position and speed terms, plus unequal dwell rejection PASS')

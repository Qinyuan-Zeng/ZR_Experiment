// Compare model sharing on identical spatial support. Inputs are development references.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
const out=fileURLToPath(new URL('../results/BC_position/',import.meta.url));
const analysis='Z-BC_20261124-192404_01';
const [header,...lines]=fs.readFileSync(path.join(out,analysis+'.references.csv'),'utf8').trim().split(/\r?\n/);
const keys=header.split(',');
const rows=lines.map(line=>Object.fromEntries(line.split(',').map((v,i)=>[keys[i],v])));
const avg=a=>a.reduce((s,v)=>s+v,0)/a.length;
const modes=['B','C2','C10','C40'];
const runs=modes.map(mode=>{
  const groups=new Map();
  for(const r of rows.filter(r=>r.phase==='B'?mode==='B':mode==='C'+r.speed)){
    if(!groups.has(r.group))groups.set(r.group,new Map());
    groups.get(r.group).set(Math.round(1000*r.position),+r.ip);
  }
  return [...groups.values()];
});
// Use actual shared nodes (0.5 mm spacing), leaving one node and one whole run out.
// At the held node adjacent training knots are equidistant. Evaluate a linear lookup.
const x=[...runs[0][0].keys()].filter(k=>runs.flat().every(r=>r.has(k))).sort((a,b)=>a-b);
const metrics=[];
for(const [m,mode] of modes.entries()){
  for(const strategy of ['separate','share_C','share_B_C']){
    const err=[];
    for(let fold=0;fold<runs[m].length;fold++){
      const ids=strategy==='separate'||(strategy==='share_C'&&m===0)?[m]:
        strategy==='share_C'?[1,2,3]:[0,1,2,3];
      function node(k){
        const v=ids.map(j=>avg(runs[j].filter((_,i)=>i!==fold%runs[j].length).map(r=>r.get(k))));
        if(strategy==='share_B_C')return (v[0]+avg(v.slice(1)))/2;
        return avg(v);
      }
      for(let i=1;i<x.length-1;i++){
        const w=(x[i]-x[i-1])/(x[i+1]-x[i-1]);
        err.push((1-w)*node(x[i-1])+w*node(x[i+1])-runs[m][fold].get(x[i]));
      }
    }
    const a=err.map(Math.abs).sort((a,b)=>a-b);
    metrics.push([mode,strategy,Math.sqrt(avg(err.map(v=>v*v))),a[Math.ceil(a.length*.95)-1]]);
  }
}
console.log('shared nodes',x.length,x[0],x.at(-1),metrics);
fs.writeFileSync(path.join(out,analysis+'.sharing_cv.csv'),['mode,strategy,RMSE_raw,P95_raw',...metrics.map(r=>r.join(','))].join('\n'));

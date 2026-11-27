"""Render auditable B0 figures from the exported B0 tables, with no device dependencies."""
import numpy as np
import pandas as pd
from reportlab.graphics import renderSVG
from reportlab.graphics.shapes import Drawing, Group, Line, Rect, String, PolyLine, Circle
from reportlab.lib import colors

from b0_sources import OUT

BLUE=colors.HexColor("#0072BD")
RED=colors.HexColor("#D95319")
DARK=colors.HexColor("#161616")
GRAY=colors.HexColor("#777777")
GRID=colors.HexColor("#E5E5E5")
LIGHT_BLUE=colors.HexColor("#D9ECF9")
FIGURES=OUT/"figures"


def text(d,x,y,s,size=13,anchor="start",color=DARK):
    d.add(String(x,y,str(s),fontName="Times-Roman",fontSize=size,textAnchor=anchor,fillColor=color))


def drawing(w,h,title):
    d=Drawing(w,h)
    d.add(Rect(0,0,w,h,fillColor=colors.white,strokeColor=None))
    text(d,w/2,h-26,title,19,"middle")
    return d


class Axis:
    def __init__(self,d,box,xlim,ylim,xticks,yticks,xlabel,ylabel):
        self.d=d
        self.x,self.y,self.w,self.h=box
        self.xlo,self.xhi=xlim
        self.ylo,self.yhi=ylim
        for v in xticks:
            xx=self.dx(v)
            d.add(Line(xx,self.y,xx,self.y+self.h,strokeColor=GRID,strokeWidth=.4))
            d.add(Line(xx,self.y,xx,self.y+4,strokeColor=DARK,strokeWidth=.65))
            text(d,xx,self.y-17,f"{v:g}",12,"middle")
        for v in yticks:
            yy=self.dy(v)
            d.add(Line(self.x,yy,self.x+self.w,yy,strokeColor=GRID,strokeWidth=.4))
            d.add(Line(self.x,yy,self.x+4,yy,strokeColor=DARK,strokeWidth=.65))
            text(d,self.x-7,yy-4,f"{v:g}",12,"end")
        d.add(Rect(self.x,self.y,self.w,self.h,fillColor=None,strokeColor=DARK,strokeWidth=.75))
        text(d,self.x+self.w/2,self.y-35,xlabel,14,"middle")
        g=Group()
        g.translate(self.x-49,self.y+self.h/2)
        g.rotate(90)
        text(g,0,0,ylabel,14,"middle")
        d.add(g)

    def dx(self,v):
        return self.x+(v-self.xlo)*self.w/(self.xhi-self.xlo)

    def dy(self,v):
        return self.y+(v-self.ylo)*self.h/(self.yhi-self.ylo)

    def trace(self,x,y,color,dash=None,size=1.15):
        x,y=np.asarray(x),np.asarray(y)
        ok=np.isfinite(x)&np.isfinite(y)&(x>=self.xlo)&(x<=self.xhi)
        ids=np.flatnonzero(ok)
        groups=np.split(ids,np.flatnonzero(np.diff(ids)!=1)+1)
        for idx in groups:
            if len(idx)>1:
                xy=np.column_stack((self.dx(x[idx]),self.dy(y[idx]))).ravel().tolist()
                self.d.add(PolyLine(xy,strokeColor=color,strokeWidth=size,strokeDashArray=dash))


def legend(d,x,y,entries):
    for label,col,dash in entries:
        d.add(Line(x,y+4,x+25,y+4,strokeColor=col,strokeWidth=1.2,strokeDashArray=dash))
        text(d,x+30,y,label,13)
        x+=38+7*len(label)


def save(d,name):
    FIGURES.mkdir(exist_ok=True)
    renderSVG.drawToFile(d,str(FIGURES/(name+".svg")))


def coverage_heatmap(gap):
    d=drawing(820,447,"Measured state coverage for B1")
    speeds=[2,5,10,15,20,30,40]
    accs=[400,500,600]
    x0,y0,cw,ch=89,105,91,34
    maximum=max(1,gap.loc[gap["stage"].eq("B1"),"partial_joint_state_runs"].max())
    for j,path in enumerate(("P1","P2")):
        left=x0+j*370
        sub=gap[(gap["stage"]=="B1")&(gap["path"]==path)].set_index(["target_speed_mm_s","target_accel_mm_s2"])
        text(d,left+cw*1.5,374, "P1  10–22 mm" if j==0 else "P2  12–20 mm",15,"middle")
        for iy,v in enumerate(speeds):
            yy=y0+iy*ch
            text(d,left-9,yy+10,v,13,"end")
            for ix,a in enumerate(accs):
                xx=left+ix*cw
                row=sub.loc[(v,a)] if (v,a) in sub.index else None
                n=int(row["partial_joint_state_runs"]) if row is not None else -1
                ratio=(n/maximum)**.55 if n>=0 else 0
                color=colors.Color(1-.57*ratio,1-.31*ratio,1-.13*ratio) if n>=0 else colors.HexColor("#EEEEEE")
                d.add(Rect(xx,yy,cw,ch,strokeColor=GRID,strokeWidth=.5,fillColor=color))
                text(d,xx+cw/2,yy+11,"–" if n<0 else n,13,"middle")
        for ix,a in enumerate(accs):
            text(d,left+(ix+.5)*cw,y0-18,a,13,"middle")
        text(d,left+1.5*cw,y0-41,"Target acceleration (mm/s²)",14,"middle")
        g=Group();g.translate(left-42,y0+ch*3.5);g.rotate(90)
        text(g,0,0,"Target speed (mm/s)",14,"middle")
        d.add(g)
    text(d,410,36,"Cell: recorded legs that reach the joint speed, acceleration and position bands.",13,"middle")
    text(d,410,18,"Grey: not planned. State overlap is distinct from full-pair replacement.",13,"middle")
    save(d,"01_B1_state_coverage")


def nice(values):
    finite=np.asarray(values)
    finite=finite[np.isfinite(finite)]
    lo,hi=np.min(finite),np.max(finite)
    span=max(.001,hi-lo)
    base=10**np.floor(np.log10(span/4))
    step=next(scale*base for scale in (1,2,2.5,5,10) if scale*base>=span/4)
    ticks=np.arange(np.floor(lo/step),np.ceil(hi/step)+.1)*step
    return (ticks[0],ticks[-1]),ticks


def trace_figure(inventory):
    selection=inventory[(inventory["cohort"]=="S2_B_expanded")&inventory["raw_quality"].fillna(False)]
    candidate=selection.sort_values(["usable_causal_dynamic","run_id"],ascending=[False,True]).iloc[0]
    names=["run_id","t_native_s","I_actual_raw","ref_I_mech_raw","causal_I_mech_raw",
           "a_ref_pulse_s2","a_causal_pulse_s2"]
    stream=pd.read_csv(OUT/"mechanism_samples.csv.gz",usecols=names,chunksize=100_000)
    rows=pd.concat([v for chunk in stream if not (v:=chunk[chunk["run_id"].eq(candidate["run_id"])]).empty])
    rows=rows.sort_values("t_native_s")
    t=rows["t_native_s"].to_numpy()
    d=drawing(820,575,"Current and reconstructed acceleration during one Watch run")
    xr,xt=nice([0,float(t.max())])
    current=["I_actual_raw","ref_I_mech_raw","causal_I_mech_raw"]
    y1,t1=nice(rows[current].to_numpy().ravel())
    y2,t2=nice((rows[["a_ref_pulse_s2","a_causal_pulse_s2"]]/1000).to_numpy().ravel())
    first=Axis(d,(89,332,700,153),xr,y1,xt,t1,"Time from first raw sample (s)","Current (raw)")
    second=Axis(d,(89,91,700,161),xr,y2,xt,t2,"Time from first raw sample (s)","Acceleration (mm/s²)")
    for name,c,dash in zip(current,[DARK,BLUE,RED],[None,None,[4,3]]):
        first.trace(t,rows[name],c,dash)
    second.trace(t,rows["a_ref_pulse_s2"]/1000,BLUE)
    second.trace(t,rows["a_causal_pulse_s2"]/1000,RED,[4,3])
    legend(d,96,505,[("Measured current",DARK,None),("Offline M1",BLUE,None),("Causal M1",RED,[4,3])])
    legend(d,96,275,[("Offline acceleration",BLUE,None),("Causal acceleration",RED,[4,3])])
    text(d,410,22,str(candidate["trial_id"]),12,"middle")
    save(d,"02_watch_time_trace")
    return str(candidate["trial_id"])


def lag_figure(lags):
    picked=lags[lags["cohort"].isin(["S2_B_expanded","S3_A_final","S1_C"])]
    if picked.empty:
        return
    pivot=picked.pivot(index=["cohort","run_id"],columns="shift_ms",values="centered_rmse_raw").dropna()
    excess=pivot.sub(pivot[0],axis=0)
    average=excess.groupby(level=0).median()
    yr,yt=nice(average.to_numpy().ravel())
    d=drawing(820,365,"Sensitivity to shifting the offline acceleration")
    panel=Axis(d,(89,77,704,205),(-20,20),yr,[-20,-10,0,10,20],yt,
               "Acceleration shift relative to current (ms)","Change in centered RMSE (raw)")
    items=[]
    for name,c,dash in (("S2_B_expanded",BLUE,None),("S3_A_final",RED,[4,3]),("S1_C",GRAY,[2,3])):
        if name in average.index:
            a=average.loc[name]
            panel.trace(a.index.astype(float),a,c,dash)
            for x,y in a.items():
                d.add(Circle(panel.dx(x),panel.dy(y),2,fillColor=colors.white,strokeColor=c,strokeWidth=.9))
            items.append(({"S2_B_expanded":"Stage 2 B Watch","S3_A_final":"Stage 3 A host","S1_C":"Stage 1 C host"}[name],c,dash))
    legend(d,94,304,items)
    text(d,410,22,"Same time points for all shifts. Positive shifts use later acceleration samples.",13,"middle")
    save(d,"03_latency_sensitivity")


def phase_errors(metrics):
    use=metrics[(metrics["scope"]=="phase")&metrics["cohort"].isin(["S2_B_expanded","S3_A_final"])
                &metrics["dataset_role"].eq("train_candidate")]
    use=use[use["metric"].isin(["ref_r_M0_common_raw","ref_r_M1_common_raw"])]
    if use.empty: return
    maximum=max(use["run_balanced_rmse_raw"].max(),1)
    yr,yt=nice([0,maximum*1.06])
    pnames=["acceleration","steady","deceleration","hold"]
    d=drawing(820,407,"Mechanism current errors on common support")
    for j,(cohort,title) in enumerate((("S2_B_expanded","Stage 2 B Watch"),("S3_A_final","Stage 3 A host"))):
        panel=Axis(d,(75+j*398,91,328,224),(.55,4.45),yr,[],yt,"","Run-balanced RMSE (raw)")
        text(d,239+j*398,330,title,15,"middle")
        for i,phase in enumerate(pnames,1):
            text(d,panel.dx(i),70,phase.capitalize(),11,"middle")
            for delta,label,c in ((-.16,"ref_r_M0_common_raw",BLUE),(.16,"ref_r_M1_common_raw",RED)):
                row=use[(use["cohort"]==cohort)&(use["phase"]==phase)&(use["metric"]==label)]
                if len(row):
                    height=panel.dy(row.iloc[0]["run_balanced_rmse_raw"])-panel.dy(0)
                    d.add(Rect(panel.dx(i+delta-.115),panel.dy(0),panel.w*.23/3.90,height,
                               fillColor=c,strokeColor=None))
    legend(d,314,355,[("M0",BLUE,None),("M1",RED,None)])
    text(d,410,26,"Training candidates only. Each leg has equal weight; blanks indicate no comparable support.",13,"middle")
    save(d,"04_common_support_errors")


def main():
    inventory=pd.read_csv(OUT/"run_inventory.csv")
    gap=pd.read_csv(OUT/"gap_matrix.csv")
    coverage_heatmap(gap)
    trace_figure(inventory)
    lag_figure(pd.read_csv(OUT/"lag_sensitivity.csv"))
    phase_errors(pd.read_csv(OUT/"mechanism_residuals.csv"))


if __name__=="__main__":
    main()

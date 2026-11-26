from pathlib import Path
import csv
from reportlab.pdfgen import canvas
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.lib.colors import HexColor
root=Path.cwd()
stem='Z-D_20261124-203450_02'
def rows(s):
    return list(csv.DictReader((root/'results'/'D'/stem/(stem+'.'+s+'.csv')).open(encoding='utf-8-sig')))
base=root/'data/figures'/'D'/stem/(stem+'.static_validation')
base.parent.mkdir(parents=True,exist_ok=True)
pdfmetrics.registerFont(TTFont('Song','C:/Windows/Fonts/simsun.ttc',subfontIndex=0))
c=canvas.Canvas(str(base)+'.pdf',pagesize=(730,500))
def label(x,y,s,size=11,center=False):
    c.setFillColor(HexColor('#161616'));c.setFont('Song',size)
    (c.drawCentredString if center else c.drawString)(x,y,s)
colors=['#0072BD','#D95319']
def axis(l,b,w,h,xmin,xmax,ymin,ymax,xt,yt):
    X=lambda v:l+(v-xmin)*w/(xmax-xmin)
    Y=lambda v:b+(v-ymin)*h/(ymax-ymin)
    for x in xt:
        c.setStrokeColor(HexColor('#dddddd'));c.line(X(x),b,X(x),b+h);label(X(x),b-14,str(x),10,True)
    for y in yt:
        c.setStrokeColor(HexColor('#bbbbbb' if y==0 else '#dddddd'));c.line(l,Y(y),l+w,Y(y));label(l-19,Y(y)-3,str(y),10,True)
    c.setStrokeColor(HexColor('#222222'));c.rect(l,b,w,h)
    return X,Y
def marker(x,y,i):
    c.setStrokeColor(HexColor(colors[i]))
    if i==0:c.circle(x,y,2.7,stroke=1)
    else:c.rect(x-2.5,y-2.5,5,5)
label(365,470,'静态位置模型的独立检验',19,True)
label(65,438,'双向中心误差（raw）',13)
for i,t in enumerate(['未见位置','另时段复测']):
    marker(475+105*i,443,i);label(486+105*i,439,t)
X,Y=axis(65,288,635,137,0,32,-20,20,[0,4,8,12,16,20,24,28,32],[-20,-10,0,10,20])
for r in rows('center_predictions'):
    if r['stage'] in ['D1','D2']:marker(X(float(r['position_pulse'])/1000),Y(float(r['error_B_raw'])),int(r['stage']=='D2'))
label(383,255,'位置（mm）',12,True)
data=rows('direction_metrics')
for j,stage in enumerate(['D1','D2']):
    l=65+j*350;label(l,222,['未见位置','另时段复测'][j]+' · 单方向平均残差（raw）',12)
    X,Y=axis(l,64,280,140,.8,4.2,-60,60,[1,2,3,4],[-60,-30,0,30,60])
    for i,d in enumerate(['1','-1']):
        a=sorted((int(r['window'][-1]),float(r['mean_error_raw'])) for r in data if r['stage']==stage and r['model']=='B' and r['scope']=='all' and r['direction']==d)
        for (s,v),(t,w) in zip(a,a[1:]):
            c.setStrokeColor(HexColor(colors[i]));c.line(X(s),Y(v),X(t),Y(w))
        for s,v in a:marker(X(s),Y(v),i)
    label(l+140,32,'保持窗口（第几秒）',12,True)
label(365,9,'下图蓝色圆点为正向到位，橙色方点为反向到位。残差取实测减预测。',10,True)
c.save()

from pathlib import Path
from io import BytesIO
from PIL import Image, ImageDraw, ImageFont
import numpy as np
from docx import Document
from docx.oxml.ns import qn
from docx.shared import Cm

root = Path.cwd()
base = root / '分项实验/01_位置相关空载电流辨识'
out = root / 'tmp/position_report_qa'
out.mkdir(exist_ok=True)
fonts = Path('C:/Windows/Fonts')
def font(n, chinese=True):
    return ImageFont.truetype(str(fonts / ('simsun.ttc' if chinese else 'times.ttf')), n)

source = next((base/'data/figures/BC_position').glob('*07-A1_short_hold.png'))
hold = Image.open(source).convert('RGB')
# The original upper axis spine is below row 56. Limit the mask to the title.
ImageDraw.Draw(hold).rectangle((1200, 0, 1730, 54), fill='white')
fixed = Image.new('RGB', (hold.width, hold.height + 24), 'white')
fixed.paste(hold, (0,24))
ImageDraw.Draw(fixed).text((hold.width/2,30),'三个代表位置的保持电流',font=font(49),fill='black',anchor='mm')
fixed.save(out/'figure1.png',dpi=(400,400))

nodes = np.loadtxt(next((base/'results/BC_position').glob('*B_nodes.csv')),delimiter=',',skiprows=1)
x,y=nodes.T
q=np.linspace(x[0],x[-1],2401)
s=np.diff(y)/np.diff(x)
t=np.zeros(len(x))
for i in range(1,len(x)-1):
    if s[i-1]*s[i]>0:
        t[i]=2/(1/s[i-1]+1/s[i])
for idx,a,b in [(0,s[0],s[1]),(-1,s[-1],s[-2])]:
    value=(3*a-b)/2
    t[idx]=0 if value*a<=0 else np.sign(value)*min(abs(value),3*abs(a))
i=np.clip(np.searchsorted(x,q,side='right')-1,0,len(x)-2)
width=x[i+1]-x[i]
u=(q-x[i])/width
cubic=(2*u**3-3*u**2+1)*y[i]+(u**3-2*u**2+u)*width*t[i]+(-2*u**3+3*u**2)*y[i+1]+(u**3-u**2)*width*t[i+1]
line=np.polyfit(x,y,1)
linear=np.polyval(line,q)
quad=np.polyval(np.polyfit(x,y,2),q)
piecewise=np.interp(q,x,y)
im=Image.new('RGB',(2000,1240),'white')
d=ImageDraw.Draw(im)
blue='#0072BD'; orange='#D95319'; purple='#7951A1'; green='#008568'; dark='#292929'
cn=font(38); tick=font(35,False)
def label(at,text,anchor='mm',color='black',f=cn):
    d.text(at,text,font=f,fill=color,anchor=anchor)
def panel(top,bottom,ymin,ymax,y_ticks,ylabel):
    left,right=202,1950
    def coord(xx,yy):
        return (left+(xx-1)/30*(right-left),bottom-(yy-ymin)/(ymax-ymin)*(bottom-top))
    for v in [1,5,10,15,20,25,31]:
        px,_=coord(v,0)
        d.line((px,top,px,bottom),fill='#e5e5e5',width=2)
        d.line((px,bottom,px,bottom-12),fill=dark,width=3)
        if bottom>900: label((px,bottom+25),str(v),f=tick)
    for v in y_ticks:
        _,py=coord(1,v)
        d.line((left,py,right,py),fill='#e5e5e5',width=2)
        d.line((left,py,left+12,py),fill=dark,width=3)
        label((left-14,py),str(v),anchor='rm',f=tick)
    d.rectangle((left,top,right,bottom),outline=dark,width=3)
    tile=Image.new('RGB',(670,55),'white')
    ImageDraw.Draw(tile).text((335,27),ylabel,fill='black',font=cn,anchor='mm')
    rot=tile.rotate(90,expand=True)
    im.paste(rot.crop((0,100,55,570)),(7,(top+bottom)//2-235))
    return coord
def curve(coord,xx,yy,color,dash=False):
    pts=[coord(a,b) for a,b in zip(xx,yy)]
    if dash:
        from math import hypot
        travel=0; run=[]
        for a,b in zip(pts,pts[1:]):
            travel+=hypot(b[0]-a[0],b[1]-a[1])
            if int(travel/16)%2==0: run.append(a)
            else:
                if len(run)>1: d.line(run,fill=color,width=5)
                run=[]
        if len(run)>1: d.line(run,fill=color,width=5)
    else: d.line(pts,fill=color,width=5)
def dots(coord,xx,yy):
    for a,b in zip(xx,yy):
        px,py=coord(a,b)
        d.ellipse((px-3,py-3,px+3,py+3),fill=dark)

label((1065,38),'静态保持实测与候选模型',f=font(49))
top=panel(95,545,100,900,[100,300,500,700,900],'电流（raw）')
curve(top,q,np.full(len(q),y.mean()),'#808080',True)
curve(top,q,linear,orange,True)
curve(top,q,quad,purple)
curve(top,q,piecewise,blue)
curve(top,q,cubic,green,True)
dots(top,x[::5],y[::5])
for j,(name,color) in enumerate([('实测点',dark),('常值', '#808080'),('线性',orange),('二次',purple),('分段线性',blue),('PCHIP',green)]):
    left=265+(j%3)*225; cy=145+(j//3)*49
    if j==0: d.ellipse((left+17,cy-5,left+27,cy+5),fill=color)
    elif j in (1,2,5):
        for offset in (0,18,36): d.line((left+offset,cy,left+offset+11,cy),fill=color,width=5)
    else: d.line((left,cy,left+48,cy),fill=color,width=5)
    label((left+59,cy),name,anchor='lm',f=font(33,name!='PCHIP'))
label((1065,612),'相对全程线性拟合的偏差')
bottom=panel(665,1120,-30,30,[-30,-15,0,15,30],'相对线性的偏差（raw）')
curve(bottom,q,np.zeros(len(q)),orange,True)
curve(bottom,q,quad-linear,purple)
curve(bottom,q,piecewise-linear,blue)
curve(bottom,q,cubic-linear,green,True)
dots(bottom,x[::5],(y-np.polyval(line,x))[::5])
label((1065,1200),'位置（mm）')
im.save(out/'figure3.png',dpi=(400,400))
print('max linear-quadratic difference',np.max(np.abs(linear-quad)))
print('max piecewise-PCHIP difference',np.max(np.abs(piecewise-cubic)))

file=root/'实验进程/实验文档记录/ZR实验_第一阶段.docx'
doc=Document(file)
def replace_picture(shape, path):
    rid,_=doc.part.get_or_add_image(str(path))
    shape._inline.graphic.graphicData.pic.blipFill.blip.set(qn('r:embed'),rid)
    with Image.open(path) as picture:
        shape.height=round(shape.width*picture.height/picture.width)
replace_picture(doc.inline_shapes[0],out/'figure1.png')
replace_picture(doc.inline_shapes[2],out/'figure3.png')
caption=next(p for p in doc.paragraphs if p.text.startswith('图 3'))
caption.runs[0].text='图 3  候选模型及相对全程线性拟合的偏差'
paragraph=next(p for p in doc.paragraphs if p.text.startswith('以一整轮'))
paragraph.insert_paragraph_before('图 3 上半部分按全电流范围绘制。全程线性与二次模型贴近，分段线性和 PCHIP 都通过实测节点。下半部分以全程线性预测为零线，放大局部偏差。为便于辨认，图中每隔 0.5 mm 显示一个实测标记。',style='Normal')
buffer=BytesIO()
doc.save(buffer)
file.write_bytes(buffer.getvalue())


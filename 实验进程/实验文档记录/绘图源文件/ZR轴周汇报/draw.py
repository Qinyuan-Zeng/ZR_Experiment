"""ZR轴周汇报插图。曲线来自候选工况、归一化示例或目标公式，无实测数据。"""
from pathlib import Path
from math import sin, pi, copysign, exp
from PIL import Image, ImageDraw, ImageFont

OUT = Path(__file__).parent
W, H = 1440, 560
BLACK, BLUE, RED, GREY = '#202020', '#0072BD', '#D95319', '#E5E5E5'

def label(d, xy, text, size=29, color=BLACK, anchor='mm'):
    face = 'simsun.ttc' if any(ord(c)>127 for c in text) else 'times.ttf'
    d.text(xy, text, font=ImageFont.truetype('C:/Windows/Fonts/'+face, size), fill=color, anchor=anchor)

def canvas(title):
    im = Image.new('RGB', (W,H), 'white')
    d = ImageDraw.Draw(im)
    label(d, (W//2,35), title, 38)
    return im,d

def arrow(d,a,b,color=BLACK):
    x,y=b; dx=x-a[0]; dy=y-a[1]; norm=(dx*dx+dy*dy)**.5
    ux,uy=dx/norm,dy/norm
    d.line([a,b],color,4)
    d.polygon([b,(x-16*ux+7*uy,y-16*uy-7*ux),(x-16*ux-7*uy,y-16*uy+7*ux)],fill=color)

def flow(name,title,steps,notes,footer):
    im,d=canvas(title)
    xs=[190,540,890,1240]
    for i,x in enumerate(xs):
        d.rectangle((x-149,155,x+149,304),outline=BLACK,width=3)
        for j,line in enumerate(steps[i].split('\n')):
            label(d,(x,209+j*44),line,31)
        label(d,(x,344),notes[i],26)
        if i<3: arrow(d,(x+150,229),(xs[i+1]-151,229))
    label(d,(720,454),footer,30)
    im.crop((0,0,W,508)).save(OUT/(name+'.png'),dpi=(400,400))

def dash(d,points,c,width=4):
    offset=0.
    for (x,y),(u,v) in zip(points,points[1:]):
        dist=((u-x)**2+(v-y)**2)**.5
        t=0.
        while t<dist:
            phase=offset%23
            step=min(dist-t, (13 if phase<13 else 23)-phase)
            if phase<13:
                d.line([(x+(u-x)*t/dist,y+(v-y)*t/dist),(x+(u-x)*(t+step)/dist,y+(v-y)*(t+step)/dist)],c,width)
            t+=step; offset+=step

def plot(name,title,xaxis,yaxis,xt,yt,series,legend_y=94,markers=(),separators=()):
    im,d=canvas(title)
    l,t,r,b=154,91,1379,447
    f=lambda x,y:(l+(r-l)*(x-xt[0])/(xt[-1]-xt[0]), b-(b-t)*(y-yt[0])/(yt[-1]-yt[0]))
    for x in xt:
        u,_=f(x,0)
        d.line((u,t,u,b),GREY,2)
        d.line((u,b,u,b-8),BLACK,2)
        label(d,(u,b+23),f'{x:g}',26)
    for y in yt:
        _,v=f(0,y)
        d.line((l,v,r,v),GREY,2)
        d.line((l,v,l+8,v),BLACK,2)
        label(d,(l-12,v),f'{y:g}',26,anchor='rm')
    d.rectangle((l,t,r,b),outline=BLACK,width=2)
    for x,txt in separators:
        u,_=f(x,0); dash(d,[(u,t+50),(u,b)],'#787878',2)
        label(d,(u+12,t+74),txt,28,anchor='lm')
    for i,(text,xy,col,broken) in enumerate(series):
        coords=[f(x,y) for x,y in xy]
        if broken: dash(d,coords,col)
        else: d.line(coords,col,4)
        u=204+365*i; v=t+legend_y-54
        if broken: dash(d,[(u,v),(u+44,v)],col)
        else: d.line((u,v,u+44,v),col,4)
        label(d,(u+55,v),text,28,anchor='lm')
    for x,y,txt in markers:
        u,v=f(x,y); d.ellipse((u-6,v-6,u+6,v+6),fill=BLACK)
        label(d,(u+12,v-17),txt,28,anchor='lm')
    label(d,(767,519),xaxis,31)
    strip=Image.new('RGB',(390,53),'white'); label(ImageDraw.Draw(strip),(195,25),yaxis,31)
    im.paste(strip.rotate(90,expand=True),(13,75))
    im.save(OUT/(name+'.png'),dpi=(400,400))

def sample(lo,hi,fn,n=201):
    return [(x:=lo+(hi-lo)*k/(n-1),fn(x)) for k in range(n)]

if __name__=='__main__':
    flow('01','位置相关电流提取',['正向到位\n取保持均值','反向到位\n取保持均值','同位置平均\n得到候选位置项','双向匀速扫描\n独立核对'],['保留同向重复','与正向交替采集','另存双向半差','按实测位置对齐'],'共同条件：R 轴固定，Z 轴空载，夹具及线缆布置一致')
    def staircase(values):
        return [(k+d,y) for k,y in enumerate(values) for d in (0,1)]
    nodes=list(range(5,36,2))
    plot('02','双向逐点到位与保持','测量顺序（到位节点）','目标位置（mm）',list(range(0,17,2)),[0,5,10,15,20,25,30,35,40],
         [('正向到位',staircase(nodes),BLUE,False),
          ('反向到位',staircase(nodes[::-1]),RED,True)],legend_y=83)
    flow('03','匹配差分辨识惯性系数',['加速试次 A\n减速试次 B','同方向匹配\n实际位置与速度','分别扣除位置项\n再求电流差','电流差对\n加速度差回归'],['独立执行与停车','核对加速度符号','保留配对误差','输出斜率与区间'],'积分交叉验证：用实际速度变化量核对差分回归结果')
    plot('04','加速与减速轨迹的匹配状态','位置（mm）','速度（mm/s）',[5,10,15,20,25,30,35],[0,10,20,30,40,50,60,70],
         [('加速轨迹局部',sample(19.2,22.4,lambda x:(900+1000*(x-20))**.5),BLUE,False),
          ('减速轨迹局部',sample(17.6,20.8,lambda x:(900-1000*(x-20))**.5),RED,True)],markers=[(20,30,'匹配点（20，30）')])
    flow('05','基础摩擦与剩余电流补偿',['反馈电流扣除\n位置项和惯性项','用双向匀速数据\n拟合基础摩擦','用启停与保持数据\n学习剩余电流','用完整新轨迹\n比较逐项增益'],['保留实际运动状态','检查方向和低速','连续历史特征','按试次划分数据'],'保持时基础滑动摩擦为零，剩余电流以同一位置基准计算')
    plot('06','连续摩擦模型与零速过渡','归一化速度 v/v_ref','归一化摩擦电流',[-1,-.5,0,.5,1],[-1,-.5,0,.5,1,1.5],
         [('连续基础摩擦',sample(-1,1,lambda v:.55*(2/(1+exp(-32*v))-1)+.32*v),BLUE,False)],
          markers=[(0,0,'零速锚点')])
    flow('07','试次间电流轨迹更新',['执行本次\n整数 DAC 点表','电流扣除空载基准\n计算估计力','用估计力误差\n求约束参数修正','校验点表后\n进入下一次试验'],['记录实际下发时刻','按实际状态回算','加载与保持共同评价','保存修正量与异常'],'力传感器独立记录真实力，用于评价及真实力更新对照')
    def target(t):
        s=min(t/.4,1)
        return .2+2.8*(10*s**3-15*s**4+6*s**5)
    plot('08','名义工况的加载与保持目标','加载起始后时间（s）','目标接触力（N）',[0,.4,.8,1.2,1.6,2,2.4],[0,1,2,3,4],
         [('目标力',sample(0,2.4,target),RED,True)],separators=[(.4,'保持开始')])



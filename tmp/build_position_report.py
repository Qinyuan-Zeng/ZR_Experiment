from pathlib import Path
from PIL import Image, ImageDraw
from docx import Document
from docx.oxml.ns import qn
from docx.shared import Cm,Pt,RGBColor
from docx.enum.text import WD_ALIGN_PARAGRAPH as A
r=Path.cwd()
f=r/'分项实验/01_位置相关空载电流辨识/data/figures/BC_position'
d=Document()
s=d.sections[0]
s.page_width,s.page_height=Cm(21),Cm(29.7)
s.left_margin=s.right_margin=Cm(3.175)
s.top_margin=s.bottom_margin=Cm(2.4)
for n,c,z in [('Normal','宋体',12),('Title','黑体',22),('Heading 1','黑体',18),('Heading 2','黑体',15),('Heading 3','黑体',14),('Caption','宋体',10.5)]:
 a=d.styles[n];a.font.name='Times New Roman';a.font.size=Pt(z);a.font.color.rgb=RGBColor(0,0,0);a.font.italic=False;a.font.bold=n in ['Title','Heading 1','Heading 2','Heading 3']
 a.element.get_or_add_rPr().rFonts.set(qn('w:eastAsia'),c)
 for attr in ['asciiTheme','hAnsiTheme','eastAsiaTheme','cstheme']:
  a.element.rPr.rFonts.attrib.pop(qn('w:'+attr),None)
 pp=a.element.get_or_add_pPr()
 for border in pp.findall(qn('w:pBdr')): pp.remove(border)
 b=a.paragraph_format;b.line_spacing=1.2;b.space_before=Pt(0);b.space_after=Pt(6);b.first_line_indent=Pt(24 if n=='Normal' else 0)
def p(t): d.add_paragraph(t)
def h(t): d.add_heading(t,1)
def fig(key,t,w):
 a=d.add_paragraph();a.alignment=A.CENTER;a.paragraph_format.first_line_indent=Pt(0);a.paragraph_format.keep_with_next=True;a.paragraph_format.space_after=Pt(2);a.paragraph_format.line_spacing=1
 source=next(f.glob('*.'+key+'.png'))
 if key.startswith('07-'):
  image=Image.open(source).convert('RGB')
  iw,ih=image.size
  ImageDraw.Draw(image).rectangle((int(iw*.39),0,int(iw*.61),int(ih*.044)),fill='white')
  source=r/'tmp/position_hold_figure.png'
  image.save(source)
 a.add_run().add_picture(str(source),width=Cm(w))
 d.add_paragraph(t,'Caption').alignment=A.CENTER
d.add_paragraph('ZR实验_第一阶段','Title').alignment=A.CENTER
p('在 Z 轴位置伺服、R 轴固定、空载无接触条件下，测量反馈电流随位置的变化。实验工作区为 0～32 mm，位置当量为 1 μm/脉冲，电流以驱动器反馈原始单位 raw 记录。用双向静态保持数据建立位置项模型，并与恒速扫描结果对照。')
h('一 位置项建模')
d.add_heading('1.1 辨识原理与保持时间',2)
p('在同一位置将正、反向电流取半和，估计两方向共有的位置项。半差描述方向相关电流。恒速扫描中，若正反向滑动摩擦近似等幅反号，取半和可抵消其反号部分。静态保持中的方向指到位前的接近方向，静摩擦及伺服调整可能保留到位历史。')
a=d.add_paragraph();a.alignment=A.CENTER;a.paragraph_format.first_line_indent=Pt(0)
for t,sub in [('I',False),('p',True),('(x) = [I',False),('+',True),('(x) + I',False),('−',True),('(x)] / 2',False)]:
 q=a.add_run(t);q.font.subscript=sub;q.italic=True
p('先在全行程选点测量，再在 8、16、24 mm 各连续保持约 30 s。三个位置的电流均在第 3 秒接近后续稳定值。随后在各点从两侧各重复到位 5 次，第 3、4 秒窗口均值的最大差为 2.7 raw，第 4 秒正、反向均值仍相差约 60～61 raw。据此统一保持 4 s，取末 1 s 的电流均值。')
fig('07-A1_short_hold','图 1  三个代表位置的连续保持电流',14)
d.add_heading('1.2 静态保持与模型拟合',2).paragraph_format.page_break_before=True
p('静态保持实验在 1～31 mm 每 0.1 mm 测一点，共 301 个双向节点，完成两轮、1204 次到位。每次从目标一侧约 1 mm 处接近，速度 2 mm/s，加速度 20 mm/s²。逐轮对末 1 s 电流做双向平均。两轮曲线逐点差值的均方根为 4.62 raw，第二轮平均低 2.81 raw。')
fig('01-B_repeatability','图 2  两轮静态保持的双向平均电流',13)
fig('04-B_candidate_models','图 3  静态保持实测节点与候选模型',12)
p('以一整轮和训练轮中对应位置同时留出的方式比较候选模型。全程线性、二次、分段线性和 PCHIP 的 RMSE 分别为 9.62、9.51、4.54 和 4.61 raw。分段线性较全程直线降低约 53%，且保留局部变化。因此以两轮双向中心的均值作为节点值，在相邻节点间线性插值，形成 1～31 mm 的主表。')
d.add_heading('1.3 恒速扫描与静态结果对照',2).paragraph_format.page_break_before=True
p('恒速扫描在 0～32 mm 内以 2、10、40 mm/s 各扫描 5 个往返。按反馈位置与时间戳筛选恒速段，再在正反有效段的重叠区按实际位置对齐并取半和。比较采用每档速度前三个开发往返。')
fig('03-BC_position_curves','图 4  静态保持与三档恒速扫描的双向中心',11.5)
p('在静态保持与三档恒速扫描均覆盖的 3.5～22 mm 内，恒速与静态双向平均电流之差的均方根分别为 7.78、5.48、4.55 raw。扣除各速度的平均偏差后，差值仍随位置变化，均方根为 3.65～4.85 raw。同一区间的平均方向半差在静态时约为 39 raw，恒速时约为 100～107 raw。后续单方向估力需考虑这部分空载电流。')
fig('06-C_minus_B','图 5  恒速与静态中心差值及去均值后的变化',11.5)
p('保持和滑动统一按位置查询静态主表。后续将在新增位置、另一时段和每档速度预留的两个往返上独立检验。3.5～22 mm 内，以静态双向平均和三档恒速双向平均的均值各占 50% 构建对照表。')
d.add_paragraph('数据与图源  工作区“位置相关空载电流辨识”分项下的原始采集数据与绘图记录。','Caption')
d.core_properties.title='ZR实验_第一阶段'
d.save(r/'实验进程/实验文档记录/ZR实验_第一阶段.docx')





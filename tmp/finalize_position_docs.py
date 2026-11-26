from pathlib import Path
from docx import Document
from docx.shared import Cm,Pt
from docx.enum.text import WD_ALIGN_PARAGRAPH as Align
from io import BytesIO
from zipfile import ZipFile
root=Path.cwd()
base=root/'分项实验/01_位置相关空载电流辨识'
batch='Z-D_20261124-203450_02'
readme=base/'README.md'
md=readme.read_text(encoding='utf-8-sig')
summary="""## 最终模型与独立验证

采用[静态双向保持单表](models/position_B_primary_v1.json)作为最终位置项。每个位置先在同一轮取正反向末 1 s 电流的半和，再对两轮等权平均。模型覆盖 **1～31 mm（1000～31000 脉冲）**，包含 301 个节点，节点间线性插值，输出有符号电流 raw。0～32 mm 为实验指令区间，查询超过模型端点时留空。[模型节点](models/position_B_primary_v1.nodes.csv)

分段线性、PCHIP、二次和全程线性的开发集 RMSE 分别为 4.54、4.61、9.51、9.62 raw。分段线性保留局部变化，精度与 PCHIP 相当。对照方案在 3.5～22 mm 内对静态中心赋 50% 权重，对三档恒速中心各赋 1/6 权重。

| 独立检验 | 主表评价范围 | RMSE / P95（raw） |
| --- | --- | --- |
| D1 未见位置，51 对 | 1.55～30.55 mm，17 点 | 8.18 / 13.94 |
| D2 同日另时段，24 对 | 4～30 mm，8 点 | 7.97 / 13.93 |
| D3 留出恒速 2 mm/s，2 个往返 | 1～31 mm | 7.59 / 13.79 |
| D3 留出恒速 10 mm/s，2 个往返 | 1～31 mm | 6.59 / 12.54 |
| D3 留出恒速 40 mm/s，2 个往返 | 3.25～28.50 mm | 6.09 / 11.14 |

D1、D2 分别完成 102、48 次到位，缺测均为 0。30 mm 复测的最大绝对误差为 11.26 raw。共同区间内，静态单表与融合表的 RMSE 在 D1 为 7.95／8.72 raw，在 D2 为 8.03／8.09 raw。融合表在三档恒速留出往返上降低了重叠区误差。最终采用覆盖更宽的静态单表作为统一位置基准。保持第 4 秒，扣除位置项后，正向平均残差为 +22～+27 raw，反向为 −22～−23 raw，供后续摩擦与到位历史辨识使用。

[全域指标](results/D/XXX/XXX.full_metrics.csv) · [双表比较](results/D/XXX/XXX.overlap_metrics.csv) · [静态验证图](data/figures/D/XXX/XXX.static_validation.png)

""".replace('XXX',batch)
assert '## 最终模型与独立验证' not in md
md=md.replace('## 数据与结果目录',summary+'## 数据与结果目录',1).replace('100 脉冲主网格采集已启动。','100 脉冲主网格已完成两轮、1204 次到位，两轮双向中心逐点差值的均方根为 4.62 raw。').replace('## 下一步 C','## C 恒速扫描').replace('每档速度做 5 个完整往返，交错运行，共 30 趟。','在 2、10、40 mm/s 各完成 5 个往返，三档交错运行，共 30 趟。实测加减速度设定为 400 mm/s²。').replace('evaluate-d . D1会话 D2会话','evaluate-d . Z-D1_20261124-201558_01 Z-D2_20261124-202754_01')
file=root/'实验进程/实验文档记录/ZR实验_第一阶段.docx'
doc=Document(file)
def replace(prefix,new):
    p=next(p for p in doc.paragraphs if p.text.startswith(prefix))
    for i,r in enumerate(p.runs):
        r.text=new if i==0 else ''
replace('在静态保持与三档恒速','在共同覆盖的 3.5～22 mm 内，逐位置计算恒速与静态双向平均电流的差值。按 2、10、40 mm/s 的顺序，差值的均方根为 7.78、5.48、4.55 raw。图 5 下图从每条差值曲线中减去该曲线在此区间的平均值，得到去除整体偏移后的局部差异，均方根为 3.65～4.85 raw。')
replace('保持和滑动统一','同一位置的正、反向电流之差除以 2，即单方向电流相对双向平均的偏离量。在共同区间内，该量平均约为静态 39 raw、恒速 100～107 raw。仅扣除位置项时，这部分方向相关电流仍留在单方向残差中。')
source=next(p for p in doc.paragraphs if p.text.startswith('数据与图源'))
source.insert_paragraph_before('1.4 独立验证与最终模型',style='Heading 2').paragraph_format.page_break_before=True
source.insert_paragraph_before('在 17 个未参与建表的位置各完成三对正反向保持，另在同日另一时段对 8 个已知位置各复测三对。两次检验的双向平均电流预测均方根误差分别为 8.18、7.97 raw，绝对误差第 95 百分位均约为 14 raw。30 mm 复测的最大绝对误差为 11.26 raw。')
p=source.insert_paragraph_before()
p.alignment=Align.CENTER
p.paragraph_format.first_line_indent=Pt(0)
p.paragraph_format.keep_with_next=True
p.paragraph_format.line_spacing=1
p.add_run().add_picture(str(base/'data/figures/D'/batch/(batch+'.static_validation.png')),width=Cm(13.4))
source.insert_paragraph_before('图 6  静态独立检验误差与保持期间的单方向残差',style='Caption').alignment=Align.CENTER
source.insert_paragraph_before('图 6 上图使用保持末 1 s 的电流。下图中，单方向平均残差在第 3～4 秒已接近稳定，第 4 秒正向为 +22～+27 raw，反向为 −22～−23 raw。按 2、10、40 mm/s 的顺序，留出恒速往返的双向中心预测均方根误差为 7.59、6.59、6.09 raw，评价范围分别为 1～31、1～31、3.25～28.50 mm。')
source.insert_paragraph_before('最终采用两轮加密静态保持得到的 301 点分段线性单表，在 1～31 mm 内统一按位置查询。在 3.5～22 mm 的共同区间，两次静态检验的均方根误差分别为静态单表 7.95、8.03 raw，静动融合表 8.72、8.09 raw。融合表在三档留出恒速检验中的重叠区误差较小。静态单表覆盖约 30 mm 的工作位置，且在保持检验中表现相当或更好，因此选为统一位置基准。')
replace('数据与图源','数据与图源  工作区“位置相关空载电流辨识”分项的原始数据、冻结模型和独立验证结果。')
buf=BytesIO()
doc.save(buf)
assert ZipFile(buf).testzip() is None
file=file.with_name("ZR实验_第一阶段（位置项已验证）.docx")
file.write_bytes(buf.getvalue())
readme.write_text(md,encoding='utf-8')
assert len(Document(file).inline_shapes)==6



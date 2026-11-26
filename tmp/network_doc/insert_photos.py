from pathlib import Path
from copy import deepcopy
import sys
from docx import Document
from docx.shared import Cm, Pt
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.oxml import OxmlElement
from docx.text.paragraph import Paragraph

target = Path("E:/A_ZQY/Experiment/实验进程/ZR实验平台搭建及调试方法/实验平台硬件搭建与接线方法.docx")
doc = Document(target)
for number, image in [(5, sys.argv[1]), (6, sys.argv[2])]:
    caption = next(p for p in doc.paragraphs if p.text.startswith(f"图 {number} "))
    slot = caption._p.getprevious()
    assert slot.tag.endswith("tbl")
    slot.getparent().remove(slot)
    pic = caption.insert_paragraph_before()
    pic.alignment = WD_ALIGN_PARAGRAPH.CENTER
    pic.paragraph_format.keep_with_next = True
    pic.paragraph_format.line_spacing = 1
    pic.paragraph_format.space_after = Pt(2)
    if number == 5:
        pic.paragraph_format.page_break_before = True
    shape = pic.add_run().add_picture(image, width=Cm(16))
    if shape.height > Cm(13):
        factor = Cm(13) / shape.height
        shape.width = int(shape.width * factor)
        shape.height = Cm(13)
    caption.text = (
        "图 5  GSVD驱动器接线实物图"
        if number == 5 else "图 6  GSVD驱动器与电脑侧通信连接实物图"
    )
    caption.alignment = WD_ALIGN_PARAGRAPH.CENTER
    caption.paragraph_format.keep_with_next = False
for p in doc.paragraphs:
    if p.text.startswith("补拍时将驱动器"):
        p._p.getparent().remove(p._p)
doc.save("tmp/network_doc/photo-review.docx")

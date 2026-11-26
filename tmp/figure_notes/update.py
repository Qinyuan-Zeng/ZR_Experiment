from pathlib import Path
from docx import Document
from docx.oxml.ns import qn
from docx.shared import Cm
from PIL import Image
import sys

target=Path("E:/A_ZQY/Experiment/实验进程/ZR实验平台搭建及调试方法/实验平台硬件搭建与接线方法.docx")
doc=Document(target)
for rid,path in zip(["rId6","rId7","rId9","rId11"],sys.argv[1:]):
    new_rid,_=doc.part.get_or_add_image(path)
    hits=0
    for blip in doc.element.body.xpath(".//a:blip"):
        if blip.get(qn("r:embed"))==rid:
            blip.set(qn("r:embed"),new_rid)
            if rid in ("rId9","rId11"):
                inline=blip
                while inline.tag!=qn("wp:inline"):
                    inline=inline.getparent()
                for crop in inline.xpath(".//a:srcRect"):
                    crop.getparent().remove(crop)
                width,height=Image.open(path).size
                extent=inline.xpath(".//wp:extent")[0]
                if rid=="rId9":
                    cx=int(Cm(16))
                    cy=int(cx*height/width)
                else:
                    cy=int(extent.get("cy"))
                    cx=int(cy*width/height)
                extent.set("cx",str(cx))
                extent.set("cy",str(cy))
                for ext in inline.xpath(".//a:xfrm/a:ext"):
                    ext.set("cx",str(cx))
                    ext.set("cy",str(cy))
            hits+=1
    assert hits==1,(rid,hits)
doc.save("tmp/figure_notes/review.docx")

import sys,re,zipfile,difflib,xml.etree.ElementTree as ET
from pathlib import Path
from pypdf import PdfReader
sys.stdout.reconfigure(encoding='utf-8')
pdf=Path(r'E:\A_ZQY\Documents\xwechat_files\wxid_x71vlkesueyk12_b845\msg\file\2026-10\GVN闭环力控相关指令与例程说明251208.pdf')
doc=Path(r'E:\A_ZQY\Experiment\参考\GVN闭环力控相关指令与例程说明251208.docx')
with zipfile.ZipFile(doc) as z: root=ET.fromstring(z.read('word/document.xml'))
ns={'w':'http://schemas.openxmlformats.org/wordprocessingml/2006/main'}
a=''.join(''.join(p.itertext()) for p in [])
a=''.join(''.join(t.text or '' for t in p.findall('.//w:t',ns)) for p in root.findall('.//w:body//w:p',ns))
texts=[p.extract_text() or '' for p in PdfReader(pdf).pages]
b=''.join(texts)
norm=lambda s:re.sub(r'\s+','',s)
a,b=norm(a),norm(b)
matcher=difflib.SequenceMatcher(None,a,b,autojunk=False)
for op,i,j,k,l in matcher.get_opcodes():
 if op!='equal': print(op,'\nOLD:',a[max(0,i-45):j+45],'\nPDF:',b[max(0,k-45):l+45],'\n')
print('PAGE2\n',texts[1])
print('PAGE11\n',texts[10])
import pymupdf
r=pymupdf.open(pdf)
for i in (1,10):r[i].get_pixmap(matrix=pymupdf.Matrix(1.4,1.4)).save(str(Path(r'E:\A_ZQY\Experiment\OpenLoopApiReadTest\tests')/f'force_new_p{i+1}.png'))

from pathlib import Path
from pypdf import PdfReader
import zipfile, xml.etree.ElementTree as ET, difflib,re
pdf=Path(r'E:\A_ZQY\Documents\xwechat_files\wxid_x71vlkesueyk12_b845\msg\file\2026-10\GVN闭环力控相关指令与例程说明251208.pdf')
out=Path(r'E:\A_ZQY\Experiment\OpenLoopApiReadTest\tests')
r=PdfReader(pdf)
texts=[p.extract_text() or '' for p in r.pages]
(out/'new_force_pdf_text.txt').write_text('\n'.join(f'PAGE {i+1}\n{t}' for i,t in enumerate(texts)),encoding='utf-8')
print('PAGES',len(texts))
for i,t in enumerate(texts):
 print('\nPAGE',i+1,'\n',t)

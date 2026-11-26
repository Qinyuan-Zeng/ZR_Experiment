from pypdf import PdfReader
from pathlib import Path
p=Path(r'E:\A_ZQY\Experiment\产品参考手册\GVN编程手册.pdf')
r=PdfReader(p)
print('pages',len(r.pages))
for i,page in enumerate(r.pages):
    t=page.extract_text() or ''
    if 'SwitchControlLoopMode' in t or 'GetControlLoopMode' in t:
        print('\nPAGE',i+1,'\n',t)

import sys
from pypdf import PdfReader
sys.stdout.reconfigure(encoding='utf-8')
p=r'E:\A_ZQY\Documents\xwechat_files\wxid_x71vlkesueyk12_b845\msg\file\2026-10\GVN系列运动控制卡编程手册V0.24_202400812.pdf'
r=PdfReader(p)
print('pages',len(r.pages),flush=True)
for i,page in enumerate(r.pages):
 t=page.extract_text() or ''
 if '周期同步力矩模式' in t or 'GTN_SetPrfTorque' in t or 'GTN_GetControlLoopMode' in t:
  print('PAGE',i+1,'\n',t,flush=True)

from pathlib import Path
from docx import Document
from docx.shared import Cm
from docx.enum.text import WD_ALIGN_PARAGRAPH

src=Path('E:/A_ZQY/Experiment/实验进程/ZR实验平台搭建及调试方法/实验平台软件安装与开发环境配置方法.docx')
d=Document(src)
p=list(d.paragraphs)
for x in p:
    for r in x.runs:
        for n in range(6,0,-1):
            if f'图 {n}' in r.text:
                r.text=r.text.replace(f'图 {n}',f'图 {n+1}')
                break
p[4].text='MotionStudio部署'
p[5].text=('1．从固高技术支持处取得与控制卡配套的 MotionStudio 软件包，按电脑系统选用对应位数。'
           '本机使用 MotionStudio_x64_Develop_V2.5.4.0，用于控制卡识别、驱动安装和后续总线配置。')
p[6].insert_paragraph_before('2．将软件压缩包完整解压到固定目录，保留同目录的 DLL、resources、plugins 等文件夹。'
                            '在解压目录运行 MotionStudio.exe，并为它创建桌面快捷方式。若收到带安装向导的版本，则按随包说明安装。')
p[6].insert_paragraph_before('3．启动后确认主界面和“工具”菜单可打开。随后按第二节安装固高 PCIe 驱动，并检查在线控制卡。')
p[6].insert_paragraph_before('DriverStudio安装','Heading 2')
p[6].text=('1．从固高技术支持处取得与 GSVD 驱动器配套的 DriverStudio 安装包。'
           '本机桌面快捷方式显示版本为 2.8.17，现场使用时记录实际软件版本。')
p[7].insert_paragraph_before('2．首次安装时，按安装向导顺序先安装网卡驱动，再安装 DriverStudio。'
                            'GSVD 手册中的 Npcap 安装界面要求勾选“Install Npcap in WinPcap API-compatible Mode”。'
                            '不同版本以随包安装说明和界面为准。')
p[7].insert_paragraph_before('3．按向导选择空的安装目录并完成安装。已有旧版本时，按安装说明卸载后再安装。'
                            '从桌面图标启动 DriverStudio，确认主界面可打开。'
                            '驱动器接线并供电后，再选取对应有线网卡连接设备、核对型号。安装依据见 GSVD 手册 PDF 第 37–38 页。')
pic=p[7].insert_paragraph_before()
pic.alignment=WD_ALIGN_PARAGRAPH.CENTER
pic.paragraph_format.keep_with_next=True
pic.add_run().add_picture('C:/Users/HYB/AppData/Local/Temp/codex-clipboard-414ad770-4aea-4dd1-9ff0-20d8d88f7174.png',width=Cm(9))
c=p[7].insert_paragraph_before('图 1  MotionStudio与DriverStudio桌面快捷方式','Caption')
c.alignment=WD_ALIGN_PARAGRAPH.CENTER
h=p[7].insert_paragraph_before('Visual Studio组件安装','Heading 2')
h.paragraph_format.page_break_before=True
p[7].text=p[7].text.replace('3．使用','使用',1)
p[9].text='确认 MotionStudio、DriverStudio 和 Visual Studio 均能打开主界面。设备通信按第二节及后续联调步骤核对，VS 组件是否齐全以第三节的工程编译为准。'
d.save('tmp/software_install/review.docx')

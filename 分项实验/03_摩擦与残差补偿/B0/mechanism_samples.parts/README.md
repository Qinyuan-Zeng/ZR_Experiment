# B0 数据分卷

此目录按原始字节保存 mechanism_samples.csv.gz 的分卷，每份最多 20 MiB。
分卷保持原始压缩数据，无需分别解压。manifest.json 保存各分卷与完整文件的大小及 SHA-256。

下载完整仓库后，在 B0 目录执行下面的命令，即可合并并校验原文件。

```powershell
python restore_mechanism_samples.py
```

随后现有分析脚本继续读取 B0/mechanism_samples.csv.gz。
合并工具会校验全部分卷和完整文件，已有文件内容不匹配时会报错。

# GitHub 文件上传与推送经验

更新日期为 2026-11-27。适用于本工作区的源码、实验数据、分析结果及报告同步。本文依据本次实际操作整理，故障现象与已完成验证分别记录。

## 下次推送的起点

- 本地仓库为 E:/A_ZQY/ZR_Experiment，远程为 https://github.com/Qinyuan-Zeng/ZR_Experiment.git，日常分支为 main。
- 远程最初名为 ZR_Experimment，已按用户要求改名。再次操作以 git remote -v 和远程实际仓库为准。
- 仓库在本次操作时为公开状态。用户已明确授权将本工作区实验数据和报告上传至该仓库。同一范围内接续工作可沿用授权，实际审批拒绝时应说明原始原因，按允许的恢复方式处理。
- 保留现有 .gitignore 和 .gitattributes。每次从当前状态判断，已有 .git 时直接复用。
- 命令行和 GitHub Desktop 只保留一个推送进程。用户中断任务后，先核对后台进程和远程状态，再继续操作。

## 日常小批量推送流程

PowerShell 中先进入明确的工作目录，避免工具的默认目录发生变化。

```powershell
Set-Location -LiteralPath 'E:\A_ZQY\ZR_Experiment'
git rev-parse --show-toplevel
git remote -v
git branch --show-current
git -c core.quotepath=false status --short
git fetch origin main
if ($LASTEXITCODE -ne 0) { throw '读取远程失败，先排查连接' }
git rev-list --left-right --count HEAD...origin/main
```

最后两个数字分别表示本地独有和远程独有的提交数。远程有新增提交时，先检查历史并完成整合。推送被拒绝时依错误原因处理。

1. 查看本轮修改和新增文件的用途、大小及所属会话，确定本次提交范围。
2. 查清已有未推送提交。重试任务可能只需要 push。
3. 按本轮确定的文件或目录暂存，查看暂存结果后提交。
4. 推送成功后核对远程提交，并再次检查工作区，单独报告上传期间新增的文件。

```powershell
# 在已核对的具体文件或目录上执行 git add -- <路径>
git diff --cached --stat
git diff --cached --check
if ($LASTEXITCODE -ne 0) { throw '暂存内容存在需要处理的问题' }
git commit -m "说明本轮具体更新"
if ($LASTEXITCODE -ne 0) { throw '提交未成功，检查原因' }
git push origin main
if ($LASTEXITCODE -ne 0) { throw '推送未成功，保留本地提交并诊断' }
git rev-parse HEAD
git ls-remote origin refs/heads/main
if ($LASTEXITCODE -ne 0) { throw '远程验收查询失败' }
git -c core.quotepath=false status --short
```

PowerShell 中普通外部程序的非零退出码需要显式检查。仅设置 ErrorActionPreference 为 Stop，仍可能在 Git 命令失败后继续执行后面的命令。本次曾发生 untrack 参数错误、fetch 失败后继续执行的情况，后续脚本应在每个依赖步骤检查退出码。

git add --all 仅在已核对整个工作区均属于本轮上传范围时使用。上传期间仍有其他实验任务写入工作区时，优先选择确定的路径，保留其他任务的修改，并在结果中说明快照边界。

## 提交前整理内容

源码、项目文件、SDK 配套文件、冻结模型、原始数据、运行记录、结果和报告按实验追溯需要保留。根据实际用途识别 bin 内的交付文件和生成产物。

现有忽略规则涵盖 .vs、ipch、obj、__pycache__、编译中间文件、用户级 VS 配置及 Word 临时锁文件。实验日志可能属于运行证据，避免使用宽泛的 *.log 规则。本次初期曾采用该规则，已撤回。

.gitignore 只影响尚未跟踪的文件。对已跟踪文件改变存储方式时，需要同步处理索引及必要的未推送历史。检查 .env、令牌、私钥等是否混入本次具体文件范围，日志与提交说明中避免输出凭据或带签名的临时上传地址。

## 先决定大文件存储方式

GitHub 浏览器上传单文件上限为 25 MiB，普通 Git 单文件上限为 100 MiB；超过普通文件限制时可选 Git LFS，或按数据恢复需求设计分卷。这些限制依据本次查阅的官方说明，后续服务规则变化时重新核对。[GitHub 文件上传说明](https://docs.github.com/en/repositories/working-with-files/managing-files/adding-a-file-to-a-repository)

本次 B0 的 mechanism_samples.csv.gz 为 116,821,781 字节，约 111.4 MiB；其 gzip 尾部记录的展开大小约 1.20 GB。内容按时刻汇总观测年龄、状态、物理项、模型预测、误差与有效性掩码，因此即使压缩后仍较大。

### Git LFS

- 本次此前已有 6 个 LFS 对象完成上传，说明该路径在本机曾成功。
- 后来的 B0 对象持续出现代理写超时、客户端错误和连接中断。端口可用和小请求成功，只能证明相应连接可达，完整大文件上传仍需要独立验证。
- 增大超时可以容纳慢连接。本次将活动超时延长至 600 秒后仍未完成 B0 对象，不能据此宣称问题解决。
- track 在暂存前设置，之后重新暂存文件。检查 git lfs ls-files 和 .gitattributes。
- 本机 git lfs untrack 使用路径参数；本次实测不接受 --filename。使用前查阅 git lfs untrack --help。

### 无损分卷

本次将 B0 的压缩文件按原始字节切成 6 份，每份最多 20 MiB。分卷无需逐份解压，按清单顺序合并后得到原来的 gzip 文件。

- 分卷位于 分项实验/03_摩擦与残差补偿/B0/mechanism_samples.parts。
- manifest.json 保存原文件、各分卷的大小与 SHA-256。
- B0/restore_mechanism_samples.py 负责合并并校验；已有目标文件内容不匹配时会报错。
- 临时目录恢复试验已通过，恢复后 SHA-256 为 8a59fda7a89799d1b77e073904b4bc712067bf45cb8f2af5b018cbaeb2ae23e3，与原文件一致。
- 本地原 gzip 保留并按精确路径忽略。新下载仓库应先取得全部分卷，再在 B0 目录执行 python restore_mechanism_samples.py。
- 维护分卷数据时，同步更新清单，并重新验证完整恢复结果。

20 MiB 为本次针对不稳定连接选用的大小。日后按实际网络和文件用途调整。

**拆文件后一次 push，Git 仍可能把所有分卷打成同一个大包。** 本次单包在约 108 MiB 时遭遇连接重置。因此需要独立传输时，按每卷暂存、提交、推送、确认远程成功的顺序执行，再开始下一卷。

如果本地已经积压多个分卷提交，直接 push 会一起发送全部未到远程的对象。先检查提交边界，设计逐批推送方案。示例以本地只有当前这一卷待推送为前提。

```powershell
# 在 B0 目录内，对每一卷分别完成以下步骤，再处理下一卷
git add -- mechanism_samples.parts/mechanism_samples.csv.gz.part001
git commit -m "Upload B0 data volume part001"
if ($LASTEXITCODE -ne 0) { throw '分卷提交失败' }
git push origin main
if ($LASTEXITCODE -ne 0) { throw '当前卷失败，停止发起下一卷' }
git rev-parse HEAD
git ls-remote origin refs/heads/main
```

manifest 和恢复工具可以先随小文件报告提交。全部分卷到齐并完成恢复校验后，才能报告数据上传完整。

### 已提交的大文件改为分卷

只在尚未推送的本地历史中整理。本次先保留 backup/pre-split-20261127 和 backup/pre-batched-parts-20261127 两个本地备份分支，再整理待上传提交。相关恢复应结合 reflog 和现有分支状态进行。

仅在末尾新增分卷，旧大文件仍可能留在待推送祖先提交中，从而继续触发原来的大文件上传。需要处理待推送历史中对原文件的引用，并保留本地原数据及其他任务的未提交修改。历史整理前确认远程最新位置；已共享的历史按实际协作情况另行处理。

## Git 代理配置与诊断

本次实际代理由 Clash Verge 的 verge-mihomo 监听在 127.0.0.1:7897。下次先检查端口和进程，代理软件切换后可能变化。

```powershell
netstat -ano -p tcp | Select-String 'LISTENING'
git config --show-origin --get-regexp 'http.*proxy|http.version|lfs.*timeout'
git config --local http.proxy http://127.0.0.1:7897
git config --local --get http.proxy
git ls-remote origin refs/heads/main
```

上述配置写入当前仓库的 .git/config。本次还设置了 HTTP/1.1、LFS 单路传输、活动超时 600 秒、连接超时 60 秒及最大重试 3 次。这些是排查时的配置记录，可按后续证据调整。

Codex 的 ~/.codex/.env、进程环境变量、Git 配置和代理软件的路由规则各有作用。本次只证实 .env 写入及特定请求的连通性；Git 推送应单独检查实际采用的代理。Windows 系统代理关闭时，本地代理程序仍可能监听端口。

临时诊断可使用 git -c http.proxy= ls-remote origin refs/heads/main，但还应核对当前进程的 HTTP_PROXY、HTTPS_PROXY、ALL_PROXY 和 URL 专属代理设置。以实际连接地址及请求结果确认路线，避免把一条覆盖参数当作完整证据。

http.postBuffer 只在怀疑 HTTP 分块传输兼容性时进行针对性试验。本次曾对小批量推送临时设置 52,428,800 字节，报告提交随后成功；证据尚不足以认定是该参数修复了网络。增大缓冲区改变的是请求缓冲和分块行为，还会增加内存使用，无法普遍修复超时与断连。[Git 配置说明](https://git-scm.com/docs/git-config)

本次还尝试过临时公共 DNS 查询及指定解析地址。部分小请求可达，但解析、直连与后续上传均曾失败。小请求成功后仍需要观察实际上传；GitHub 地址可能变化，避免把本次临时 IP 固定为以后默认配置。

## 故障时的处理顺序

| 现象 | 先核对与下一步 |
| --- | --- |
| not a git repository | 查看当前目录，确认 .git 及仓库根目录。已有仓库时修正目录即可。 |
| dubious ownership | 核对目录所有者与当前用户。本次 CodexSandboxOffline 和 HYB 身份不同，可在确认目录可信后用单次 -c safe.directory=E:/A_ZQY/ZR_Experiment；后续子进程也需要取得相应配置。 |
| gh 不存在 | 查看现有 Git、凭据管理器或已连接的 GitHub 工具。缺少 gh 并不妨碍普通 Git 推送。 |
| 自动审批超时或拒绝 | 准确区分超时、工具故障和风险拒绝，依据实际返回信息恢复。此前用户的同范围授权应明确保留。 |
| Desktop 控制辅助程序初始化失败 | 按接口规定重试后切换到可用的命令行；报告实际工具错误。 |
| Failed to connect、解析失败 | 检查 GitHub 小请求、DNS、代理监听和实际路由，限制诊断请求的超时。 |
| 401 或 403 | 区分未带凭据的测试请求、仓库权限和具体上传端点；根据操作上下文判断。 |
| Connection was reset、write timeout、Empty reply | 记录端点、失败阶段、退出码和已完成批次，尝试小批量及可用路线；达到本轮重试边界后说明阻塞。 |
| 上传计数归零后重新增长 | 可能正在重传，应核对对象或远程提交是否真正完成。 |
| 推送失败后仍出现 Everything up-to-date | 结合退出码、后续远程查询及具体提交核验。本次在 RPC 失败后出现过这行文本。 |

诊断工具也要先确认可用性。本次默认 python.exe 指向 WindowsApps 占位程序，py 命令不存在；内置 Python 可执行但没有 requests。Python 标准库可用于检查数据和恢复文件。切换客户端前检查解释器、依赖及错误类型，避免反复启动无法工作的路径。

凭据交给已配置的管理器或已连接工具。确需诊断临时上传接口时，仅在内存中使用认证信息，避免将令牌、签名 URL、完整凭据响应打印到输出。

## GitHub Desktop 手工接续

1. 打开 GitHub Desktop，选择 File → Add Local Repository，添加 E:/A_ZQY/ZR_Experiment。
2. 确认当前仓库和 main 分支，查看未推送提交，再点击顶部 Push origin。
3. 已在命令行提交的修改可以显示为零个未提交文件，此时仍可能有提交待推送。
4. 网页的 Add file → Upload files 属于网页上传入口，Desktop 的 Push origin 位于桌面软件。
5. Desktop 与命令行共享本地仓库和网络条件。工具切换后继续按退出结果、远程提交及数据完整性验收。

官方操作入口见 [添加本地仓库](https://docs.github.com/en/desktop/adding-and-cloning-repositories/adding-a-repository-from-your-local-computer-to-github-desktop)。

## 完成条件和本次剩余项

推送成功需具备以下证据。

- 本次 push 的退出码为零，远程 refs/heads/main 等于应上传的本地提交。
- 已取得本轮具体文件清单，逐项确认所需文件在该远程提交中。
- LFS 文件需要验证实际对象可获取；分卷文件需要确认全部分卷可获取、合并后大小和 SHA-256 一致。
- 上传后的本地 status 已核对；其他任务新增或继续修改的文件单独说明。
- 报告最终提交号、已上传范围、未完成项及恢复所需操作。

截至本次记录整理，d68a51f 是此前已确认上传的报告、绘图、文档和恢复工具提交。本地已有分卷第 1 卷提交 913f3a5，第 2～6 卷仍显示为未跟踪。第 1 卷此前推送失败，六卷完整上传尚未验收；下一次先重新查询远程状态再接续，避免把此处的历史快照当作实时状态。

工作区还存在后续 B1 和源码修改。它们应在下一次用户要求的上传范围中按实际状态核对。

本次已经验证分卷可无损合并，尚未验证六个数据分卷已全部到达 GitHub。后续成功后补充最终提交和恢复验收结果。

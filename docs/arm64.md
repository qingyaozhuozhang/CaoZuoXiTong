# A1 多架构实现与验证

本项目提供 Linux x86-64 与 ARM64 两套本机架构后端。编译器目标决定选用的指令处理、trampoline 和状态验证程序，公共 C API 与事件布局保持一致。控制程序与被跟踪进程必须具有相同架构。

提交 `3ffd7368af3ac1ba1df4385ca4bf1f342b858903` 已在 GitHub 原生 x86_64 和 ARM64 Runner 上分别通过 full 与 UBSan 验证。运行编号为 `45946335169`，完整原始证据见 [验证报告归档](archive/README.md)。两架构内核 uprobe 对照均因权限不足跳过，完整 detach 延迟未满足 10 ms；模拟执行的时间数据不作为赛题性能指标的验收依据。

## 1. 架构差异与实现位置

| 项目 | x86-64 | ARM64 |
| --- | --- | --- |
| ABI | System V AMD64 | AAPCS64 |
| 指令处理 | [`x86dec.c`](../Proj40/x86dec.c) | [`aarch64.c`](../Proj40/aarch64.c) |
| 目标执行代码 | [`trampoline.S`](../Proj40/trampoline.S) | [`aarch64_trampoline.S`](../Proj40/aarch64_trampoline.S) |
| 入口补丁 | 5 字节 `JMP rel32`，约 ±2 GiB | 4 字节 `B imm26`，−128 MiB 至 +128 MiB−4 |
| 参数事件 | RDI、RSI、RDX、RCX、R8、R9 | X0–X5；X6/X7 与栈参数透传 |
| 返回事件 | RAX、RDX、XMM0 | X0、X1、V0 |
| 状态保护 | GPR、RFLAGS、red zone；A4 额外 XSAVE/FXSAVE | X0–X30、NZCV、16 字节栈对齐；A4 额外 Q0–Q31、FPCR/FPSR |
| 寄存器访问 | GETREGS/SETREGS | GETREGSET/SETREGSET、NT_PRSTATUS、NT_ARM_SYSTEM_CALL |
| 时间计数 | RDTSC，与单调时钟采样校准 | CNTVCT_EL0，按 CNTFRQ_EL0 换算并对齐单调时钟锚点 |
| 线程标识 | FS 基址，条件允许时读取 | TPIDR_EL0；控制侧读取 NT_ARM_TLS |

`ludp_event.fp_ret` 是浮点返回寄存器的 16 字节原始内容；`xmm0` 作为兼容别名保留。公开事件仍为 144 字节，调用方根据真实函数签名解释参数和返回值。

ARM64 入口若有 BTI，保留其着陆指令并在后续 4 字节安装补丁；`ludp_probe_info.addr` 表示实际补丁地址。trampoline 退出使用直接分支，避免借用 X16/X17 破坏目标现场。ADR/ADRP 通过 MOVZ/MOVK 重建地址，相对分支及 literal load 检查编码范围；无法安全重定位时返回失败。

ARM64 写入目标可执行内存使用 `/proc/<pid>/mem` 或 ptrace 路径，由 Linux 的 `copy_to_user_page` 同步指令缓存；不使用缺少该同步语义的 `process_vm_writev` 快路。事件发布使用 release store，槽位复用使用 acquire load，保持多生产者事件环的发布顺序。

## 2. 构建与功能复现

在原生 x86-64 或 ARM64 Linux 环境执行同一命令：

```bash
python3 scripts/run_demo.py --correctness-only
python3 scripts/run_demo.py --full --rounds 5
```

第一条运行全部九个 C 验证程序、两个演示程序与合流工具离线检查；第二条增加五轮百万次 `getpid` 基准及真实事件合流。每次强制重新构建并记录实际编译器、参数、平台和源码 SHA-256。无需手工为本机选择架构。

| 验证范围 | 对应程序或哨兵 |
| --- | --- |
| 入口补丁、参数、整数/浮点返回、事件配对 | `probe_demo`、`probe_abi_validation` |
| 状态完整性 | `abi_state_aarch64.S` 的 800 字节快照；`simd_state_aarch64.S` 的回调状态检查 |
| 超过寄存器数量的参数 | 十参数函数，覆盖 X6/X7 与栈上第 9、10 个参数 |
| 指令支持与拒绝边界 | `instruction_validation` |
| F4–F7 | 生命周期、多探针、线程与信号验证 |
| A2/A3/A4/A5 | 过滤、格式、目标回调和配置更新验证 |
| 未定义行为 | 同一套程序的 UBSan 构建 |

交叉编译可用于开发检查；切换编译器架构前执行 `make -C Proj40 clean`。交叉构建成功或 QEMU 执行成功，均不能据此报告原生执行开销。

## 3. GitHub Actions 原生验收

工作流为 [`.github/workflows/ci.yml`](../.github/workflows/ci.yml)，push、pull request 或手工触发后运行四个独立作业：

| 作业 | Runner | 内容 |
| --- | --- | --- |
| x86_64 / full | `ubuntu-24.04` | 完整正确性、演示、五轮百万次性能基准 |
| arm64 / full | `ubuntu-24.04-arm` | 相同流程，在原生 ARM64 环境执行 |
| x86_64 / ubsan | `ubuntu-24.04` | 全部正确性与演示、UBSan |
| arm64 / ubsan | `ubuntu-24.04-arm` | 相同 UBSan 流程，在原生 ARM64 环境执行 |

工作流核验 `uname -m`，记录 CPU、页大小、编译器、透明大页及 ptrace/perf 策略。一个作业失败不会取消其他架构；已有日志在失败时仍上传。新版门禁同时检查 `<1000 ns`、插入/移除 `<10 ms` 和 remove 至 detach 完成的完整清理 `<10 ms`。full 使用 `--require-kernel --require-merge`，必须取得真实内核 uprobe 对照与 ftrace 合流；sudo 权限限于临时 Runner 的验证命令，缺少内核设施仍会失败并保留日志。

原生 Runner 标签依据 [GitHub 官方参考](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)。共享 Runner 上的性能波动必须结合原始样本和环境解释；一次通过不代表所有 ARM64 设备均达标。

## 4. test 功能包与最新结果

与 `Proj40/` 同级的 [test 功能包](../test/README.md) 集中维护运行查询、结果同步和同步工具的离线验证。代码随仓库提交，下载内容保存在包内 `results/`，仅该结果子目录在下一次同步时替换。

首次同步前，安装支持 `gh auth token` 的 [GitHub CLI](https://github.com/cli/cli/blob/trunk/docs/install_linux.md)，并在本机完成登录：

```bash
gh auth login --hostname github.com --web
```

登录方式见 [GitHub CLI 官方说明](https://cli.github.com/manual/gh_auth_login)。脚本读取已有 `gh` 登录，或本机环境的 `GH_TOKEN` / `GITHUB_TOKEN`。凭据需要仓库 Actions 读取权限，不写入日志或仓库。能执行 `git push` 并不表示已为 GitHub API 配置认证；未认证请求也更容易达到 API 限额。下载接口依据 [GitHub artifact API](https://docs.github.com/en/rest/actions/artifacts#download-an-artifact)。

提交并推送代码后，在本地仓库根目录运行：

```bash
# 等待当前分支最新一轮结束并下载
python3 -m test sync --wait

# 也可按 GitHub Actions 页面中的运行编号下载
python3 -m test sync --run-id RUN_ID --wait
```

`test` 是 Python 功能包名，默认查询当前检出分支；如需指定名为 `test` 的分支，增加 `--branch test`。包目录名与 Git 分支名无绑定关系。

脚本启动时立即打印仓库、分支、认证与代理状态，每次请求前显示接口和尝试次数。`--wait` 表示等待选定的 CI 运行结束；查询运行记录、等待作业、下载附件分别显示进度。下载完成即可查看本地结果，无需执行 `git pull`。

默认沿用终端的 HTTP(S) 代理配置。需要显式指定时，使用代理软件实际提供的 HTTP 端口；以下端口仅为示例：

```bash
python3 -m test sync --wait --proxy http://127.0.0.1:7890
```

`--no-proxy` 可忽略环境代理、直接连接。`--request-timeout` 控制单次网络 I/O 等待时间，默认 15 秒；临时网络错误和 HTTP 500/502/503/504 默认额外重试两次，可通过 `--retries` 调整。该超时不等于整个下载的总时限；`--timeout` 单独控制选定运行的 CI 等待时间，默认 1800 秒。

| 提示 | 含义与处理 |
| --- | --- |
| 连接被重置、超时或连接被拒绝 | 检查终端代理、代理进程及 HTTP 端口；重试耗尽后保留已有结果 |
| API 限流，HTTP 403/429 | 未认证时先完成登录；已认证时等待限流解除，避免频繁重试 |
| 认证失败，HTTP 401 | 检查登录或环境变量中的凭据是否有效 |
| 拒绝访问，HTTP 403 | 检查 Actions 读取权限、组织策略及限流状态 |
| 资源不可见，HTTP 404 | 检查仓库和工作流是否已推送，以及私有仓库读取权限 |
| 分支尚无运行记录 | 确认当前分支已推送，并在 Actions 页面检查 `ci.yml` 是否触发 |

已启动的旧脚本不会自动载入代码修改；更新脚本后应先按 `Ctrl+C`，再重新执行同步命令。

```text
.
├── Proj40/
└── test/
    ├── README.md                 功能包用途和操作说明
    ├── __init__.py               Python 包标识
    ├── __main__.py               sync / validate 命令入口
    ├── sync_ci_results.py        GitHub 结果同步
    ├── ci_results_validation.py  同步逻辑离线验证
    └── results/
        ├── report.md             最新运行的作业结论与缺失项
        ├── latest.json           run ID、attempt、提交、分支、作业和 artifact
        ├── x86_64/
        │   ├── full/             完整报告、基准和 Runner 环境
        │   └── ubsan/            UBSan 分步报告和 Runner 环境
        ├── arm64/
        │   ├── full/
        │   └── ubsan/
        └── failed-jobs/          可下载的失败作业原始日志
```

查看 `test/results/report.md` 获取汇总结论，`test/results/latest.json` 提供结构化记录。架构目录内保留 artifact 原布局：`ci/<运行时间>/report.md`、`summary.json`、逐步日志，以及 `runner/environment.log`。首次下载前没有生成结果。

每次完整下载到暂存目录后，仅替换旧 `test/results/`，保留包代码和说明。失败运行也替换旧结果，并保留失败原因；未上传的架构明确标记缺失，不沿用上一轮 PASS。重新运行部分作业时只保存当前 attempt 的产物，其他作业报告标记缺失。网络或下载异常时不替换已有结果。

退出码 `0` 表示运行成功且四份产物齐全；`1` 表示已保存结果，但该轮失败、报告缺失或失败作业日志未能完整取得；`2` 表示同步未完成。结果目录中的生成文件由 Git 忽略，避免提交报告再次触发 CI。本地验证结果仍保存在 `artifacts/`，固定提交的验证报告保存在 `docs/archive/`，不随同步覆盖。

同步逻辑的离线回归可独立执行，不访问 GitHub，也不写入真实 `test/results/`：

```bash
python3 -m test validate
```

## 5. 验收边界

仅支持常规小端 AAPCS64；SVE/SME 扩展状态、GCS 和 PAC 返回协议未纳入兼容范围。附加或暂停时检查到活动扩展状态会拒绝操作；目标运行期间也不得切换到这些未支持状态。部分入口编码如 BL、SVC、PACIASP 被拒绝，不能将有限解码器表述为完整 ARM 指令集支持。

探针管理仍要求应用安全点，返回探针仍要求正常 ABI 返回；异常展开、`longjmp`、换栈、代码动态卸载等限制见 [支持范围与限制](limitations.md)。A1 验收应同时核对两个原生架构、同一源码提交、功能与 UBSan 结论及性能原始样本。

实现依据：[Arm AAPCS64](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst)、[Linux ARM64 ptrace](https://github.com/torvalds/linux/blob/v6.6/arch/arm64/kernel/ptrace.c)、[Linux ARM64 可执行页缓存同步](https://github.com/torvalds/linux/blob/v6.8/arch/arm64/mm/flush.c)。

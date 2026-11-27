# A1 多架构实现与验证

本项目提供 Linux x86-64 与 ARM64 两套本机架构后端。编译器目标决定选用的指令处理、trampoline 和状态验证程序，公共 C API 与事件布局保持一致。控制程序与被跟踪进程必须具有相同架构。

提交 `b3988f82b2108ce99bdd192855b367483477bde8` 已在 GitHub Actions 原生 x86_64 和 ARM64 Runner 上分别通过 full 与 UBSan 验证，运行编号为 `45958959682`，attempt 为 `1`。两个 full 作业均完成真实内核 uprobe 对照、A3 ftrace 合流和完整清理耗时门禁，固定原始证据见 [验证报告归档](archive/README.md)。ARM64 结论来自 GitHub 的 ARM64 机器，与本地 x86_64 测试分别记录；交叉编译或 QEMU 执行不能替代该原生证据。

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
| A2/A4/A5 | 过滤、目标回调和配置更新验证 |
| A3 | perf-script 风格输出、合流工具离线检查；full 另采集真实 ftrace 事件进行合流 |
| 未定义行为 | 同一套程序的 UBSan 构建 |

交叉编译可用于开发检查；切换编译器架构前执行 `make -C Proj40 clean`。交叉构建成功或 QEMU 执行成功，均不能据此报告原生执行开销。

## 3. GitHub Actions 原生验收

工作流为 [`.github/workflows/ci.yml`](../.github/workflows/ci.yml)，push、pull request 或手工触发后运行四个独立作业：

| 作业 | Runner | 内容 |
| --- | --- | --- |
| x86_64 / full | `ubuntu-24.04` | 功能与正确性、五轮百万次基准、真实内核 uprobe 对照、A3 合流 |
| arm64 / full | `ubuntu-24.04-arm` | 相同 full 流程，在原生 ARM64 环境执行 |
| x86_64 / ubsan | `ubuntu-24.04` | 功能与正确性、两个演示程序和离线工具检查，启用 UBSan |
| arm64 / ubsan | `ubuntu-24.04-arm` | 相同 UBSan 流程，在原生 ARM64 环境执行 |

工作流核验 `uname -m`，记录 CPU、页大小、编译器、透明大页及 ptrace/perf 策略。一个作业失败不会取消其他架构；已有日志在失败时仍上传。性能门禁同时检查 `<1000 ns`、插入/移除 `<10 ms` 和 remove 至 detach 完成的完整清理 `<10 ms`。full 使用 `--require-kernel --require-merge`，必须取得真实内核 uprobe 对照与 ftrace 合流；sudo 权限限于临时 Runner 的验证命令，缺少内核设施仍会失败并保留日志。

UBSan 作业专门检查执行路径中的未定义行为，不进行性能测量、内核能力预检或真实 ftrace 合流；这些步骤在其报告中标记 `SKIP`，由同一提交的 full 作业提供对应证据。不能将 UBSan 插桩运行的耗时用于性能验收，也不能将此分工中的跳过解释为 full 缺少结果。

本轮 x86_64 使用 Intel Xeon 6975P-C，ARM64 使用 Neoverse-N2，均为原生 Runner。A3 在每个架构核验 100 次 getpid 调用、200 条用户态事件和 100 条内核事件，两路均未丢失。内核事件超出对应入口/返回区间的最大距离分别为 1,623 ns 和 6,091 ns，低于预设的 100,000 ns 容差。该验证限于固定 CPU 的约 106 ms 短会话，不代表跨 CPU 或长时钟漂移已完成验证。

原生 Runner 标签依据 [GitHub 官方参考](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)。共享 Runner 上的性能波动必须结合原始样本和环境解释；一次通过不代表所有 ARM64 设备均达标。

## 4. test 功能包与最新结果

与 `Proj40/` 同级的 [test 功能包](../test/README.md) 集中维护运行查询、结果同步和同步工具的离线验证。代码随仓库提交，下载内容保存在包内 `results/`，仅该结果子目录在下一次同步时替换。

GitHub CLI 的安装、登录、代理和故障处理集中见 [test 功能包说明](../test/README.md)。首次同步前需在本机完成 GitHub API 登录：

```bash
gh auth login --hostname github.com --web
```

推送 `test` 分支后，在本地仓库根目录运行常用同步命令；代理端口按实际配置填写：

```bash
python3 -m test sync --branch test --wait --proxy http://127.0.0.1:7890
```

`--wait` 等待选定的 Actions 运行结束并下载附件，下载完成即可查看 `test/results/report.md`，无需执行 `git pull`。`latest.json` 保存 run ID、attempt、提交和产物信息，架构子目录保存分步报告、性能样本、合流原始事件与 Runner 环境。包目录名与 Git 分支名无绑定关系。

同步仅替换 `test/results/`，不改包代码；失败运行保留失败和缺失项，不混用上一轮 PASS，下载异常则保留原结果。下载目录由 Git 忽略。本地复现写入 `artifacts/`，正式提交的固定证据由 [验证报告归档](archive/README.md) 导航，且不随同步覆盖。

## 5. 验收边界

仅支持常规小端 AAPCS64；SVE/SME 扩展状态、GCS 和 PAC 返回协议未纳入兼容范围。附加或暂停时检查到活动扩展状态会拒绝操作；目标运行期间也不得切换到这些未支持状态。部分入口编码如 BL、SVC、PACIASP 被拒绝，不能将有限解码器表述为完整 ARM 指令集支持。

探针管理仍要求应用安全点，返回探针仍要求正常 ABI 返回；异常展开、`longjmp`、换栈、代码动态卸载等限制见 [支持范围与限制](limitations.md)。A1 验收应同时核对两个原生架构、同一源码提交、功能与 UBSan 结论及性能原始样本。

实现依据：[Arm AAPCS64](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst)、[Linux ARM64 ptrace](https://github.com/torvalds/linux/blob/v6.6/arch/arm64/kernel/ptrace.c)、[Linux ARM64 可执行页缓存同步](https://github.com/torvalds/linux/blob/v6.8/arch/arm64/mm/flush.c)。

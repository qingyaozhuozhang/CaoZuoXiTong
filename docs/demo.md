# 功能演示与结果复现

本指南说明“轻量级用户态动态探针”的可执行演示、输出判定和验收复现方法。演示以可执行程序和原始日志为证据，覆盖 F1–F7，并在完整模式中运行正确性回归、A2/A3/A4/A5 测试和百万次 `getpid` 性能基准。提供 Linux x86-64 和 ARM64 两套后端；双架构原生结果由 GitHub Actions 分别生成，详见 [ARM64 实现与验证](arm64.md)。

## 1. 一键运行

在仓库根目录执行。所需工具为 Python 3.9+、GNU Make、GCC 或兼容编译器、binutils，以及允许父进程跟踪子进程的 Linux x86-64 或 ARM64 环境。演示程序自行创建受控子进程。

```bash
# 快速演示：构建 + F1/F2/F3 真实事件 + F4/F5 + F6 + F7
python3 scripts/run_demo.py

# 完整验收：快速演示 + ABI/API/进阶回归 + 5 轮百万次 getpid 基准
python3 scripts/run_demo.py --full

# 正式三组对比：必须取得内核 uprobe 实测，否则返回失败
python3 scripts/run_demo.py --full --require-kernel
```

也可以从项目构建入口运行 `make -C Proj40 demo` 或 `make -C Proj40 demo-full`。需要指定采样轮数或结果父目录时，使用脚本参数：

```bash
python3 scripts/run_demo.py --full --rounds 5 --output artifacts/demo
python3 scripts/run_demo.py --help
```

每次运行都会创建一个新的 `artifacts/demo/<运行时间>/` 目录，运行时间采用 `Asia/Shanghai` 时区。脚本不覆盖先前结果。快速模式用于功能展示，不输出性能达标结论；完整模式的每轮调用数固定为 1,000,000。

执行器使用 `make -B` 强制重新构建，避免沿用其他编译参数或检测器生成的旧二进制。默认 `CFLAGS` 为项目的 `-O2 -g -Wall -Wextra -fno-strict-aliasing -pthread`，`LDFLAGS` 为 `-pthread`；可以通过环境变量显式覆盖 `CC`、`AR`、`CFLAGS` 和 `LDFLAGS`。报告记录实际构建参数及 `LUDP_*` 配置，并核验构建前后的源码 SHA-256 一致。执行器不继承 `MAKEFLAGS`、`MFLAGS`、`MAKEOVERRIDES` 和 `GNUMAKEFLAGS`，防止上层 Make 的预演或其他控制选项跳过真实构建。

## 2. 输出与校验内容

### F1/F2/F3：真实入口与返回事件

[`probe_demo.c`](../Proj40/probe_demo.c) 启动目标子进程，再由父进程安装探针。目标程序实际调用 `libprobe_targets.so` 中的两个共享库函数：

```c
six(1, 2, 3, 4, 5, 6);  /* 结果：183 */
floating(2.5);           /* 结果：4.0 */
```

演示依次展示并校验：

1. 目标 PID、共享库符号、函数地址和探针 ID。
2. 安装前后的入口字节；x86-64 检查 `E9`（`JMP rel32`），ARM64 检查直接 `B` 指令；存在 BTI 时保留首条 BTI，检查其后的补丁。
3. 从事件中读取的前六个 INTEGER 类参数 `[1, 2, 3, 4, 5, 6]`，对应 x86-64 的 `RDI/RSI/RDX/RCX/R8/R9` 或 ARM64 的 `X0–X5`。
4. `six` 的整数返回值 `183`（`RAX` / `X0`），以及 `floating` 按 `double` 解读的浮点返回值 `4.00`（`XMM0` / `V0`）。
5. 入口与返回事件的 `call_id` 配对；两个函数各收到一条入口事件和一条返回事件，丢失数与运行期 `ptrace` 停止数为零。
6. 移除探针后的原始入口字节恢复，以及会话分离结果。

以下是确定性校验值的说明，不是某次运行的原始日志。实际 PID、地址、探针编号和 `call_id` 应以现场输出为准。

| 展示项 | 预期值 | 不满足时的行为 |
|---|---|---|
| `six` 的六个参数 | `1, 2, 3, 4, 5, 6` | 演示失败 |
| `six` 的整数返回值 | `183` | 演示失败 |
| `floating` 的浮点返回值 | `4.00` | 演示失败 |
| 入口/返回事件 | 每个函数各 `1 / 1` 条，配对信息一致 | 演示失败 |
| 事件丢失、返回记录丢失 | 均为 `0` | 演示失败 |
| 本次函数调用过程中的 `ptrace` 停止 | `0` | 演示失败 |
| 移除后的入口字节 | 与安装前读取的 16 字节一致 | 演示失败 |

这里的“无运行期停止”指受控函数调用期间没有观测到 `ptrace` 停止。安装与管理操作仍使用 `ptrace`；目标函数本身也可以包含系统调用，例如基准中的 `getpid`。不要将“探针用户态跳转”解释成“目标程序不使用内核”。

`libprobe_targets.so` 是便于解释 ABI 的测试夹具，编译时预留了函数入口补丁空间；完整模式中的 `getpid` 探针直接安装到目标进程的 `libc.so.6`，用于验证真实动态库场景。

### F4–F7：可复现的功能检查

| 演示步骤 | 程序 | 展示内容 | 验证边界 |
|---|---|---|---|
| F4/F5 | [`probe_lifecycle_validation`](../Proj40/probe_lifecycle_validation.c) | 三个会话中的原始指令恢复、映射回收、动态开关和在途返回处理 | 在应用安全点进行探针管理 |
| F6 | [`multiprobe_validation`](../Proj40/multiprobe_validation.c) | 16 个不同地址的探针，以及同地址探针链 | 本次受控并发测试 |
| F7 | [`thread_signal_validation`](../Proj40/thread_signal_validation.c) | 主线程活跃或阻塞时的线程管理，以及信号交付 | 测试覆盖的线程/信号场景 |

F4 的映射回收检查与单个函数入口恢复是两个验收点；`probe_demo` 的恢复字节展示不能单独代替无映射泄漏检查。完整模式追加 `instruction_validation`、`probe_abi_validation`、`api_contract_validation`、`probe_extensions_validation` 和 `callback_validation`，保留各自的日志。

## 3. 演示执行顺序

先运行完整模式并检查全部结果，再按以下顺序复现功能。性能采样应单独进行，避免与编译、录屏或其他负载争用 CPU。

| 步骤 | 执行或查看内容 | 核验依据 |
| --- | --- | --- |
| 1 | README、架构说明和本次运行环境 | 目标架构、源码提交、支持范围 |
| 2 | `python3 scripts/run_demo.py` | 构建成功，真实入口跳转及参数/返回值校验 |
| 3 | 生命周期、多探针、多线程步骤 | 原始字节恢复、映射回收、动态开关、16 个地址与信号处理 |
| 4 | 完整模式的进阶功能日志 | 条件过滤、目标回调、状态恢复和配置更新 |
| 5 | `benchmark/benchmark.md` | 百万次调用、逐轮增量、插入/移除及 attach/detach 原始口径 |
| 6 | `report.md`、`summary.json` 和步骤日志 | 退出码、源码 SHA-256、PASS/FAIL/SKIP 及缺失项 |
| 7 | GitHub 双架构报告 | 相同提交的 x86-64、ARM64 功能与 UBSan 结果 |

日志和结构化报告为主要复核材料。需要录屏时，可以按此顺序记录实际命令及输出，并注明提交、日期和环境；录屏属于可选的辅助材料，不是本文新增的赛题要求。

## 4. 性能结果如何解读

完整模式调用 [`benchmark.py`](../Proj40/benchmark.py)，运行以下模式，并将输出放入本次目录下的 `benchmark/`：

| 模式 | 含义 |
|---|---|
| `baseline` | 无探针、无 tracer 附加 |
| `inert` | 已附加但未插入探针，校验入口字节未改动 |
| `entry` | 用户态入口探针，每次函数调用产生一条事件 |
| `return` | 用户态入口与返回探针，每次函数调用产生两条事件 |
| `uprobe` | 内核 perf uprobe，对照命中计数和执行耗时 |

单次额外开销按“同一轮探针模式 `ns_per_call` − `baseline ns_per_call`”计算，再取多轮中位数。程序分别检查入口模式、入口加返回模式的额外开销 `< 1000 ns`，以及各样本插入/移除延迟 `< 10 ms`。`attach` 和 `detach` 耗时单独列出，不混入插入/移除耗时。

无探针模式的时间差受调度、频率和缓存影响，正负差值均可能出现；入口字节未改动与执行路径没有探针跳转，才是“未插入探针不增加探针执行路径”的直接依据。一次计时差不能证明严格的零纳秒。

内核 uprobe 受 PMU 支持与 `perf_event_open` 权限影响。普通完整模式允许其为 `SKIP`，报告会保留具体原因，此时尚未完成“无探针 / 内核 uprobe / 用户态探针”的三组实测对照。正式需要该项时使用 `--require-kernel`，缺失内核数据会使验收失败。内核侧采集字段和用户态事件字段不同，报告不宣称两者具有等价的完整日志吞吐负载。

## 5. 结果在项目中的位置

固定入口与代码随仓库维护：

```text
README.md                    项目与评审入口
docs/demo.md                 本指南
scripts/run_demo.py          一键执行、超时处理与证据归档
Proj40/probe_demo.c           真实事件演示程序
Proj40/Makefile               demo / demo-full 构建入口
scripts/sync_ci_results.py    GitHub 最新结果下载与替换
test/                        与 Proj40 同级，仅保存最新一轮 GitHub 结果
```

本地演示使用独立运行目录：

```text
artifacts/demo/<运行时间>/
├── report.md                人可读的逐项 PASS / FAIL / SKIP 汇总
├── summary.json             命令、退出码、环境与构建参数、提交、构建前后源码 SHA-256
├── 01-build.log             构建命令和原始输出
├── 02-entry-return.log      真实参数、返回值、入口字节和配对证据
├── 03-lifecycle.log         F4/F5
├── 04-multiprobe.log        F6
├── 05-threads.log           F7
├── 06-*.log … 10-*.log      完整模式的额外回归日志
├── 11-benchmark.log         完整模式的基准执行日志
└── benchmark/
    ├── benchmark.md         性能报告
    ├── benchmark.json       逐轮测量数据
    └── benchmark.log        基准程序原始输出
```

`PASS` 只表示本次选择的相应检查通过，`FAIL` 表示构建、功能或阈值检查失败，`SKIP` 表示该项未取得结果。快速模式整体通过仍会将性能与内核对照列为 `SKIP`。失败时仍保留报告与已完成的日志；单步骤超时、`Ctrl+C`（SIGINT）或 SIGTERM 中断会终止其进程组，并写出失败报告。中断退出码分别为 `130`（SIGINT）和 `143`（SIGTERM）。

GitHub 结果单独保存。在代码推送并触发 CI 后执行：

```bash
python3 scripts/sync_ci_results.py --wait
```

脚本下载当前分支最新一轮结果，成功下载后整体替换根目录 `test/`，只保留当前轮次的 x86-64 / ARM64 full 与 UBSan 结果。`test/report.md` 是汇总入口，`latest.json` 记录提交和作业状态；缺失架构报告不会沿用旧版本。失败运行也保存已有报告与可下载的失败作业日志；下载失败保留原目录。登录方式、目录结构和退出码见 [多架构验证说明](arm64.md)。

本地演示与模拟环境结果保存在 `artifacts/`，不写入 GitHub 专用的 `test/`。原始历史报告位于 `docs/archive/`，不因下载最新结果而改写。

## 6. 常见问题

| 现象 | 处理方式 |
|---|---|
| 不是 Linux x86-64 / ARM64 | 使用对应原生 Linux 环境；ARM64 CI 使用 `ubuntu-24.04-arm` |
| 构建工具缺失 | 安装 GCC/兼容编译器、GNU Make、binutils 和 Python 3.9+ 后重试 |
| `ludp_attach: Operation not permitted` | 检查容器能力、沙箱和主机 ptrace 策略；使用允许父进程跟踪子进程的测试环境 |
| 内核 uprobe 为 `SKIP` | 查看 `benchmark.log` 的实际原因；在支持 uprobe PMU 且具备 perf 权限的验收机重新执行严格模式 |
| 性能阈值为 `FAIL` | 保留失败样本，检查主机负载、CPU 频率与运行环境；在同一验收条件下复测，不删除失败记录或改写阈值 |
| 某步骤超时或功能失败 | 从 `report.md` 打开该步骤日志，复现其中记录的命令；功能问题解决前不据此生成性能达标结论 |

演示成功是本次受控场景的结果。复杂异常展开、`longjmp`、换栈以及 CET shadow stack、ARM GCS/PAC 和 SVE/SME 等边界，应结合公共 API 和设计说明评估，不由一次演示推导为任意程序均可安全插桩。

# 功能演示与结果复现

本指南说明“轻量级用户态动态探针”的可执行演示、输出判定和验收复现方法。演示以可执行程序和原始日志为证据，覆盖 F1–F7，并在完整模式中运行正确性回归、A2/A3/A4/A5 测试和百万次 `getpid` 性能基准。提供 Linux x86-64 和 ARM64 两套后端；双架构原生结果由 GitHub Actions 分别生成，详见 [ARM64 实现与验证](arm64.md)。

## 1. 一键运行

在仓库根目录执行。所需工具为 Python 3.9+、GNU Make、GCC 或兼容编译器、binutils，以及允许父进程跟踪子进程的 Linux x86-64 或 ARM64 环境。演示程序自行创建受控子进程。

```bash
# 快速演示：构建 + F1/F2/F3 真实事件 + F4/F5 + F6 + F7
python3 scripts/run_demo.py

# 完整验收：快速演示 + ABI/API/进阶回归 + 5 轮百万次 getpid 基准
python3 scripts/run_demo.py --full

# 严格验收：必须取得内核 uprobe 对照和 A3 真实事件合流
# 在具备 ptrace、perf 和 tracefs instance 权限的环境执行；GitHub CI 已配置
python3 scripts/run_demo.py --full --require-kernel --require-merge
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
| F4/F5 | [`probe_lifecycle_validation`](../Proj40/probe_lifecycle_validation.c) | 四个会话（含 seccomp 目标）中的原始指令恢复、映射回收、动态开关和在途返回处理 | 在应用安全点进行探针管理 |
| F6 | [`multiprobe_validation`](../Proj40/multiprobe_validation.c) | 16 个不同地址的探针，以及同地址探针链 | 本次受控并发测试 |
| F7 | [`thread_signal_validation`](../Proj40/thread_signal_validation.c) | 主线程活跃或阻塞时的线程管理，以及信号交付 | 测试覆盖的线程/信号场景 |

F4 的映射回收检查与单个函数入口恢复是两个验收点；`probe_demo` 的恢复字节展示不能单独代替无映射泄漏检查。完整模式追加指令、ABI、API、独立 exec/ASLR、条件过滤、目标回调及配置更新验证，并保留各自的日志。

### A1–A5：进阶功能与真实事件流

| 条目 | 演示中的检查 | 查看位置 |
|---|---|---|
| A1 | 同一提交在 x86_64 与 ARM64 原生 Runner 上执行 | 下载结果的双架构 CI 汇总 |
| A2 | 多种比较条件、第六参数过滤，按预期集合核对事件 | `09-advanced.log` |
| A3 | 同机采集 libc getpid 用户态入口/返回与真实 ftrace 内核事件，按单调时间合流 | `event-merge/report.md`、`merged.perf` |
| A4 | 在目标进程内执行回调，核验参数、现场恢复和忙时保护 | `10-callback.log` |
| A5 | 安全点修改过滤、开关、模式和回调，核验 ID、版本及错误配置处理 | `09-advanced.log`、`10-callback.log` |

A3 使用独立 tracefs instance，选择 `mono` 时钟，将目标固定到一个允许的 CPU，默认采集 100 次调用。原始两路数据、合流文本、缓冲统计与时钟偏差均保留；实例在结束时释放。普通本地运行缺少权限时标为 `SKIP`，严格模式失败。离线解析检查只验证工具，不能替代真实采集。独立运行方式：

```bash
make -C Proj40 -j4
python3 scripts/event_merge_demo.py --require-kernel
```

## 3. 演示执行顺序

先运行完整模式并检查全部结果，再按以下顺序复现功能。性能采样应单独进行，避免与编译、录屏或其他负载争用 CPU。

| 步骤 | 执行或查看内容 | 核验依据 |
| --- | --- | --- |
| 1 | README、架构说明和本次运行环境 | 目标架构、源码提交、支持范围 |
| 2 | `python3 scripts/run_demo.py` | 构建成功，真实入口跳转及参数/返回值校验 |
| 3 | 生命周期、多探针、多线程步骤 | 原始字节恢复、映射回收、动态开关、16 个地址与信号处理 |
| 4 | 完整模式的进阶功能日志 | 条件过滤、目标回调、状态恢复和配置更新 |
| 5 | `event-merge/report.md` 与 `merged.perf` | 真实内核事件、用户态事件及合流后的时间顺序 |
| 6 | `benchmark/benchmark.md` | 百万次调用、逐轮增量、插入/移除、完整清理及 attach/detach |
| 7 | `report.md`、`summary.json` 和步骤日志 | F1–F7 / A1–A5 覆盖表、源码 SHA-256、PASS/FAIL/SKIP |
| 8 | GitHub 双架构 CI 汇总报告 | 相同提交的 x86-64、ARM64 full 与 UBSan 结果 |

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

单次额外开销按“同一轮探针模式 `ns_per_call` − `baseline ns_per_call`”计算，再取多轮中位数。程序分别检查入口模式、入口加返回模式的额外开销 `< 1000 ns`，以及各样本插入、移除和完整清理 `< 10 ms`。完整清理从 remove 开始，包含原始字节校验，直到 detach 返回；分段耗时继续保留，attach 单列。

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
test/                        与 Proj40 同级的 GitHub 结果同步与离线验证功能包
test/results/                下载生成，仅保存最新一轮 GitHub 结果
```

本地演示使用独立运行目录：

```text
artifacts/demo/<运行时间>/
├── report.md                功能、正确性与性能验证报告（含全部赛题项覆盖表）
├── summary.json             命令、退出码、环境与构建参数、提交、构建前后源码 SHA-256
├── 01-build.log             构建命令和原始输出
├── 02-entry-return.log      真实参数、返回值、入口字节和配对证据
├── 03-lifecycle.log         F4/F5
├── 04-multiprobe.log        F6
├── 05-threads.log           F7
├── 06-*.log … 10-*.log      完整模式的额外回归日志
├── 11-benchmark.log         完整模式的基准执行日志
├── 12-event-merge.log       真实用户态 / ftrace 合流日志
├── event-merge/
│   ├── report.md            A3 用户态与内核事件合流验证报告
│   ├── summary.json         实际状态、采集配置及误差统计
│   ├── ludp.perf            用户态原始事件（采集成功时）
│   ├── kernel.ftrace        内核原始事件（采集成功时）
│   └── merged.perf          合流结果（校验通过时）
└── benchmark/
    ├── benchmark.md         getpid 探针性能基准报告
    ├── benchmark.json       逐轮测量数据
    └── benchmark.log        基准程序原始输出
```

`PASS` 只表示本次选择的相应检查通过，`FAIL` 表示构建、功能或阈值检查失败，`SKIP` 表示该项未取得结果。快速模式整体通过仍会将性能与内核对照列为 `SKIP`。失败时仍保留报告与已完成的日志；单步骤超时、`Ctrl+C`（SIGINT）或 SIGTERM 中断会终止其进程组，并写出失败报告。中断退出码分别为 `130`（SIGINT）和 `143`（SIGTERM）。

GitHub 结果单独保存。在代码推送并触发 CI 后执行：

```bash
python3 -m test sync --branch test --wait --proxy http://127.0.0.1:7890
```

`test/` 功能包下载当前分支最新一轮结果，成功下载后仅替换 `test/results/`，保留包代码和说明。结果子目录只保存当前轮次的 x86-64 / ARM64 full 与 UBSan 结果。`test/results/report.md` 是汇总入口，`test/results/latest.json` 记录提交和作业状态；缺失架构报告不会沿用旧版本。失败运行也保存已有报告与可下载的失败作业日志；下载失败保留原结果。登录方式、目录结构和退出码见 [多架构验证说明](arm64.md)。

上述为 `test` 分支和本机 HTTP 代理的常用命令；其他分支或端口按实际配置调整。下载完成即可查看报告，不需要 `git pull`。报告名称与范围见[报告阅读顺序](testing.md#8-报告名称与阅读顺序)。

本地演示与模拟环境结果保存在 `artifacts/`，不写入 GitHub 专用的 `test/results/`。[验证报告归档](archive/README.md) 固定保存 `3ffd736` 的双架构 full / UBSan 结果、逐步日志、环境和性能样本，可用于查看完整演示的实际输出；后续下载不会改写这份归档。

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

# 功能演示与结果复现

本指南介绍可执行演示、报告阅读顺序和复现方法。演示程序自行创建受控目标进程，展示动态库入口跳转、参数与返回值采集，再验证指令恢复、动态开关、多探针和多线程场景。完整流程还包含 ABI、ASLR/PIE、进阶功能、`getpid` 性能基准和真实用户态／内核事件合流。

已归档的验证依据为提交 **`b3988f8`**、GitHub Actions 运行 **`45958959682 / attempt 1`**：x86_64 与 ARM64 的 full、UBSan 四个作业均通过。固定报告入口为 [验证报告归档](archive/README.md)，下载的新结果保存在 `test/results/`。下文实测值仅对应这次提交及其运行环境。

## 1. 常用运行方式

在仓库根目录执行。需要 Linux x86_64 或 ARM64、Python 3.9+、GNU Make、GCC 或兼容编译器、binutils，以及允许父进程跟踪子进程的 `ptrace` 权限。

```bash
# 先查看真实参数、返回值及 F4–F7 功能输出
python3 scripts/run_demo.py

# 完整复现：正确性回归、进阶功能、五轮百万次基准与 A3 合流
python3 scripts/run_demo.py --full
```

执行结束后，终端给出 `artifacts/demo/<运行时间>/report.md` 路径。先查看主报告，再进入失败步骤的日志或专项报告。每次运行新建目录，保留以前的结果；运行时间采用 `Asia/Shanghai` 时区。

**推送后下载 GitHub 双架构结果**：首次使用先完成 [GitHub 本地配置与登录](../test/README.md#首次配置-github只需一次)，之后常用命令为：

```bash
python3 -m test sync --branch test --wait --proxy http://127.0.0.1:7890
```

下载后直接打开 `test/results/report.md`，无需 `git pull`。命令等待 `test` 分支最新一轮 CI 完成，成功同步后整体替换 `test/results/`；网络或下载失败时保留原结果，因此应同时核对 `latest.json` 中的提交和运行编号。分支与代理端口按实际配置调整。

## 2. 运行模式与结果含义

| 模式 | 命令或配置 | 实际执行范围 |
|---|---|---|
| quick | 不带模式参数 | 构建、F1/F2/F3 事件演示、F4/F5、F6、F7；不测性能或真实合流 |
| full | `--full` | quick 加全部正确性回归、A2/A4/A5、事件导出、合流工具检查、性能基准与真实 A3 采集；内核能力或权限不足时可出现 `SKIP` |
| strict full | `--full --require-kernel --require-merge` | 与 full 相同，但必须取得每轮内核 uprobe 对照和 A3 真实合流，缺失即失败；GitHub full 作业采用此配置 |
| correctness-only | `--correctness-only` | 全部正确性回归及用户态事件导出、离线合流检查；不测性能、不采集真实内核合流 |
| UBSan | 配置 UBSan 编译／链接参数后运行 `--correctness-only` | 检查执行到的 C 代码是否出现未定义行为；GitHub 两个 UBSan 作业均采用此配置 |

`--correctness-only` 本身不会开启 UBSan。UBSan 也不代替汇编、并发或性能验证；其报告中的性能和真实 A3 合流为 `SKIP`，属于该模式的预期范围。

单架构报告中的 **A1 为 `SKIP`**，表示它只能证明本架构的运行结果。A1 要结合**同一提交**的 x86_64 和 ARM64 原生报告、环境日志与源码标识判断，不能由一份报告或模拟环境结果代替。归档的 `b3988f8` 已具备这两份原生 full 结果及对应 UBSan 结果。

`PASS` 表示本次实际执行的检查通过；`FAIL` 表示构建、功能或阈值检查失败；`SKIP` 表示该项未取得结果。报告整体 `PASS` 不会把跳过项变为已验证项。

<details>
<summary>严格复现、UBSan 和其他执行选项</summary>

严格模式需要 `ptrace`、perf uprobe 和 tracefs instance 权限，在已具备这些权限的环境执行：

```bash
python3 scripts/run_demo.py --full --require-kernel --require-merge
```

复现 CI 的 UBSan 配置：

```bash
CFLAGS='-O1 -g -Wall -Wextra -Werror -fno-strict-aliasing -fno-omit-frame-pointer -fsanitize=undefined -fno-sanitize-recover=all -pthread' \
LDFLAGS='-pthread -fsanitize=undefined -fno-sanitize-recover=all' \
UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1' \
python3 scripts/run_demo.py --correctness-only
```

`--rounds` 调整完整模式的采样轮数，`--output` 指定结果父目录，`--help` 查看选项。`make -C Proj40 demo` 和 `make -C Proj40 demo-full` 是对应构建入口。

执行器使用 `make -B` 强制重建，记录实际编译参数、`LUDP_*` 配置及构建前后的源码 SHA-256。默认使用 `-O2`；GitHub full 还启用 `-Werror`。执行器不继承 Make 的预演等控制变量，避免未实际构建却继续验证。

</details>

## 3. 已归档结果的阅读顺序

从 [验证报告归档](archive/README.md) 进入 [本轮双架构 CI 汇总](archive/reports/2026-11-27-b3988f8/report.md)，依次核对提交、四个作业状态和各架构主报告。

| 阅读目的 | x86_64 | ARM64 |
|---|---|---|
| F1–F7、A2–A5 与正确性检查 | [full 主报告](archive/reports/2026-11-27-b3988f8/x86_64/full/ci/20261127-153059-814545/report.md) | [full 主报告](archive/reports/2026-11-27-b3988f8/arm64/full/ci/20261127-153056-279603/report.md) |
| 开销、插入与完整清理 | [性能基准](archive/reports/2026-11-27-b3988f8/x86_64/full/ci/20261127-153059-814545/benchmark/benchmark.md) | [性能基准](archive/reports/2026-11-27-b3988f8/arm64/full/ci/20261127-153056-279603/benchmark/benchmark.md) |
| A3 真实事件合流 | [合流报告](archive/reports/2026-11-27-b3988f8/x86_64/full/ci/20261127-153059-814545/event-merge/report.md) | [合流报告](archive/reports/2026-11-27-b3988f8/arm64/full/ci/20261127-153056-279603/event-merge/report.md) |
| UBSan 正确性检查 | [UBSan 报告](archive/reports/2026-11-27-b3988f8/x86_64/ubsan/ci/20261127-153058-549791/report.md) | [UBSan 报告](archive/reports/2026-11-27-b3988f8/arm64/ubsan/ci/20261127-153056-394408/report.md) |

主报告按步骤链接原始日志，`summary.json` 记录构建参数与源码 SHA-256，架构目录下的 `runner/environment.log` 记录 CPU、内核和工具环境。专项报告回答具体测量问题，不能仅凭性能报告判断所有功能完成。

## 4. 可执行程序与实际输出

### 4.1 入口、六参数与返回值

[`probe_demo.c`](../Proj40/probe_demo.c) 创建目标子进程并在 `libprobe_targets.so` 的 `six`、`floating` 函数上安装入口与返回探针。以下摘自本轮 [x86_64 原始日志](archive/reports/2026-11-27-b3988f8/x86_64/full/ci/20261127-153059-814545/02-entry-return.log)：

```text
F2：six 六个整数参数 = [1, 2, 3, 4, 5, 6]，call_id=1
F3：six 返回值（RAX）= 183，call_id=1
入口：floating，call_id=2
F3：floating 返回值（XMM0）= 4.00，call_id=2
事件核验：six=1 入口/1 返回，floating=1 入口/1 返回，丢失=0，返回丢失=0，运行期 ptrace 停止=0
清理：six 原始入口 16 字节校验通过。
清理：floating 原始入口 16 字节校验通过。
清理：会话已分离。
F1–F3 演示：PASS
```

[ARM64 原始日志](archive/reports/2026-11-27-b3988f8/arm64/full/ci/20261127-153056-279603/02-entry-return.log) 得到相同参数与返回值，整数／浮点返回寄存器分别为 `X0`／`V0`。程序还打印 PID、符号地址、探针 ID 和入口字节：x86_64 校验 `JMP rel32`，ARM64 校验直接 `B`，存在入口 BTI 时保留该指令。地址等运行标识以每次实际输出为准。

六参数检查覆盖 ABI 的整数类寄存器，x86_64 为 `RDI/RSI/RDX/RCX/R8/R9`，ARM64 为 `X0–X5`。`floating(2.5)` 用于验证浮点**返回值**为 `4.0`，不能据此声称已采集浮点输入参数或任意混合参数。

共享库夹具预留函数入口补丁空间，便于稳定演示 ABI。完整模式另外直接探测目标进程 `libc.so.6` 的真实 `getpid`。探针触发使用用户态跳转；安装与管理使用 `ptrace`，`getpid` 本身仍执行系统调用。

### 4.2 生命周期、并发与进阶检查

| 程序 | 检查内容 | 主报告中的日志 |
|---|---|---|
| [`probe_lifecycle_validation`](../Proj40/probe_lifecycle_validation.c) | F4/F5：四个会话的入口恢复、映射回收、开关、在途返回与 seccomp 目标 | `03-lifecycle.log` |
| [`multiprobe_validation`](../Proj40/multiprobe_validation.c) | F6：16 个不同地址探针及同地址探针链 | `04-multiprobe.log` |
| [`thread_signal_validation`](../Proj40/thread_signal_validation.c) | F7：已有／新线程、主线程活跃／阻塞及信号交付 | `05-threads.log` |
| [`probe_abi_validation`](../Proj40/probe_abi_validation.c) | 寄存器／标志、返回值、递归、信号与缓冲区边界 | `07-abi.log` |
| [`aslr_pie_validation`](../Proj40/aslr_pie_validation.c) | 8 次独立 exec，PIE 与共享库地址变化、参数／返回及恢复 | `08-aslr.log` |
| [`probe_extensions_validation`](../Proj40/probe_extensions_validation.c) | A2 单参数过滤、A3 事件格式、A5 配置更新 | `09-advanced.log` |
| [`callback_validation`](../Proj40/callback_validation.c) | A4 目标进程内回调、状态恢复及 A5 回调配置更新 | `10-callback.log` |
| [`event_stream_demo`](../Proj40/event_stream_demo.c) | 用户态事件导出、TID 与入口／返回配对 | `10-event-stream.log` |

完整模式还运行指令解码、公共 API／ELF 契约与合流工具的离线检查。入口字节恢复与会话映射回收分别核验，前者不能单独证明 F4 的资源回收要求。

安装、开关、更新和清理要求应用建立安全点：阻止新增被探测调用，并等待在途调用结束。暂停所有线程不自动满足这个条件。A4 回调需短小、可重入、正常返回；A5 验证的是安全点配置更新。相关适用范围见 [能力边界与限制](limitations.md)。

## 5. A3 真实用户态与内核事件合流

完整流程通过 [`event_merge_demo.py`](../scripts/event_merge_demo.py) 同时采集真实 libc `getpid` 的 LUDP 入口／返回事件与 ftrace `syscalls:sys_enter_getpid` 事件。工具使用独立 tracefs instance 和 `mono` 时钟，将目标固定到允许的一个 CPU；LUDP 时间戳换算到同机、同次启动的 `CLOCK_MONOTONIC` 时间域。结束时释放实例。

本轮两架构均完成真实采集，数据见第 3 节的合流报告：

| 指标 | x86_64 | ARM64 |
|---|---:|---:|
| 核验调用数 | 100 | 100 |
| 用户态／内核事件数 | 200／100 | 200／100 |
| 内核事件超出对应入口／返回时间区间的最大距离 | 1,623 ns | 6,091 ns |
| 预设最大允许距离 | 100,000 ns | 100,000 ns |
| 结果 | PASS | PASS |

每份报告链接 `ludp.perf`、`kernel.ftrace`、`merged.perf` 和 `kernel-buffer-stats.txt`。检查覆盖事件计数、目标 TID、固定 CPU、各来源时间单调性、入口／返回配对及两路事件丢失；合流按纳秒整数排序。

这里的距离用于核验跨来源时间对齐，**不是探针执行开销**。内核文本时间精度为 1,000 ns，容差还包含用户态计数器校准误差，不能把接近或相同时间戳解释为严格因果顺序。本轮验证 ftrace syscall 合流，未验证所有 perf／kprobe 采集器、长期时钟漂移或任意线程迁移；输出为 perf-script 风格文本，不是 `perf.data` 二进制文件。离线解析检查不能代替这项真实采集。

## 6. 性能与完整清理如何复核

完整模式调用 [`benchmark.py`](../Proj40/benchmark.py)，默认轮换运行五种模式，每种模式每轮调用 `getpid` **1,000,000 次，共 5 轮**：

| 模式 | 含义 |
|---|---|
| `baseline` | 无探针、无 tracer 附加 |
| `inert` | 已附加但未插入探针，校验入口字节未改动 |
| `entry` | 用户态入口探针，每次调用一条事件 |
| `return` | 用户态入口与返回探针，每次调用两条事件 |
| `uprobe` | 内核 perf uprobe，核验命中计数与执行耗时 |

本轮结果如下，原始逐轮样本见对应 `benchmark/benchmark.json`：

| 指标 | x86_64 | ARM64 | 本次判定 |
|---|---:|---:|---|
| 入口额外开销中位数 | 60.264 ns | 46.091 ns | 均 < 1,000 ns |
| 入口＋返回额外开销中位数 | 124.975 ns | 145.095 ns | 均 < 1,000 ns |
| 内核 uprobe 额外开销中位数 | 4,266.449 ns | 359.253 ns | 各有 5 轮实测 |
| 最大插入耗时 | 1.590 ms | 2.315 ms | 均 < 10 ms |
| 最大移除耗时 | 0.862 ms | 0.728 ms | 均 < 10 ms |
| 最大完整清理耗时 | 2.464 ms | 1.653 ms | 均 < 10 ms |

额外开销先计算同轮探针模式与 `baseline` 的 `ns_per_call` 差值，再取五轮中位数；每个 `ns_per_call` 是该轮百万次调用的均值。因此这不是单次最坏值或 P99，也不自动适用于任意回调、长探针链或多线程吞吐负载。

插入计时从已附加且到达安全点后开始。完整清理从 `ludp_probe_remove` 调用前开始，覆盖入口恢复、恢复字节校验、`ludp_detach` 及最终会话映射回收；各样本均进入 `< 10 ms` 门禁。等待应用安全点和 attach 不在此区间，报告另列 attach／detach 分段耗时。旧版仅有分段耗时，未连续计量完整清理，不能直接与本轮完整清理作同口径比较。

本轮每架构入口模式共读取 500 万条用户事件，入口＋返回共 1,000 万条，用户事件丢失、返回记录丢失和运行期 `ptrace` 停止均为零。内核侧每轮通过 perf 计数器核验 100 万次命中，**未完整消费采样环**，因此不能把内核样本的 `drops=0` 当作完整日志无丢失证明，也不能把本对照表述为等价采集字段下的日志吞吐比较。

`inert` 的增量为 x86_64 `0.268 ns`、ARM64 `-0.073 ns`，差异属于测量噪声；入口字节不变支持“未插入探针不增加探针执行路径”，不证明严格零纳秒或条件过滤失败零开销。不同 CI 主机的 CPU、内核、负载会变化，应使用报告记录的环境评估结果，避免将跨主机差异全部归因于代码优化。

## 7. 文件位置与失败复核

| 位置 | 用途 |
|---|---|
| `scripts/run_demo.py`、`Proj40/` | 执行器、演示程序、验证程序和实现源码 |
| `artifacts/demo/<运行时间>/` | 每次本地演示的独立结果目录，由 Git 忽略 |
| `test/` | GitHub 结果同步与离线验证功能包 |
| `test/results/` | 仅保留最近成功同步的一轮 GitHub 结果，由 Git 忽略 |
| `docs/archive/reports/2026-11-27-b3988f8/` | 随仓库保存的固定原始证据，不随下次同步变化 |

单次 full 结果的主要文件：

```text
report.md                   功能、正确性与性能验证报告，含赛题覆盖表
summary.json                环境、构建参数、命令、退出码、提交与源码 SHA-256
01-build.log … 10-*.log      构建、演示和正确性回归的原始输出
11-benchmark.log            性能基准执行日志
12-event-merge.log          A3 真实合流执行日志
benchmark/                  getpid 基准报告、JSON 样本与原始输出
event-merge/                A3 报告、摘要、两路原始事件、合流结果与缓冲统计
```

权限不足时查看 `benchmark.log` 或 `event-merge/summary.json` 的具体原因；普通 full 可能标为 `SKIP`，严格模式会失败。功能或阈值失败时保留本次结果，从主报告定位对应日志并复现其中记录的命令，不通过删除失败样本或修改阈值取得通过结果。

执行器保留失败前已完成的日志；单步骤超时、SIGINT 或 SIGTERM 会终止该步骤进程组并写出失败报告。详细测试范围、返回模式与扩展寄存器等边界分别见 [测试与验收](testing.md) 和 [能力边界与限制](limitations.md)。

# 测试与量化验收规范

本规范用于复现 LUDP 的功能验证、正确性回归与性能测量。验收对象为 Linux x86_64 / System V AMD64 ABI 与 Linux ARM64 / AAPCS64 两套后端。A1 的验收条件是同一提交在两种原生环境中分别完成回归；提交 `3ffd736` 已取得两架构 full / UBSan 均通过的 [原生报告](archive/README.md)。下表中的“已有用例”表示仓库提供对应检查，不代表任意机器、任意提交已经通过；实际结论以该次运行的退出码、日志、环境和源码版本为准。

## 1. 功能与证据矩阵

| 赛题项 | 当前实现与可核验证据 | 验收入口 | 适用范围与边界 |
|---|---|---|---|
| F1 动态库入口探针 | 真实共享库符号安装；入口为 x86_64 `JMP rel32` 或 ARM64 `B imm26`，保留 ARM64 BTI；拒绝不支持入口时原字节不变；基准检查运行期 ptrace 停止数为 0 | [probe_abi_validation.c](../Proj40/probe_abi_validation.c)、[getpid_benchmark.c](../Proj40/getpid_benchmark.c) | 控制阶段使用 ptrace；不支持的入口指令应拒绝安装。`getpid` 原函数自身的系统调用不属于探针 trap 开销 |
| F2 函数参数 | 对比 `args[0..5]` 与六个不同的 64 位实参，含高位数据 | [probe_abi_validation.c](../Proj40/probe_abi_validation.c) | 对应 x86_64 RDI/RSI/RDX/RCX/R8/R9 或 ARM64 X0–X5 的整数类参数；不代表浮点/聚合类型的前六个源语言形参 |
| F3 返回值 | 校验 RAX/RDX/XMM0 或 X0/X1/V0；负整数、指针、128 位整数、浮点、递归和十参数透传；用 `call_id` 配对入口/返回 | [probe_abi_validation.c](../Proj40/probe_abi_validation.c) | 正常返回路径；不支持跨探测帧异常展开、`longjmp`、换栈、线程取消、`noreturn`、CET shadow stack 或 ARM64 GCS/PAC 返回协议 |
| F4 清理 | 四次附加/分离会话（含 seccomp 目标）；逐字节比对入口；检查会话拥有的远程映射消失；257 次槽位复用；在途返回时拒绝分离 | [probe_lifecycle_validation.c](../Proj40/probe_lifecycle_validation.c) | 单探针移除与完整会话分离分开计时；无泄漏结论限于用例核验的映射和生命周期 |
| F5 动态开关 | 开启、关闭、再开启后分别核对事件数和业务返回值；重复开关幂等 | [probe_lifecycle_validation.c](../Proj40/probe_lifecycle_validation.c) | 在应用协调的安全点修改代码；关闭后旧事件仍可被消费，在途返回仍可完成 |
| F6 多探针 | 16 个不同函数地址与第 17 个同地址入口探针共存；逐 ID 核验；另验证 80 项同地址链 | [multiprobe_validation.c](../Proj40/multiprobe_validation.c) | 注册表上限 256；同地址叠加只支持入口模式，返回探针有互斥约束 |
| F7 多线程 | 运行后创建四线程；附加已有四线程；主线程活跃/futex 阻塞；线程退出回收；64 次信号与暂停竞争 | [probe_abi_validation.c](../Proj40/probe_abi_validation.c)、[thread_signal_validation.c](../Proj40/thread_signal_validation.c) | 目标进程可多线程；同一 `ludp_ctx` 使用单控制线程和单事件消费者；全线程暂停不替代应用安全点 |
| A1 多架构 | x86_64、ARM64 后端及对应 ABI/指令用例已提供；`3ffd736` 原生双架构 full / UBSan 均通过 | [CI 矩阵](../.github/workflows/ci.yml)、[ARM64 验证说明](arm64.md) | 交叉编译和模拟器仅作开发检查；两种原生 runner 的同提交结果共同构成验收证据 |
| A2 条件探针 | EQ、NE、MASK_EQ、LT、LE、GT、GE 与第六个参数过滤；按预期参数集合核对事件 | [probe_extensions_validation.c](../Proj40/probe_extensions_validation.c) | 单参数、单条件、无符号 64 位比较；不自动解引用字符串或指针 |
| A3 事件格式与合流 | TSC/CNTVCT 换算到 `CLOCK_MONOTONIC`；真实用户态事件与 ftrace getpid 事件合并，检查计数、TID、CPU 和时间关系 | [event_merge_demo.py](../scripts/event_merge_demo.py)、[merge_events.py](../scripts/merge_events.py) | 使用独立 tracefs instance 和单 CPU 短会话；真实采集与离线解析检查分开，新版双架构结论待 CI |
| A4 目标进程内调用 | 回调上下文、过滤、热更新、非法地址回滚、活跃回调 `EBUSY`；主动破坏 GPR/浮点状态后核对业务结果；x86_64 检查 AVX 高半区，ARM64 检查 Q0–Q31 高半区及 FPCR/FPSR | [callback_validation.c](../Proj40/callback_validation.c)、[avx_state_sentinel.S](../Proj40/avx_state_sentinel.S)、[simd_state_aarch64.S](../Proj40/simd_state_aarch64.S) | 回调须短小、非阻塞、正常返回；不保存 AMX/SVE/SME；AVX 子用例受硬件条件限制，ARM64 固定宽度 SIMD 子用例为必测 |
| A5 热更新 | 保持 probe ID；模式/过滤/启停/回调更新；版本号递增与幂等；非法配置不改变旧配置 | [probe_extensions_validation.c](../Proj40/probe_extensions_validation.c)、[callback_validation.c](../Proj40/callback_validation.c) | 应用安全点更新；用例覆盖配置拒绝与状态保持，不等价全部系统调用失败位置的故障注入 |

## 2. 正确性检查

默认 `make check` 运行以下 9 个 C 验证程序及 `probe_demo`、`event_stream_demo --standalone`，任一程序返回非零即停止验收。Python 报告门禁与事件合流工具的离线检查另列于第 5 节；它们不能替代原生 C/汇编执行或真实内核事件采集。

| 程序 | 核心检查 | 可观测判据 |
|---|---|---|
| `instruction_validation` | x86_64 解码/寻址/短分支；ARM64 ADR/ADRP 展开、分支与 literal 重定位、范围边界、截断及不支持编码拒绝 | 对应架构 decoder/relocation 输出 PASS |
| `probe_abi_validation` | F1–F3、ABI 现场、递归、信号处理器重入、缓冲区溢出及恢复、16 轮附加/分离 | 三组模式通过，溢出/恢复和附加/分离检查通过 |
| `probe_lifecycle_validation` | F4/F5、原始入口、映射回收、忙时保护和槽位复用 | 四次会话通过，含禁止 madvise 的目标 |
| `multiprobe_validation` | ≥16 个地址共存、同地址链重建、80 项链完整命中 | `F6 multi-probe: PASS` |
| `probe_extensions_validation` | A2 过滤、A3 格式及时间范围、A5 配置更新；零溢出、零返回记录遗漏 | `A2/A3/A5 advanced suite: PASS`；该项不采集真实 ftrace 数据 |
| `callback_validation` | 回调参数、现场恢复、更新回滚和活跃状态保护 | `A4 in-process call suite: PASS` |
| `thread_signal_validation` | 已有线程附加、futex 阻塞、退出清理和信号转发 | 两种线程状态及信号竞争检查均通过 |
| `api_contract_validation` | 公共 API 空指针/非法参数/`errno` 契约；主程序符号解析 | `public API/ELF contract: PASS` |
| `aslr_pie_validation` | 8 次独立 exec，校验 PIE/动态库符号、实参、返回和入口恢复 | 两种映射地址均变化，全部调用通过 |
| `probe_demo` | 动态库入口、六参数、整数与浮点返回的可读演示 | 演示程序退出码为 0，输出对应数值 |
| `event_stream_demo --standalone` | 固定 CPU 的真实 getpid 用户态事件导出，8 次调用 | 16 条入口/返回事件，调用配对、TID、返回值正确且无丢失；不代表已采集内核事件 |

ABI 哨兵按架构选择：[abi_state_sentinel.S](../Proj40/abi_state_sentinel.S) 对比 x86_64 通用寄存器、RFLAGS、x87/SSE 及 red zone；[abi_state_aarch64.S](../Proj40/abi_state_aarch64.S) 对比 ARM64 X0–X30、SP、NZCV、FPCR/FPSR 与 Q0–Q31 共 800 字节快照。十参数函数同时覆盖 ARM64 的 X6/X7 和第 9、10 个栈参数。A4 另验证回调扰动后的向量现场。AVX-512 没有独立的寄存器哨兵回归，不能由 AVX 子用例推导其全部状态已实测。`probe_abi_validation` 中的 `tiny_target` 为负向入口用例：x86_64 使用过短 RET，ARM64 使用不支持的 SVC；拒绝信息应结合套件最终退出码判断。ARM64 普通 RET 本身可搬迁。

`callback_validation` 单独打印 AVX 子项的 PASS/FAIL，CPU/OS 不支持 AVX 时明确打印 SKIP。套件成功不消除该硬件条件下的跳过项。ARM64 分支使用固定宽度 SIMD 验证，必须打印对应 PASS，不能用 AVX 不可用代替 ARM64 的现场恢复检查。

ASLR/PIE 增加 [aslr_pie_validation.c](../Proj40/aslr_pie_validation.c)：8 次独立 `exec`，分别按目标的实际布局解析主程序和动态库符号，并检查两个映射的装载地址确实发生变化；每轮核验参数、返回和入口恢复。该用例避免仅依赖 fork 继承布局，未覆盖所有 ELF 变体或会话中途 exec 迁移。UBSan 可发现已执行 C 路径中的部分未定义行为，不覆盖汇编语义或所有并发时序。

## 3. 量化指标与统计口径

基准实现为 [getpid_benchmark.c](../Proj40/getpid_benchmark.c)，汇总与门禁实现为 [benchmark.py](../Proj40/benchmark.py)。标准配置为每种模式 5 轮、每轮 1,000,000 次 `getpid`；每个子进程计时前预热 10,000 次，各轮轮换模式顺序。基准用 `CLOCK_MONOTONIC` 记录整段循环总耗时，计时后读取事件。

| 模式 | 目标进程状态 | 每轮预期事件数 |
|---|---|---:|
| `baseline` | 不附加 tracer | 0 |
| `inert` | 附加 tracer、不安装探针，入口字节保持不变 | 0 |
| `entry` | 用户态入口探针 | 1,000,000 |
| `return` | 用户态入口与返回探针 | 2,000,000 |
| `uprobe` | 内核 uprobe PMU，采样 IP/TID 并读取命中计数 | 命中计数 1,000,000 |

单轮单位成本与成对增量按下式计算：

```text
ns_per_call(mode, round) = total_ns(mode, round) / iterations
overhead(mode, round) = ns_per_call(mode, round) - ns_per_call(baseline, round)
paired_overhead_ns(mode) = median(overhead(mode, round))
```

报告中的增量是“同轮相减后取中位数”，不必等于两个模式中位数之差。`return` 的成本对应一次函数调用包含入口和返回两个事件，不能直接标成单条事件成本。

| 赛题指标 | 当前量化方式 | 判定与解释 |
|---|---|---|
| 单次探针开销 `<1000 ns` | `entry`、`return` 的 `paired_overhead_ns` | 两者分别严格小于 1000；未量化带 A2 过滤、A4 任意回调或大量同地址链的开销 |
| 插入延迟 `<10 ms` | 每次 `ludp_probe_add_ex` 的 `insert_ms` | 所有 `entry`、`return` 样本的最大值严格小于 10；不含 `attach_ms` |
| 清理延迟 `<10 ms` | `cleanup_ms` 从开始 remove 到 detach 完成，含入口恢复校验 | 所有 entry / return 样本完整清理最大值严格小于 10；同时保留 `remove_ms` 与 `detach_ms`，不把会话释放排除在门禁外 |
| 无探针触发时零开销 | `inert` 检查入口字节不变并报告与基线的差值；F5 检查禁用后恢复原字节 | 证明没有探针跳转路径；不能要求含调度噪声的实测差值恰好为 0，也不表示控制进程和已分配内存无资源占用 |
| 运行结果正确 | 返回值累计和、事件数、返回值、入口恢复、退出状态 | 样本 `status=PASS`；用户态模式要求 `drops=0`、`missed=0`、`stops=0` |
| F4 无远程映射残留 | 独立 `probe_lifecycle_validation` 检查会话拥有的地址区间 | 不以移除耗时替代映射检查，也不据此宣称所有错误路径或无限时长运行均无泄漏 |

新版“性能门禁通过”要求 getpid 入口/入口加返回增量、插入、移除及完整清理同时达标。`cleanup_ms` 是连续测量值，包含 remove、字节恢复校验和 detach，不以两个分段最大值相加代替。attach 和等待应用安全点仍单独说明。旧版 `3ffd736` 的门禁未包含完整清理，不能直接与新版 PASS 等同。

事件环默认容量为 `2^22` 条，远程布局为每槽 128 字节事件与 8 字节就绪标记，合计约 544 MiB 虚拟地址空间，另有头部和代码区；按需触页决定实际驻留内存。百万次返回基准计时结束后才消费事件，至少需容纳 2,000,000 条事件。不要沿用短演示的低容量 `LUDP_RING_CAP` 配置，否则应按溢出失败处理。事件区首次触页成本包含在本基准时间中。

当前实现在所有暂停线程均确认未启用 seccomp 时，对至少 2 MiB 的事件环请求 `MADV_HUGEPAGE`，以降低首次触页及最终页表回收成本。内核可忽略或拒绝该建议；事件容量与正确性要求保持不变。`LUDP_HUGEPAGES=0` 可关闭建议，便于在同机复测普通页路径。启用 seccomp 或无法读取策略时跳过建议；独立受控用例禁止 madvise 并核验目标正常退出。基准记录该环境变量和透明大页策略，不能仅凭配置推断实际使用了大页，也不能将不支持大页解释为允许放宽清理阈值。

## 4. 标准三组对照及现有缺口

赛题要求的“无探针 / 内核 uprobe / 用户态探针”对应 `baseline`、`uprobe` 与 `entry`；`return` 是额外的入口加返回测量。当前程序已提供三组执行入口，但内核 PMU 设备、内核配置或权限不可用时会记录 `SKIP uprobe: ...`。

普通运行允许内核组跳过，仍可判定用户态路径与基础阈值；只有加上 `--require-kernel` 且取得全部 5 轮内核样本，才完成本基准定义的三组对照。当前固定归档的两架构内核组均为 `SKIP`，不能用赛题给出的 1500–3000 ns 参考范围填入实测表，也不能据此给出相对内核 uprobe 的加速比。

两条采集路径的工作量仍有差异：用户态组保存参数/返回信息并核验全部事件；内核组采样 IP/TID，perf ring 可能溢出，但通过计数器核验命中总数。该对照展示当前实现配置的函数调用成本，不等价相同事件字段、相同输出吞吐和零丢包下的严格比较。正式报告应保留此差异及内核权限环境。

A3 分为三个层次：`probe_extensions_validation` 检查格式与约 ±1 秒的时间范围；`merge_events_validation.py` 使用离线样本检查解析、纳秒整数排序、配对和失败处理；`event_merge_demo.py` 才采集同一目标的真实用户态与 ftrace `sys_enter_getpid` 事件。第三项默认固定单 CPU、调用 100 次，要求 100 条入口、100 条返回和 100 条内核事件，核验 TID、CPU、唯一 call_id、时间单调性及两路无丢失。默认允许内核事件超出对应入口/返回区间最多 100,000 ns，保留实际最大偏差；这不是探针执行开销阈值。

真实合流使用独立 tracefs instance 的 `mono` 时钟，结束后关闭并删除该实例。验证覆盖同机同次启动的短会话，不证明长期漂移上界、任意 CPU 迁移或所有 perf/kprobe 采集器兼容。`3ffd736` 归档没有这项原生采集，新版需在两架构 CI 中重新取得证据。

## 5. 复现命令

环境要求：原生 Linux x86_64 或 ARM64、GCC、GNU make、glibc、Python 3.9 及以上，并允许父进程跟踪其子进程。以下命令从仓库根目录执行。先完成正确性检查，再采集性能；本地验收写入 `artifacts/` 的独立目录；`test/` 功能包将下载的 GitHub 结果写入 `test/results/` 并替换旧一轮，二者用途不同。

### 5.1 构建与正确性回归

```bash
make -C Proj40 clean
make -C Proj40 -j2 CFLAGS='-O2 -g -Wall -Wextra -Werror -fno-strict-aliasing -pthread'
make -C Proj40 check CFLAGS='-O2 -g -Wall -Wextra -Werror -fno-strict-aliasing -pthread'
```

Makefile 用 `$(CC) -dumpmachine` 选择架构。跨架构交叉编译只验证构建，不能直接在 x86_64 上执行 ARM64 产物作为原生验收；切换编译器架构或 sanitizer 配置前需清理产物。单项复核可在 `Proj40/` 中执行上述验证程序。演示使用较小事件环时，应与百万次基准配置分开；正式基准可用 `env -u LUDP_RING_CAP` 恢复默认容量。

需要完整分步日志但不测性能时：

```bash
python3 scripts/run_demo.py --correctness-only
```

该模式运行全部 9 个 C 验证程序、两个演示程序和事件合流工具的离线检查，报告中的性能与真实合流项明确标为 SKIP。快速演示只运行部分功能，不能代替完整正确性回归。

### 5.2 重新生成当前量化报告

```bash
env -u LUDP_RING_CAP python3 Proj40/evaluate.py \
  --rounds 5 --iterations 1000000 --output artifacts/validation
```

`evaluate.py` 会构建、运行完整 `make check`，再执行基准。输出包括 `tests.log`、`benchmark.log`、`benchmark.json` 和 `benchmark.md`。`benchmark.json` 保留环境、每轮样本、跳过原因、成对增量、完整清理最大值和分项 `checks`；`kernel_comparison_complete` 表示内核对照样本完整，`passed` 表示当前运行配置的全部门禁通过。Markdown 用于阅读，日志用于复核原始输出。基准异常结束时应先阅读日志，不能将缺失样本视作 0。

具备内核 uprobe 条件的正式三组验收：

```bash
env -u LUDP_RING_CAP python3 Proj40/evaluate.py \
  --rounds 5 --iterations 1000000 --require-kernel \
  --output artifacts/submission
```

`--require-kernel` 在内核样本缺失时返回失败。不要为了得到绿色结论而删除该参数；环境不具备条件时保留 `SKIP` 并更换已配置的 Linux 验收机。

### 5.3 UBSan 回归

```bash
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
CFLAGS='-O1 -g -Wall -Wextra -Werror -fno-strict-aliasing -fno-omit-frame-pointer -fsanitize=undefined -fno-sanitize-recover=all -pthread' \
LDFLAGS='-pthread -fsanitize=undefined -fno-sanitize-recover=all' \
python3 scripts/run_demo.py --correctness-only
```

`run_demo.py` 强制重新构建并保留实际构建参数。手工运行 Makefile 时，编译选项变化不会自动使目标过期；性能基准前应清理并构建正常优化版本。

### 5.4 版本对照与本机调优

```bash
env -u LUDP_RING_CAP python3 Proj40/evaluate.py \
  --rounds 5 --iterations 1000000 --output artifacts/comparison \
  --baseline-json /path/to/previous/benchmark.json

env -u LUDP_RING_CAP python3 Proj40/auto_tune.py \
  --rounds 5 --iterations 1000000 --output artifacts/tuning
```

版本比较要求源码、机器、编译选项、负载和环境可追溯。`auto_tune.py` 比较 `LUDP_BULK_IO=0/1` 的远程内存复制路径（ARM64 的代码写入始终经指令缓存同步路径），以通过正确性和阈值的候选中返回探针插入中位数作为选择依据；差距小于 0.2 ms 时优先批量复制路径。该参数影响控制侧复制，不改变探针命中汇编路径。推荐值仅适用于当次环境；`--apply` 可写入本地配置文件，使用前应保留测量依据。

版本对照输出 `version_comparison.md`；复制路径调优输出 `memory_io_tuning.md` 和 `memory_io_tuning.json`。旧版基准缺少 `cleanup_ms` 时显示未测，不能按零耗时或新版完整清理通过处理。

### 5.5 报告门禁与 A3 专项验证

以下两项不访问 GitHub，也不需要内核追踪权限：

```bash
python3 Proj40/benchmark_validation.py
python3 scripts/merge_events_validation.py
```

前者检查缺失/重复样本、异常数值、事件计数、严格内核要求和完整清理门禁；后者检查事件解析、时间排序、来源信息及合流错误处理。真实 A3 采集需内核暴露 tracefs instance、`mono` 时钟和 `syscalls:sys_enter_getpid`，并具备相应访问权限：

```bash
python3 scripts/event_merge_demo.py --require-kernel
```

该命令读取真实采集数据，自动生成独立结果目录；权限或设施缺失时失败并记录原因。GitHub full 作业已准备权限和 tracefs，统一执行 `run_demo.py --full --require-kernel --require-merge`，同时要求三组性能对照和 A3 实测；本地无需为了离线检查修改系统追踪策略。

## 6. 报告归档与结果解释

[验证报告归档](archive/README.md) 保存提交 `3ffd736` 在 GitHub 原生 x86_64 和 ARM64 上的完整报告、环境、源码指纹、原始日志及性能样本。四个 full / UBSan 作业均通过，内核 uprobe 因权限不足为 `SKIP`，完整 detach 超过 10 ms；这些边界与原始数据一并保留。

已被该双架构证据替代的早期开发快照从当前目录移除，仍可通过 Git 历史追溯。`docs/archive/` 中的报告随仓库提交，适用于记录中的版本；本机新结果存入 `artifacts/`，GitHub 下载结果使用第 7 节的滚动 `test/results/` 目录。同步最新结果不会改写固定归档。

每份正式验收材料应附源码提交 ID、工作树是否有修改、实际构建命令、机器与内核信息、CPU/亲和性、迭代数、轮数、原始日志和所有跳过项。现有 `benchmark.json` 自动记录日期、平台、CPU、亲和性与采样配置，但不自动记录 Git 提交和编译器版本，应在提交说明中补充。性能统计反映该次环境，复测时不要选择性删除慢样本或用不同机器的单次差值宣称优化收益。

## 7. GitHub 原生双架构回归与最新结果

工作流 [.github/workflows/ci.yml](../.github/workflows/ci.yml) 在 push、pull request 和手工触发时运行四个独立作业：

| 架构 | Runner | full | ubsan |
| --- | --- | --- | --- |
| x86_64 | `ubuntu-24.04` | 全部正确性、演示、5 轮百万次基准及真实 A3 合流 | 全部正确性与演示，不测性能及真实合流 |
| ARM64 | `ubuntu-24.04-arm` | 全部正确性、演示、5 轮百万次基准及真实 A3 合流 | 全部正确性与演示，不测性能及真实合流 |

ARM64 标签对应原生 ARM64 虚拟机，工作流另外检查 `uname -m`，避免将交叉编译误记为原生运行。标签说明见 [GitHub 官方 runner 参考](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)。`fail-fast: false` 保留另一架构的诊断机会；每个作业无论成功与否均上传已有环境记录、分步报告和日志。只有两架构的相关用例均通过，才能对该提交作出对应的 A1 验收结论。

推送后，在仓库根目录下载最新一轮：

```bash
python3 -m test sync --wait
```

也可明确指定运行编号：

```bash
python3 -m test sync --run-id RUN_ID --wait
```

下载使用本机已有 `gh` 登录，或 `GH_TOKEN` / `GITHUB_TOKEN` 环境凭据，需具有仓库 Actions 读取权限。默认选择当前分支最新的 `ci.yml` 运行；指定分支使用 `--branch BRANCH`，与 `test` 功能包名无关。查看 `test/results/report.md` 获取作业结论，`test/results/latest.json` 记录 run ID、attempt、提交、分支、产物清单和缺失原因。文件布局为：

```text
test/
├── README.md
├── __init__.py
├── __main__.py                 # sync / validate 命令入口
├── sync_ci_results.py
├── ci_results_validation.py
└── results/                    # 下载后生成，仅此目录滚动替换
    ├── report.md
    ├── latest.json
    ├── x86_64/
    │   ├── full/               # ci/<时间>/报告、日志及 runner 环境
    │   └── ubsan/
    ├── arm64/
    │   ├── full/
    │   └── ubsan/
    └── failed-jobs/            # 失败作业日志，存在时保存
```

每次同步先下载到临时目录，完成后仅替换 `test/results/` 上一轮结果；网络或下载失败保留已有结果。失败运行也保存，缺失产物不会混用旧文件，部分重跑按当前 attempt 明确标记未生成的产物。生成的结果不提交 Git，避免结果文件再次触发 CI；`test/` 下的包代码和说明随源码维护。本地独立运行仍保存在 `artifacts/`，固定报告归档不受同步影响。

同步工具的离线回归独立于项目 C/汇编功能验证，可运行 `python3 -m test validate`。它使用临时结果目录检查请求重试、产物处理和替换行为，不访问 GitHub，也不改写已有下载结果。完整使用说明见 [test 功能包](../test/README.md)。

共享 runner 适合功能回归和明显性能退化定位，调度与频率变化可能影响最大延迟；正式指标应在指定验收机复测。新版 CI full 使用 `--require-kernel --require-merge`，同时要求真实内核对照和 A3 合流；两项缺失都会失败。权限通过临时 runner 的 sudo 执行获取，不要求修改开发机全局策略。Actions 产物有保留期限，正式提交时需归档与被测版本一致的结果。

## 8. 报告名称与阅读顺序

报告按内容命名，路径保持稳定，便于同步和自动处理：

| 路径 | 报告标题 | 内容 |
|---|---|---|
| `test/results/report.md` | LUDP 双架构 CI 验证汇总报告 | 同提交四个作业状态和报告入口 |
| `ci/<时间>/report.md` | LUDP 功能、正确性与性能验证报告 | F1–F7、A1–A5 覆盖表、执行日志、性能及合流结果 |
| `benchmark/benchmark.md` | getpid 探针性能基准报告 | 五种模式的耗时、完整清理、计数和量化门禁 |
| `event-merge/report.md` | A3 用户态与内核事件合流验证报告 | 真实采集配置、时间关系、合流文件与通过条件 |
| `version_comparison.md` | getpid 探针性能版本对照报告 | 可选的跨版本指标对照，明确旧报告缺失项 |
| `memory_io_tuning.md` | 远程内存复制路径调优报告 | 可选的本机复制策略对比与配置建议 |

单架构报告的 A1 标记为需跨架构汇总，不能由单机结果判定多架构完成。性能基准只度量 getpid 工作负载，不代表所有进阶功能组合的性能。历史归档保留原始标题与内容以维持 SHA-256 证据一致，新标题从下一次运行生效。

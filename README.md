# LUDP：轻量级用户态动态探针

Lightweight Dynamic Probe for User Space

> 2026 年全国大学生计算机系统能力大赛 · 操作系统设计赛 · 西北区域赛
> 赛题类型：工程型；难度：A；实现平台：Linux x86-64 / System V AMD64 ABI 与 ARM64 / AAPCS64。

参赛队伍：**在线求偶等她入队**。成员：刘志钰、迮泳瑞、张向前。

本文按设计赛道文档大纲组织。[演示指南](docs/demo.md) 提供运行步骤与输出说明，[验证报告归档](docs/archive/README.md) 提供当前双架构实测证据，技术细节见 [文档索引](docs/README.md)。

## 1. 目标描述

LUDP 利用 `ptrace` 在目标进程中部署探针代码，将函数入口改写为用户态跳转，实现动态库函数探测、参数采集、返回值捕获与探针管理。内置采集路径在命中时不通过 `int3`/trap 进入内核；安装、线程协调和回收发生在控制阶段。

项目交付 C 静态库、可运行示例、功能验证、百万次 `getpid` 基准及自动生成的验证报告。提交 `b3988f8` 已通过 x86_64 与 ARM64 原生 full / UBSan 四个作业，F1–F7、A1–A5 在声明的支持范围内均有验证证据；完整模式包含内核 uprobe 对照、A3 真实 ftrace 合流、独立 exec/ASLR 检查及完整清理计时。结果与边界见第 6 节和 [测试规范](docs/testing.md)。

| 目标 | 验收方式 |
| --- | --- |
| F1–F7 基础功能 | 专项测试核验入口跳转、六参数、返回值、清理、开关、多探针及多线程 |
| 单次额外开销 `<1000 ns` | entry、entry+return 分别与同轮无探针基线相减，取增量中位数 |
| 插入 / 清理延迟 `<10 ms` | 每轮插入和完整清理最大耗时；完整清理覆盖 remove、入口校验和 detach |
| 无探针触发时零开销 | 未安装补丁时不增加函数入口执行路径；测量差值保留调度噪声 |
| 文档完整与演示可复现 | 一键执行、逐步日志、明确判定、环境与源码身份、原始数据 |

上述检查在本次原生双架构完整验证中均通过，结论限定于被测提交、运行环境与已执行用例。[测试规范](docs/testing.md) 说明统计方法和覆盖范围；历史报告保留原版本口径。

## 2. 比赛题目分析和相关资料调研

赛题由龙蜥社区、阿里云计算有限公司维护，要求降低传统内核 uprobe 命中时的上下文切换成本，并增强用户态探针表达能力。系统需要同时解决指令重定位、ABI 现场保持、动态链接与 ASLR、返回地址管理、多线程和资源回收等问题。

| 资料 | 对本项目的参考价值 |
| --- | --- |
| [Linux Uprobe tracer](https://docs.kernel.org/trace/uprobetracer.html) | 理解内核用户态探针及对照测量对象 |
| [Linux Kprobes](https://docs.kernel.org/trace/kprobes.html) | 理解入口/返回探针、指令处理与运行约束 |
| [ptrace(2)](https://man7.org/linux/man-pages/man2/ptrace.2.html) | 进程附加、停止状态、寄存器与线程事件控制 |
| [Libcare](https://github.com/cloudlinux/libcare) | 运行中用户态代码补丁的工程思路 |
| [Frida](https://frida.re/) | 动态插桩与目标进程内执行逻辑 |
| [bpftime](https://eunomia.dev/zh/blogs/bpftime/) | 用户态可观测性和低开销采集思路 |

本项目选择“控制阶段 ptrace + 命中阶段用户态 trampoline”的方案。文中参考资料用于说明设计依据，不构成对其他项目的性能优劣结论。赛题的 1500–3000 ns uprobe 参考范围不作为本项目实测数据。

## 3. 系统框架设计

```mermaid
flowchart LR
    A[控制程序] -->|ptrace 安装与管理| B[目标进程函数入口]
    B -->|x86 JMP / ARM B| C[用户态 trampoline]
    C --> D[条件过滤与可选回调]
    D --> E[事件缓冲区]
    D --> F[重定位指令与原函数]
    F --> G[可选 return stub]
    G --> E
    E -->|批量读取| A
```

| 模块 | 实现文件 | 职责 |
| --- | --- | --- |
| 进程控制与事件消费 | `ludp.c` | 线程协调、远程内存访问、资源管理及事件读取 |
| 探针管理 | `probes.c` | 注册表、同地址入口链、开关和配置更新 |
| 符号与指令处理 | `elf.c`、`x86dec.c`、`aarch64.c` | ELF/ASLR 定位、入口解码和重定位 |
| 目标进程执行路径 | `trampoline.S`、`aarch64_trampoline.S` | 参数采集、返回探针、现场保护和回调 |
| 公共接口 | `ludp.h` | C API、事件及探针配置结构 |
| 演示与评测 | `scripts/run_demo.py`、`benchmark.py` | 顺序执行、结果校验及证据保存 |

x86-64 入口使用 5 字节 `JMP rel32`，ARM64 使用 4 字节 `B`；被覆盖指令按架构规则重定位。返回探针通过每次调用独立的 return stub 采集 `RAX/RDX/XMM0` 或 `X0/X1/V0`，以 `call_id` 配对。事件缓冲支持多生产者、单消费者，溢出与返回记录不足均显式计数。

安装、开关、更新和清理要求应用协调的安全点；单个 `ludp_ctx` 只支持一个控制线程和一个事件消费者。返回探针不支持跨帧异常展开、`longjmp`、换栈、CET shadow stack 或 ARM GCS；A4 不保存 AMX tile、SVE/SME 扩展状态。完整说明见 [系统设计](docs/architecture.md)、[API](docs/api.md) 和 [支持范围与限制](docs/limitations.md)。

## 4. 开发计划

以下按工程交付阶段整理当前状态，并非补写历史上未留存的原始排期。

| 阶段 | 交付目标 | 当前状态与出口条件 |
| --- | --- | --- |
| 基础路径 | F1–F3、指令解码、参数与返回值 | 已有实现；通过对应集成与 ABI 用例 |
| 生命周期与并发 | F4–F7、资源回收、信号与线程协调 | 已有实现；验证入口恢复、映射回收和多探针命中 |
| 进阶与加固 | A2–A5、API/ELF/向量状态加固 | 专项验证通过；A3 已完成双架构真实事件合流 |
| 文档与演示 | 正式文档、一键 demo、量化证据 | 提供运行指南、逐项判定及固定报告归档 |
| 提交验证 | 完整三组对照、真实合流及原始报告 | `b3988f8` 两个 full 作业通过严格检查，包含完整清理 `<10 ms` |
| 多架构扩展 | A1/ARM64 后端与独立验收 | 同一提交的双架构原生 full / UBSan 四个作业均通过 |

## 5. 比赛过程中的重要进展

下表来自仓库 Git 提交记录；日期是记录中的提交日期，不推断未留存的比赛活动。

| 日期 | 进展 | 提交依据 |
| --- | --- | --- |
| 2026-11-21 | 完善 F1–F3 基础功能 | `ff0e8b6` |
| 2026-11-24 | 加入清理与运行期开关 | `3804e1c` |
| 2026-11-25 | 整合 F1–F7，增加正确性与性能验收 | `575dc0b`、`19eb240` |
| 2026-11-25 | 增加 A2/A3/A5 及专项测试 | `07ee58d`、`231fffb` |
| 2026-11-25 | 增加 A4 目标进程回调及现场恢复 | `e0df139`、`5ca63df` |
| 2026-11-25 | 加固线程事务、ELF、向量状态，增加 UBSan | `36538a4`、`8a5343c`、`f04615e` |
| 2026-11-27 | 完成文档、演示与 A1 双架构原生验证 | `3ffd736`；Actions 运行 `45946335169` |
| 2026-11-27 | 优化完整清理，补齐三组性能对照、真实事件合流与独立 exec/ASLR 验证 | `b3988f8`；Actions 运行 `45958959682` |

关键改进的原因和验证依据见 [工程改进记录](docs/engineering.md)。双架构原始结果固定保存在 [验证报告归档](docs/archive/README.md)，每份结论均对应其记录的提交和运行环境。

## 6. 系统测试情况

### 6.1 功能与正确性

下表列出能力与对应程序。提交 `b3988f8` 的双架构原生结果已验证这些项目；A1 由同一提交的两种架构结果联合确认。

| 赛题项 | 当前能力 | 验证程序 |
| --- | --- | --- |
| F1 / F2 / F3 | 动态库入口跳转、六个 INTEGER 参数、返回寄存器及调用配对 | `probe_demo`、`probe_abi_validation` |
| F4 / F5 | 入口恢复、映射回收、在途调用保护、动态开关 | `probe_lifecycle_validation` |
| F6 | ≥16 个不同地址；80 项同地址入口链压力用例 | `multiprobe_validation` |
| F7 | 多线程跟踪、并发采集、信号转发；变更要求安全点 | `thread_signal_validation`、`probe_abi_validation` |
| A1 | x86_64 / ARM64 原生验证通过 | 双架构 CI：同一套完整验证、UBSan 与性能基准 |
| A2 / A5 | 条件过滤、安全点配置更新及错误配置拒绝 | `probe_extensions_validation` |
| A3 | CLOCK_MONOTONIC 时间换算、TID 映射和真实 ftrace 合流通过 | `event_stream_demo`、`event_merge_demo.py` |
| A4 | 目标进程回调和上下文恢复 | `callback_validation` |
| 通用正确性 | 指令解码、API/ELF、独立 exec/ASLR、UBSan | `instruction_validation`、`api_contract_validation`、`aslr_pie_validation`、CI |

### 6.2 构建和一键演示

环境要求：Linux x86-64 或 ARM64、GCC、GNU make、glibc、Python 3.9+（含时区数据），允许父进程 `ptrace` 自己启动的子进程；Python 无第三方包依赖。默认事件环约占 544 MiB 虚拟空间，基准需要足够内存，见 [测试规范](docs/testing.md)。

在仓库根目录执行：

```bash
# 基础演示：自动构建，展示 F1–F3，并验证 F4–F7
python3 scripts/run_demo.py

# 完整演示：正确性、进阶功能、五轮百万次 getpid 基准
python3 scripts/run_demo.py --full

# 严格验收：内核 uprobe 对照、A3 真实合流均不可缺失
python3 scripts/run_demo.py --full --require-kernel --require-merge
```

每次演示在 `artifacts/demo/` 下生成独立目录，包含步骤日志、`summary.json` 和功能、正确性与性能验证报告；完整模式另保存 getpid 性能基准和 A3 事件合流专项报告。主报告逐项列出 F1–F7、A1–A5。F1–F3 展示真实入口字节、参数 `[1,2,3,4,5,6]`、整数返回值 `183`、浮点返回值 `4.00` 及事件配对。失败返回非零退出码，跳过项明确为 `SKIP`。

[演示指南](docs/demo.md) 提供运行步骤、输出解释和判定条件。GitHub 登录配置及常用下载命令集中于 [test 功能包说明](test/README.md)。最新结果保存在 `test/results/`，下一次同步仅替换该结果目录；已归档报告可直接阅读，无需重新运行 demo 或下载。

### 6.3 独立验收与性能口径

```bash
make -C Proj40 -j4
make -C Proj40 check
python3 Proj40/evaluate.py --rounds 5 --iterations 1000000 \
  --output artifacts/validation
```

基准逐轮测量 baseline、inert、entry、entry+return 和可用的内核 uprobe。开销按“同轮相减再取中位数”计算；entry+return 按一次函数调用计，包含两条事件。报告保留最大插入/移除、attach/detach、完整清理、丢失计数和原始样本。

普通模式的内核组不可用时保留 `SKIP`，不补写参考数值或加速比。完整清理 `<10 ms` 已纳入门禁，覆盖从 remove 开始、入口字节校验到 detach 返回的连续区间。CI full 在临时 Runner 中以所需权限执行严格模式，内核对照或真实合流缺失均失败。测量不覆盖所有线程规模或任意 A4 回调开销，详见 [测试与性能评估](docs/testing.md)；CI 配置见 [ci.yml](.github/workflows/ci.yml)。

### 6.4 2026-11-27 双架构原生验证快照

GitHub Actions [运行 45958959682](https://github.com/qingyaozhuozhang/CaoZuoXiTong/actions/runs/45958959682) 对提交 `b3988f82b2108ce99bdd192855b367483477bde8` 执行了四个作业。两个 full 各有 **18 PASS / 0 FAIL / 0 SKIP**；两个 UBSan 各有 **15 PASS / 0 FAIL / 3 SKIP**。UBSan 的三项跳过为性能基准、内核 uprobe 检查和真实合流，均由 full 执行。

x86_64 使用 Intel Xeon 6975P-C，ARM64 使用 Neoverse N2，均为原生 Runner。性能构建使用 GCC 13.3.0、`-O2 -Werror`；每架构完成 5 种模式 × 5 轮 × 每轮 1,000,000 次 `getpid` 调用。

| 实测指标 | x86_64 | ARM64 | 判定口径 |
| --- | ---: | ---: | --- |
| 无探针基线 | 103.719 ns | 198.964 ns | 每轮平均调用耗时的中位数 |
| 仅附加会话的增量 | 0.268 ns | -0.073 ns | 未安装补丁，差值接近测量噪声 |
| 入口探针增量 | 60.264 ns | 46.091 ns | `<1000 ns`，通过 |
| 入口 + 返回探针增量 | 124.975 ns | 145.095 ns | `<1000 ns`，通过 |
| 内核 uprobe 增量 | 4266.449 ns | 359.253 ns | 同轮内核对照，五轮实测 |
| 最大插入延迟 | 1.590 ms | 2.315 ms | 全部 entry / entry+return 样本 `<10 ms` |
| 最大完整清理延迟 | 2.464 ms | 1.653 ms | 连续计时，全部样本 `<10 ms` |

增量按每轮百万次调用的均值减去同轮基线后取中位数，不代表单次最大值或 P99。内核组记录 perf 命中计数，未完整消费采样环；它与用户事件采集的负载不同，不据此宣称通用加速倍数。不同 Runner 的数据也不用于比较架构优劣。

A3 在每种架构上实际采集 100 次 `getpid`，得到 200 条用户事件与 100 条 ftrace 事件，合流检查通过。内核时间戳超出对应入口/返回区间的最大距离分别为 1,623 ns 和 6,091 ns，低于预设 100,000 ns 容差；该指标衡量短时、固定 CPU 会话的跨源对齐，不是探针执行开销。

完整作业报告、环境、37 个源码指纹、性能原始样本及 A3 事件文件见 [验证报告归档](docs/archive/README.md)。本次文档整理保留这些原始文件及被测版本标识。

### 6.5 A1 集成验证

归档中的原生 x86_64 和 ARM64 报告来自同一次 Actions 运行、同一提交，构成该版本的 A1 双架构验证证据。单个作业报告中的 A1 标为 `SKIP`，原因是单一架构不能独立判定多架构支持；应联合阅读两种架构结果。这里引用的 ARM64 数据来自 GitHub 原生 ARM64 Runner，交叉编译或 QEMU 耗时不作为原生性能数据。

后续提交仍需重新运行 [GitHub Actions](.github/workflows/ci.yml)，再执行 `python3 -m test sync --wait` 下载当前分支最新结果。`test/results/report.md` 与 `test/results/latest.json` 记录提交、架构、作业结论及缺失项；它们是滚动结果，不能替代 `docs/archive/` 中固定提交的报告。

## 7. 遇到的主要问题和解决方法

| 问题 | 处理方法 | 验证或保留边界 |
| --- | --- | --- |
| 覆盖入口可能拆分或改变原指令语义 | 完整解码与重定位；拒绝不支持的编码 | 解码负向用例及真实函数探测 |
| 信号停止与 syscall-stop 混淆 | TRACESYSGOOD、逐线程停止状态、暂存并转发真实信号 | 信号重入及暂停竞争回归 |
| PIE/无 section-header ELF 解析 | 结合映射与动态段定位符号 | ELF/API 测试；每个作业通过 8 次独立 exec 的双映射地址变化验证 |
| 回调破坏扩展向量现场 | XSAVE/XRSTOR；旧 CPU 回退 FXSAVE | AVX 条件回归；不支持 AMX tile |
| 清理时存在在途返回或回调 | 忙时拒绝、等待安全点后重试 | F4/A4 专项测试 |
| 历史报告容易误作当前成绩 | 归档原始数据；每次 demo 建独立目录并记录源码身份 | 按该次日志及退出码复核 |
| 完整清理延迟与内核对照缺项 | 优化 ptrace 等待，按 seccomp 状态选择可选大页提示；完整清理与真实内核采集进入门禁 | `b3988f8` 双架构通过完整清理、内核对照、A3 合流及 seccomp 回归 |

仍需遵守应用安全点、单控制线程等边界。`PTRACE_O_EXITKILL` 意味着控制进程异常退出可能导致被跟踪目标终止；不应宣称故障后无影响。详细故障处理见 [支持范围与限制](docs/limitations.md)。

## 8. 分工和协作

队伍名称：**在线求偶等她入队**；成员：**刘志钰、迮泳瑞、张向前**。以下作为后续开发、验证和维护的职责安排，历史贡献以实际提交与团队记录为准。

| 成员 | 主要职责 | 协作接口与交付 |
| --- | --- | --- |
| 刘志钰 | 进程控制、探针生命周期与整体集成 | `ludp.c` / `probes.c`，维护状态、错误契约与版本集成 |
| 迮泳瑞 | 指令、符号解析与 ABI 现场 | `x86dec.c` / `aarch64.c` / `elf.c` / 两套 trampoline，维护重定位和寄存器保护 |
| 张向前 | 正确性验证、性能评测与演示文档 | `*_validation.c` / 评测脚本 / `docs/`，维护验证报告与演示说明 |

模块负责人提交实现与对应测试，跨模块修改由相关成员交叉核对；集成时检查文档、测试矩阵与 CI 结果是否同步。每轮集成统一记录系统设计、适用范围、源码版本和实测结果，依据失败日志分配问题并复核修复。

## 9. 提交仓库目录和文件描述

```text
.
├── README.md                 # 设计赛道十章项目说明与评审入口
├── Proj40/                   # C/汇编核心库、示例、测试与评测程序
│   ├── ludp.h                # 公共 API
│   ├── Makefile              # 构建、测试与演示目标
│   ├── probe_demo.c          # 真实动态库函数探针演示
│   └── README.md             # 源码文件职责
├── scripts/                  # 一键验证、A3 真实事件采集与合流
│   ├── run_demo.py
│   ├── event_merge_demo.py
│   └── merge_events.py
├── docs/                     # 需求、设计、接口、测试、限制与演示
│   └── archive/              # 固定提交的验证报告
├── test/                     # GitHub 结果同步与离线验证功能包
│   ├── __main__.py           # python3 -m test 命令入口
│   ├── sync_ci_results.py    # 运行查询、下载与结果替换
│   ├── ci_results_validation.py # 同步逻辑离线验证
│   └── results/              # 最新一轮结果，仅此目录在同步后替换
├── artifacts/                # 本地生成的演示/评测结果，不纳入版本管理
└── .github/workflows/ci.yml  # 原生 x86-64 / ARM64 CI 与 UBSan
```

正式文档集中于 [docs](docs/README.md)。临时二进制和缓存可重新生成；本地结果写入 `artifacts/`，GitHub 最新结果写入 `test/results/`；固定验证报告保存在 `docs/archive/`，不会随最新结果同步而覆盖。公共头文件声明 MIT，仓库尚未提供完整的 LICENSE 文件。

## 10. 比赛收获

从现有实现和测试可以归纳出以下工程经验：

1. 用户态动态探针的正确性依赖指令、ABI、信号和线程状态的共同约束；功能演示必须配合现场恢复与错误路径测试。
2. 低开销设计需要区分控制阶段和命中阶段，也需要明确事件字段、样本规模和资源占用；单个耗时数字不能代替完整测量条件。
3. 可复现交付需要建立“需求—实现—测试—原始证据”的对应关系，明确历史结果、当前结果、跳过项和尚未覆盖的场景。

这些经验形成了后续维护的工作方法：先定义可验证的需求和接口，再实现与测试，最后用一致的文档和原始数据说明结果。

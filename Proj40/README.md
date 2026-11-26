# LUDP：轻量级用户态动态探针（F1–F7、A1）

> 功能范围：F1 入口探针、F2 六参数、F3 返回值，以及已有的 F6 多探针共存、F7 多线程协调与稳定运行能力。支持 Linux x86-64（System V AMD64）及 Linux ARM64（AAPCS64）。

## A1 多架构支持

在对应架构的 Linux 中运行 `make clean && make test`。Makefile 根据编译器的目标选择后端，跨编译可用 `make CC=aarch64-linux-gnu-gcc AR=aarch64-linux-gnu-ar`。切换编译器目标前执行 `make clean`。跟踪器与目标进程使用相同的 64 位架构。

| 平台 | 函数入口 | 参数和返回值 |
|---|---|---|
| x86-64 | 保持原有 `jmp rel32` 和 RIP 相对指令重定位 | `RDI、RSI、RDX、RCX、R8、R9`，返回 `RAX、RDX、XMM0` |
| ARM64 | 一条直接 `B` 跳转，保留 BTI 入口，支持常用位置无关入口指令及 `ADR/ADRP` 重定位 | `X0–X5`，返回 `X0、X1、V0` |

ARM64 后端用 `PTRACE_GETREGSET`/`PTRACE_SETREGSET` 管理线程寄存器，用直接分支执行搬移后的原指令。入口和返回在用户态发布事件。事件布局保持 128 字节，ARM64 的 `xmm0` 字段保存 `V0` 原始字节，`tsc` 字段记录 `CNTVCT_EL0`。

ARM64 支持小端 Linux。当前保守地拒绝入口分支、literal load 和其他未识别入口指令。返回探针适用于正常 ABI 返回，目标的 GCS 需关闭。ARM64 的 `test_f123` 覆盖共享库符号、六个参数、整数和浮点返回、寄存器现场、BTI 后的 ADR 搬移、递归、多线程及环形缓冲区溢出恢复。后文的指令和寄存器示例使用 x86-64。

## 1. 功能完成情况

| 目标 | 实现方式 | 验证方式 |
|---|---|---|
| F1：动态库函数入口探针 | `ptrace` 停止/注入，函数入口改写为 `jmp rel32`，执行用户态 trampoline | 真实共享库符号、原字节恢复、拒绝过短或不支持的指令 |
| F2：读取前六个整数/指针参数 | 从 trampoline 保存的 `RDI、RSI、RDX、RCX、R8、R9` 取得原始参数 | 六个互不相同的 64 位实参、高位数据、负整数、指针 |
| F3：获取返回值 | 临时替换当前调用的栈返回地址，返回时进入独立用户态 stub | `RAX/RDX/XMM0`、递归、栈参数、入口/返回配对 |
| F6：同一进程至少 16 个并发探针 | 独立探针 ID/注册表、不同地址共存、同地址入口探针链 | 此前版本完成 30 探针共存压力验证；当前 `test_f123` 覆盖 9 个目标，精简测试集不含独立 ≥16 探针验收 |
| F7：多线程安全 | 全线程暂停协调、线程状态一致性、clone/退出处理、可重入 trampoline 与多生产者事件缓冲 | 四线程调用、信号处理器重入、已有线程附加、主线程在 `pthread_join`/futex 中阻塞、线程退出回收 |

F6–F7 已有实现是当前探针运行的基础组成部分，并非只有兼容接口。下文分别说明已实现机制、验证证据及尚未覆盖的极端交错。F4、F5 的测试入口为 `make test_f45 && ./test_f45`（Linux x86-64）。

## 2. 工作原理

### F1：入口跳转

```text
原始调用者  call 目标函数
                  │
                  ▼
   目标函数地址 ── jmp rel32 ──────► 入口 trampoline（用户态）
                                       │ 1. 保存寄存器/标志位
                                       │ 2. 采集参数并写事件
                                       │ 3. 恢复寄存器/标志位
                                       ▼
                               执行重定位的原入口指令
                                       │
                                       └──────► 目标函数地址 + 覆盖长度
                                                       │
                                                       ▼
                                                  正常执行函数
```

- 安装时才调用 `ptrace`/远程内存操作；每次命中不执行 `int3`、`syscall` 或 `ptrace`。
- 只在可用 5 字节相对跳转的地址安装，完整解码被覆盖指令；重定位 RIP 相对寻址及可支持的相对分支，遇到不支持的指令拒绝安装。
- ELF 符号定位支持 ASLR/PIE 环境的已加载对象，不能把 IFUNC 解析器直接当作最终函数入口。

### F2：六参数

```text
函数原型（示例）：uint64_t work(uint64_t a, b, c, d, e, f)

参数位置： RDI    RSI    RDX    RCX    R8     R9
事件字段： args[0] ... args[5]
```

这里的“前六个参数”专指 **INTEGER 类寄存器参数**。浮点参数走 `XMM0–XMM7` 等寄存器，混合参数、结构体及隐藏返回指针需要依据函数的真实 ABI 分类解释，不能直接把 `args[0..5]` 当成所有函数前六个形参。事件不自动解引用指针。

### F3：返回事件

```text
正常调用栈返回地址  ──替换──► 唯一 return stub
                                  │ 读取原始 RAX/RDX/XMM0
                                  │ 输出 RETURN 事件
                                  │ 释放调用记录
                                  ▼
                           跳回原始调用者
```

- 调用记录预先分配（4096 槽），入口和返回事件通过 `call_id` 关联；递归和多线程正常返回均有测试。
- 返回值是寄存器原始值：`retval = RAX`，`retval_hi = RDX`，`xmm0[16]` 是 `XMM0` 原始数据。调用者负责结合函数签名解析类型；不支持完整的复杂聚合类型自动还原。
- 返回记录耗尽时正常执行原函数，但该次调用没有返回事件，计入 `ludp_return_missed()`；事件环形缓冲满时丢弃新事件，计入 `ludp_overflow()`，绝不悄悄覆盖旧事件。
- **不支持**被探测栈帧的 `longjmp`、C++ 异常展开、线程取消、协程换栈、`noreturn` 函数或 CET shadow stack；这些路径可能留下无法安全回收的在途返回记录。

### F6：多探针共存

`probes.c` 使用统一注册表管理同一进程内的探针。每个探针保存自己的 ID、符号/库名、目标地址、原始指令、trampoline 地址和启用状态；事件携带 `probe_id`，消费者可以按探针分别统计。`ludp_probe_count()` 返回当前有效探针数，`ludp_probe_list()` 提供详细信息及已消费入口事件的 `hits`。

- **不同函数地址**：分别安装入口跳转和 trampoline，事件写入同一进程的共享 ring。
- **同一函数地址**：仅入口模式支持链式叠加。入口跳到最新启用的探针，依次经过较早的探针；只有链尾执行一次原始指令，然后继续目标函数。增删或切换状态时，`rebuild_addr()` 重建该地址的启用链。
- **返回探针限制**：一个地址存在返回探针，或者新添加的是返回探针时，拒绝该地址的叠加；不同地址的入口+返回探针可以共存。
- **容量**：注册表有 256 项，单地址链重建数组为 64 项。调用方不得超过单地址链容量；移除后可复用注册表槽位，256 是同时注册的探针数上限。

同地址两个入口探针的执行顺序为：

```text
目标函数入口 → 新探针 trampoline → 旧探针 trampoline
                                     ↓
                             重定位的原入口指令
                                     ↓
                              原函数剩余指令
```

此前版本通过了 **30 个探针共存**（含 getpid 同地址双入口探针）的四线程压力测试；无丢事件的测试轮次中，两条 getpid 探针命中数一致。当前目录已经精简为 F1–F3 演示和线程回归程序，旧 `demo stress`/`target` 不在当前构建中，不能直接用旧命令复现。当前 `make test` 的 9 目标测试证明多地址共存，但不等同于重新完成 ≥16 探针专项验收。

### F7：多线程协调与安全运行

F7 由安装时的线程协调和命中时的可重入事件路径共同实现：

| 场景 | 已实现处理 |
|---|---|
| 附加已有多线程进程 | 枚举 `/proc/<pid>/task`，逐线程 `PTRACE_SEIZE`，维护线程 ID 与附加状态的一一对应 |
| 修改入口或探针链 | `quiesce()` 发出 `PTRACE_INTERRUPT` 并等待停止；操作后保持调用前的停止/运行状态 |
| 运行期间创建线程 | `ludp_poll()` 处理 `PTRACE_EVENT_CLONE`，登记新线程并继续运行 |
| 线程退出 | 处理 wait 退出事件和 `/proc` 刷新，以真实线程 ID 删除记录，避免旧状态残留 |
| 主线程阻塞在 futex | 远程系统调用优先选用其他停在用户态的安全线程；无可用线程时返回错误，避免破坏 syscall restart 现场 |
| 多线程同时命中 | 寄存器保存使用各自线程的栈；返回调用记录原子领取；ring 使用 CAS 分配独立事件槽 |
| 信号处理器重入 | 不依赖全局互斥锁，也不等待其他生产者提交；正常信号重入已有测试 |
| 消费者来不及读取 | ring 满时丢弃新事件并计数，不覆盖仍在使用的槽；消费推进水位后继续记录 |

ring 采用“预留序号 → 写事件 → 发布该槽 ready → 消费者推进水位”的协议。生产者不必等待较早序号的生产者完成，避免线程被抢占或信号重入时出现提交等待死锁。消费者只读取连续已发布的事件；`ludp_overflow()` 用于识别过载导致的数据缺失。

使用时应由**单个 tracer 控制线程**定期调用 `ludp_poll()` 和 `ludp_events_read()`。目标可多线程生产事件，但这不代表同一 `ludp_ctx` 的控制 API 可以被多个 tracer 线程并发调用。事件的 `tid` 是 TLS 基址标识；不支持 FSGSBASE 时为 0，不能将它作为始终可用的内核线程号。

这些机制和测试覆盖了稳定运行、已有线程附加、动态创建线程及正常信号重入。全线程暂停本身并不能保证任意时刻都能安全改写指令：线程 RIP 可能落在待覆盖指令中间，或仍在执行要重建的 trampoline。因此安装/重建/移除仍需应用协调的安全点，不能把 F7 的已验证结果扩展为任意并发热插拔保证。

## 3. 构建和测试

环境：Linux x86-64、gcc、GNU make、glibc、Python 3.9+。运行环境必须允许 tracer 对它自己启动的子进程使用 `ptrace`。

```bash
cd Proj40
make -j4            # 构建核心库、测试与性能程序
make test           # 验证解码、ABI、F1–F3、多地址共存与 F7 线程/futex 场景
./demo_f123         # 逐条展示六参数及 RAX / XMM0 返回值（中文）
python3 evaluate.py --rounds 5 --iterations 1000000 --output reports
```

`make test` 覆盖：

1. `test_decode`：RIP 相对/绝对寻址、短跳转重定位、特殊前缀、截断指令拒绝。
2. `test_f123`：9 个真实函数、前六个整数参数、指针、负数、双精度/128 位返回、递归、栈第七八参数、signal 重入、GPR/RFLAGS/x87/SSE/red zone 保存及缓冲区满载。
3. `test_thread_state`：已存在的 4 个工作线程、主动运行/主线程阻塞于 `pthread_join` 两种情况，探针命中、退出事件回收、状态一致性。
4. `demo_f123`：真实打印 F1 安装结果、F2 六个整数参数、F3 整数及浮点返回值。

打印 `SKIP uprobe` 代表内核 perf/uprobes 基准不可用，并非验证通过。`make test` 过程中对刻意不支持的短函数打印 `decode fail` 是**负向测试的预期输出**。

按功能单独运行：

```bash
./test_f123          # F1–F3、多地址共存、运行后创建四线程、signal 重入
./test_thread_state  # F7：附加已有四线程、futex 阻塞主线程、退出状态回收
```

现有执行结果见 [reports/tests.log](reports/tests.log)，线程状态修复过程见 [docs/实现与优化记录.md](docs/实现与优化记录.md)。本 README 的补充不代表重新运行了全部压力或性能测试。

## 4. 最小 C API 示例

```c
#include "ludp.h"

ludp_ctx *ctx = ludp_attach(pid);  /* 目标进程此时已暂停 */
u64 id = 0;
if (!ctx) return;

if (ludp_probe_add_ex(ctx, "libc.so.6", "getpid",
                      LUDP_PROBE_ENTRY | LUDP_PROBE_RETURN, &id) != 0) {
    ludp_detach(ctx);
    return;
}
ludp_resume(ctx);

/* 在目标进程执行期间，单个控制线程定期处理 ptrace 停止。 */
ludp_poll(ctx);

ludp_event events[256];
int n = 0;
if (ludp_events_read(ctx, events, 256, &n) == 0) {
    for (int i = 0; i < n; ++i) {
        if (events[i].type == LUDP_EVENT_RETURN) {
            /* events[i].retval 为 RAX 的原始值 */
        }
    }
}
/* 在应用已协调的安全点移除并分离；生产调用方应检查两个返回值。 */
ludp_probe_remove(ctx, id);
ludp_detach(ctx);
```

入口模式用 `ludp_probe_add()`；入口+返回模式用 `ludp_probe_add_ex()`。事件结构是 128 字节，更新库后必须重新编译调用方。`tsc` 单位是 CPU 周期，不是 CLOCK_MONOTONIC 纳秒；`tid` 是 TLS 基址标识，不是 Linux 内核 TID。

### F6：批量注册 16 个不同地址的入口探针

下面的辅助函数用于目标已附加且停在应用安全点的场景；要求目标已加载 `libc.so.6`，这些符号都能解析和安全重定位。每次注册必须检查返回值，不能把部分成功当作 ≥16 探针成功。

```c
#include "ludp.h"

int add_16_entry_probes(ludp_ctx *ctx, u64 ids[16])
{
    const char *symbols[16] = {
        "getpid", "getppid", "getuid", "geteuid",
        "getgid", "getegid", "gettid", "malloc",
        "free", "calloc", "realloc", "rand",
        "srand", "qsort", "getenv", "atoi"
    };
    for (int i = 0; i < 16; ++i) {
        if (ludp_probe_add(ctx, "libc.so.6", symbols[i], &ids[i]) != 0) {
            /* 由调用方在安全点统一处理已添加的探针或 detach。 */
            return -1;
        }
    }
    return 0;
}
```

全部安装成功后调用 `ludp_resume()`，在控制循环中处理 `ludp_poll()` 并读取事件，按 `probe_id` 计数。**注册成功不等于命中验证**：F6 验收还应让目标实际调用每个函数，核对所有 16 个 ID 均出现事件，同时检查溢出计数和线程退出状态。

### 控制与观测接口

| 接口 | 用途 |
|---|---|
| `ludp_probe_count` / `ludp_probe_list` | 查询有效探针及其地址、状态、已消费的入口命中数 |
| `ludp_probe_enable` / `ludp_probe_disable` | 切换已有探针状态；同地址入口链会重建 |
| `ludp_quiesce` / `ludp_resume` | 暂停/恢复已跟踪线程；不替代应用自身的安全点协议 |
| `ludp_thread_count` | 查询 tracer 当前维护的线程数 |
| `ludp_poll` | 处理 clone、退出及常规信号停止，维持线程运行 |
| `ludp_events_read` | 单消费者批量读取事件，推进 ring 消费水位 |
| `ludp_overflow` / `ludp_return_missed` | 观察事件缓冲过载、返回记录不足造成的缺失 |

## 5. 自动评测、对照、调优

### 正确性与性能一键评测

```bash
python3 evaluate.py --rounds 5 --iterations 1000000 --output reports
```

脚本依次构建、运行全部测试、执行基准、检查阈值并生成 `reports/benchmark.md`、`reports/benchmark.json`、`reports/tests.log`。任意测试失败、数据错误或性能超阈值，返回非零退出码。

比较两个版本：

```bash
python3 evaluate.py --rounds 5 --baseline-json reports/优化前基准.json --output reports
```

会生成 `reports/对照分析.md`；“优化前”原始数据仅供本次对照，其他 CPU 需重新实测。

### 自动选择大块跨进程内存复制方式

```bash
python3 auto_tune.py --rounds 5 --iterations 1000000 --output reports
# 如需将本机建议写成可加载的环境变量文件：
python3 auto_tune.py --rounds 5 --apply
source .ludp_tuning.env
```

`auto_tune.py` 自动比较两条 tracer 侧数据通路：`LUDP_BULK_IO=1`（优先 `process_vm_{readv,writev}`，失败回退）与 `LUDP_BULK_IO=0`（原 `/proc/<pid>/mem` 路径），只从正确性和阈值都通过的候选中选取插入延迟更优者；差异很小时优先默认配置。**调优不会修改目标程序汇编热路径，也不会盲目改写源码。**

## 6. 指标及测量边界

| 指标 | 如何衡量 | 判断方式 |
|---|---|---|
| 单次探针开销 | 百万次 `getpid`，用户态入口与入口+返回相对同轮 baseline 的纳秒差 | 两者均 `< 1000ns` |
| 插入/移除延迟 | tracer 已附加且已到安全点后，测补丁安装与原指令恢复耗时 | 每次 `< 10ms` |
| 无探针时的额外触发成本 | baseline（完全未附加）与 inert（只附加、未修改入口） | 仅作实验对照，差值受调度噪声影响 |
| ABI 正确性 | 参数、返回值、通用/浮点上下文、red zone 等独立用例 | 全部通过 |
| F6 多探针共存 | 同一进程注册并触发 ≥16 个探针，按 ID 核对命中；同地址入口链检查重复采集 | 此前 30 探针压力验证已完成；当前精简测试不含这一规模的专项复测 |
| F7 多线程运行 | 四线程并发、clone、已有线程附加、futex 阻塞主线程及退出回收 | 用例中参数/返回值正确、事件数量符合预期、线程正常完成且无死锁 |
| 内核 uprobe 对照 | `perf_event_open` 创建的真实 uprobe | 权限/设备缺失则报告 SKIP |

基准在被测子进程中使用 `CLOCK_MONOTONIC` 测量一百万次调用，运行后再由 tracer 消费事件；入口模式采集一百万条，入口+返回模式采集两百万条。默认事件环的逻辑容量为 4194304（约 544 MiB 虚拟映射，物理页按需分配），实际物理占用与触达事件数相关。普通监控可以自行缩小容量：

```bash
LUDP_RING_CAP=65536 ./demo_f123
```

`LUDP_RING_CAP` 必须为 16 到 4194304 之间的 2 的幂；65536 槽约占用 8.5 MiB 虚拟映射。使用小环时必须**及时消费事件**，否则会显式产生丢失计数；百万次 `getpid` 严格基准不应减小默认环，否则事件可能溢出。

**插入延迟并不包含最初 attach 的耗时**，也不等同于完整 F4 无泄漏卸载；“无探针时零开销”指未安装补丁时不会增加函数入口执行路径，不能把有 ptrace 附加的 inert 绝对测量值解释成严格 0 ns。

## 7. 当前尚未覆盖的边界

- 只支持 x86-64；不支持 ARM64。
- 在主线程处于被中断的阻塞系统调用时，远程系统调用优先选用其他安全停在用户态的线程；如果找不到安全线程，返回错误，不强行覆盖其寄存器。进程中的任意并发瞬间仍需更严格验证。
- 对于 CET、IBT 强制执行、异常展开、全部信号交错、任意时间热插拔和完全映射回收，仍未满足全面验证条件；这些边界不否定上文列出的 F6–F7 已实现能力，也不能被已有测试结果覆盖。
- 目标进程信号与线程跟踪需要定期调用 `ludp_poll()`；事件消费只支持单消费者。
- 目前主目标是正常 C ABI 函数的稳定入口与正常返回，并非对所有机器码都能动态重定位。

## 8. 文件说明

| 文件 | 用途 |
|---|---|
| `ludp.c`、`ludp.h`、`ludp_internal.h` | 进程控制、内存访问、公共 API、内部数据结构 |
| `probes.c` | F1 安装、F6 注册表/同地址入口链、探针状态与链重建 |
| `x86dec.c`、`elf.c`、`trampoline.S` | 指令解码、符号解析、F1–F3 用户态事件路径与可重入写入 |
| `test_f123.c`、`test_abi.S`、`testlib.c` | F1–F3 集成与 ABI 验证 |
| `test_thread_state.c`、`test_decode.c` | F7 已有多线程/futex/退出状态及解码回归测试 |
| `demo_f123.c` | 面向赛题演示的中文 F1–F3 完整实例 |
| `bench_f123.c`、`benchmark.py` | 标准化性能基准与报告 |
| `evaluate.py`、`auto_tune.py` | 自动构建、正确性验收、版本对照与性能参数调优 |
| `reports/` | 本次测试结果和可再生成的报告；无二进制缓存 |

## 9. 主要参考资料

- [Linux Kernel：Uprobe tracer](https://docs.kernel.org/trace/uprobetracer.html)
- [Linux Kernel：Kprobes / return probes](https://docs.kernel.org/trace/kprobes.html)
- [ptrace(2) 系统调用手册](https://man7.org/linux/man-pages/man2/ptrace.2.html)
- [Libcare：运行中用户态代码热补丁](https://github.com/cloudlinux/libcare)
- [Frida：动态插桩思路](https://frida.re/)
- [bpftime：用户态可观测性技术](https://eunomia.dev/zh/blogs/bpftime/)

## F4、F5 验证与调用约定

- 在应用安全点调用 `ludp_probe_disable`、`ludp_probe_enable`。两者均可重复调用。关闭只影响后续入口，此前已经进入的调用仍可产生返回事件，队列中原有的事件仍可读取。
- 最后一个探针移除时恢复函数入口，回收返回槽和辅助代码区，并重置共享代码池。事件环和主代码池属于会话；`ludp_detach` 成功后释放所有会话映射，目标进程继续运行。
- `ludp_detach(ctx)` 返回 0 时句柄已销毁。应用应先协调线程离开探针及可能中断探针的信号处理程序。若线程正在执行探针代码，或仍有占用的返回槽，清理返回 `-1/EBUSY`。让在途调用完成后重试。进入释放阶段后发生的系统错误保留剩余资源的归属信息，调用方只能重试清理。
- `test_f45` 对自己创建的子进程执行三轮附加和分离，逐字节比较函数入口，检查事件及其返回值，同时核对 `/proc/<pid>/maps` 中是否还有任何会话地址区间。测试还让一次调用停留在函数体内，检查清理拒绝回收活跃返回槽，并在另一个探针保持注册时循环增删 257 次，验证槽位复用。

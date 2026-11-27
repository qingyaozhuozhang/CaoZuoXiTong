# API 使用说明

公共接口定义于 [`Proj40/ludp.h`](../Proj40/ludp.h)，静态库为 `libludp.a`。支持 Linux x86_64 / System V AMD64 ABI 与 Linux ARM64 / AAPCS64；控制器和目标使用相同架构，在更新头文件或库后重新编译。架构见[架构与实现](architecture.md)，运行约束见[适用范围与限制](limitations.md)。

## 1. 构建与会话生命周期

在仓库根目录构建核心库：

```bash
make -C Proj40 libludp.a
```

调用方包含 `ludp.h`，链接 `Proj40/libludp.a` 与 pthread。完整的进程启动、安全点协调、事件读取和清理示例见 [`probe_demo.c`](../Proj40/probe_demo.c)；演示运行方式见[演示指南](demo.md)。

常规调用顺序：

1. 目标初始化并进入应用约定的安全点。
2. `ludp_attach(pid)` 附加已有线程；成功后目标仍处于暂停状态。
3. 添加探针，检查每次返回值与探针 ID。
4. `ludp_resume(ctx)` 恢复目标；由单个控制线程持续调用 `ludp_poll()` 和 `ludp_events_read()`。
5. 通过应用协议停止新增目标调用，等待在途调用退出；在安全点操作启停、更新或移除。
6. 读取所需的剩余事件，检查丢失计数，调用 `ludp_detach(ctx)`。
7. 仅在 detach 返回 0 后将本地 `ctx` 清空；成功后不得再访问它。

`ludp_quiesce()` 只提供线程暂停，不负责构造应用安全点。`ludp_detach()` 失败时上下文仍有效；返回 `EBUSY` 时应在适当恢复目标、使在途调用完成后重新协调清理。其他错误可能涉及部分清理，必须保留上下文并检查状态，不能无条件继续或忽略失败。

## 2. 会话、线程和内存接口

| API | 成功返回 | 说明 |
| --- | --- | --- |
| `ludp_attach(pid)` | 非空 `ludp_ctx *` | 创建并暂停会话；失败返回 NULL |
| `ludp_detach(ctx)` | 0 | 恢复入口、回收会话映射、分离并销毁上下文；NULL 为幂等空操作 |
| `ludp_resolve(ctx, lib, sym, &addr)` | 0 | 查找已加载对象的运行时符号；`lib` 为 NULL/空串表示主程序 |
| `ludp_quiesce(ctx)` / `ludp_resume(ctx)` | 0 | 暂停/恢复已跟踪线程并处理已记录的信号 |
| `ludp_poll(ctx)` | 已处理事件数，或 0 | 处理 clone、退出、信号停止；-1 表示目标退出或处理错误，需结合目标状态和 `errno` 判断 |
| `ludp_thread_count(ctx)` | 线程数 | tracer 当前记录的线程数量 |
| `ludp_mem_read(ctx, addr, buf, len)` | 0 | 读取目标进程内存 |
| `ludp_mem_write(ctx, addr, buf, len)` | 0 | 写入目标进程内存；不自动满足调用方所需的同步协议 |

除明确列出的计数/指针返回接口外，操作通常以 0 表示成功，-1 表示失败并设置 `errno`。所有控制 API 仅支持同一上下文由一个控制线程调用；不得在多个 tracer 线程中并发使用该上下文。

## 3. 添加、查询和管理探针

`flags` 只能为 `LUDP_PROBE_ENTRY` 或 `LUDP_PROBE_ENTRY | LUDP_PROBE_RETURN`，不支持单独的 RETURN 模式。默认添加后启用，`id_out` 用于取得非零探针 ID。地址接口的地址必须属于目标进程，不能将独立 tracer 中的同名函数地址直接代入。

| API | 用途 |
| --- | --- |
| `ludp_probe_add` / `ludp_probe_add_addr` | 按库/符号或运行时地址添加入口探针 |
| `ludp_probe_add_ex` / `ludp_probe_add_addr_ex` | 显式选择入口或入口+返回模式 |
| `ludp_probe_add_filtered_ex` / `ludp_probe_add_addr_filtered_ex` | 添加 A2 条件探针；filter 必须非空，无条件使用 NONE |
| `ludp_probe_add_call_ex` / `ludp_probe_add_addr_call_ex` | 添加 A4 回调探针；call 必须非空且 function 非零 |
| `ludp_probe_enable` / `ludp_probe_disable` | 在安全点启停；相同状态的重复操作幂等 |
| `ludp_probe_remove` | 在安全点删除指定 ID，并检查可回收条件 |
| `ludp_probe_count` | 返回有效探针数 |
| `ludp_probe_list(ctx, out, max)` | 返回实际写入 `out` 的项目数，最多 max 项 |
| `ludp_probe_get_config` | 读取完整配置 |
| `ludp_probe_update` | 在安全点提交完整配置，保留 ID 与 trampoline 地址 |
| `ludp_probe_set_filter` / `ludp_probe_set_call` | 只修改过滤/回调；传入 NULL 清除对应配置 |

`ludp_probe_info` 包含符号、库、目标地址、trampoline、覆盖长度、启用状态、flags、filter、call、generation 和 hits。ARM64 入口存在 BTI 时保留该指令，`addr` 报告 `符号入口 + 4` 的实际补丁地址，`orig_len` 为 4；未带 BTI 时在原入口安装。`hits` 仅统计**已经被消费者读取的入口事件**；未消费、被过滤或被环丢弃的事件不会计入，因此不能将它当作目标函数的实际调用总数。

最多同时存在 256 个探针，同地址入口链同样最多 256 项。同地址存在或新增返回探针时禁止叠加；不同地址的入口+返回探针可以共存。关闭后仍应处理在途返回事件和已经排队的事件。

## 4. 事件字段与读取

目标端 `ludp_raw_event` 为 128 字节；公共 `ludp_event` 为 144 字节。最后两个字段由 tracer 补充，二者不能直接混用布局。

| 字段 | 含义 |
| --- | --- |
| `tsc` | 保留的公共字段名；x86_64 为 RDTSC，ARM64 为 CNTVCT_EL0 原始计数，均不是纳秒 |
| `tid` | x86_64 的 FS/TLS 基址或 ARM64 的 TPIDR_EL0，不是 Linux TID；不可用时为 0 |
| `probe_id` | 探针 ID |
| `args[6]` | x86_64 的 RDI、RSI、RDX、RCX、R8、R9；ARM64 的 X0–X5；返回事件保留对应入口参数 |
| `ret_addr` | 原始调用者返回地址 |
| `type` | `LUDP_EVENT_ENTRY` 或 `LUDP_EVENT_RETURN` |
| `retval` / `retval_hi` | 返回时 x86_64 的 RAX/RDX，或 ARM64 的 X0/X1 原始值；入口事件为 0 |
| `fp_ret[16]` | 返回时 XMM0 或 V0 原始 16 字节；旧字段 `xmm0[16]` 为同布局别名 |
| `call_id` | 成功建立返回记录时非零，用于入口/返回配对；否则为 0 |
| `monotonic_ns` | 原始硬件计数经会话校准映射到 CLOCK_MONOTONIC 的时间估计 |
| `kernel_tid` | 根据当前线程 TLS 标识映射得到的 Linux TID；无法映射时为 0 |

`ludp_events_read(ctx, events, max, &n)` 非阻塞读取连续已发布的事件，成功返回 0，`n` 为本次读取数量。`max == 0` 时允许 `events == NULL`；`max > 0` 时缓冲区必须可容纳相应数量的完整公共事件。一次返回 `n == 0` 只表示当前没有可消费前缀，不等价于目标执行已经结束。

| 计数接口 | 含义 |
| --- | --- |
| `ludp_overflow(ctx)` | 事件环满而丢弃新事件的累计次数 |
| `ludp_return_missed(ctx)` | 返回记录池耗尽、未能安装返回探针的累计次数 |
| `ludp_committed(ctx)` | 消费者已推进的逻辑水位，不是目标端生成总数 |

计数接口的 0 也可能来自读取或上下文错误；需要检查 `errno` 和会话状态。事件消费者必须为单线程。线程退出后 TLS 标识映射可能已被移除，因而即使事件仍在队列中，也不保证总能补全其 `kernel_tid`。

## 5. 条件探针（A2）

过滤器结构：

```c
ludp_filter filter = {
    .op = LUDP_FILTER_MASK_EQ,
    .arg_index = 0,       /* x86_64: RDI；ARM64: X0；有效范围 0..5 */
    .value = 3,
    .mask = 15,
};
```

| op | 无符号 64 位条件 |
| --- | --- |
| `LUDP_FILTER_NONE` | 无条件 |
| `LUDP_FILTER_EQ` / `LUDP_FILTER_NE` | `arg == value` / `arg != value` |
| `LUDP_FILTER_MASK_EQ` | `(arg & mask) == (value & mask)` |
| `LUDP_FILTER_LT` / `LUDP_FILTER_LE` | `arg < value` / `arg <= value` |
| `LUDP_FILTER_GT` / `LUDP_FILTER_GE` | `arg > value` / `arg >= value` |

非 NONE 过滤器的 `arg_index` 必须在 0–5，非法运算或索引返回 `EINVAL`。NONE 被规范化为空配置；MASK_EQ 将 value 规范化为 `value & mask`；其他运算忽略 mask。过滤不通过时原函数仍执行，但不产生该探针的事件、返回记录或回调。当前没有条件树、字符串解引用、浮点比较或有符号比较运算。

以下辅助函数对已经解析的目标进程地址添加入口+返回探针，只采集第一个整数类参数低四位等于 3 的调用。调用前需已附加且处于应用安全点，成功后将 ID 交给正常会话管理流程：

```c
#include "ludp.h"

int add_filtered_probe(ludp_ctx *ctx, uintptr_t target_addr, u64 *id)
{
    const ludp_filter filter = {
        .op = LUDP_FILTER_MASK_EQ,
        .arg_index = 0,
        .value = 3,
        .mask = 15,
    };
    return ludp_probe_add_addr_filtered_ex(ctx, "target", target_addr,
        LUDP_PROBE_ENTRY | LUDP_PROBE_RETURN, &filter, id);
}
```

## 6. 目标进程回调（A4）

```c
typedef struct {
    u64 probe_id;
    u64 args[6];
    u64 user_data;
} ludp_call_context;

typedef void (*ludp_call_fn)(const ludp_call_context *context);

typedef struct {
    uintptr_t function;
    u64 user_data;
} ludp_call_action;
```

`function` 是目标进程内、可执行映射中的函数地址；`user_data` 是目标进程能够解释的值或地址。不能将 tracer 的普通指针直接作为目标进程指针使用。`context` 位于目标线程栈上，回调返回后失效；回调不得保存该指针。

回调在通过过滤之后、原函数执行之前运行，入口事件也在回调结束后生成。回调必须遵守相应架构 ABI，保持短小、非阻塞且可重入，不得递归进入同一探针、异常展开、`longjmp` 或修改未保护的 AMX/SVE/SME 状态。ARM64 回调现场包括 X0–X30、NZCV、Q0–Q31 和 FPCR/FPSR；不支持活动 SVE/SME 上下文、GCS 或在会话中启用这些扩展。库会恢复其保护范围内的寄存器与浮点/向量状态，但不会撤销回调对目标内存、TLS、`errno`、锁或其他应用状态的修改。

`ludp_probe_set_call(ctx, id, NULL)` 清除回调；普通完整配置中 `call.function == 0` 表示关闭回调。带 `add_call_ex` 名称的添加接口则要求明确提供非零回调地址。回调执行期间的更新/回收可能返回 `EBUSY`，此时应等待该执行完成。

下面的辅助函数接受**目标进程地址空间内**的目标函数、回调和用户数据，安装一个无条件入口回调。可编译运行的目标回调与上下文验证见 [`callback_validation.c`](../Proj40/callback_validation.c)：

```c
#include "ludp.h"

int add_callback_probe(ludp_ctx *ctx, uintptr_t target_addr,
                       uintptr_t callback_addr, u64 target_user_data,
                       u64 *id)
{
    const ludp_call_action action = {
        .function = callback_addr,
        .user_data = target_user_data,
    };
    return ludp_probe_add_addr_call_ex(ctx, "target", target_addr,
                                       LUDP_PROBE_ENTRY, NULL, &action, id);
}
```

带回调的添加接口允许 filter 为 NULL；这与 `add_filtered_ex` 的非空 filter 契约不同。

## 7. 配置热更新（A5）

建议先读取现有配置，再只修改所需字段，避免意外清除回调或切换模式：

```c
/* 前提：ctx 有效，id 已存在，应用已经协调到安全点。 */
ludp_probe_config config;
if (ludp_probe_get_config(ctx, id, &config) == 0) {
    config.filter = (ludp_filter){
        .op = LUDP_FILTER_GE,
        .arg_index = 0,
        .value = 90,
    };
    if (ludp_probe_update(ctx, id, &config) != 0) {
        /* 保留 ctx；结合 errno 处理，不能忽略更新失败。 */
    }
}
```

实际变更成功时 `generation` 递增；幂等配置不变。切换入口/返回模式需要不存在冲突的同地址链，且满足相关在途记录检查。

更新失败会尝试恢复原配置与代码；恢复本身失败时返回 `EIO` 并保留停止状态。`-1` 还可能表示恢复运行失败，因此不能仅凭返回值推断旧配置必然继续执行。应查询可读配置、保留上下文并处理底层故障。

## 8. perf-script 风格导出（A3）

```c
char line[512];
int len = ludp_event_format_perf(&event, &probe,
                                 target_pid, cpu, "my-app",
                                 line, sizeof line);
```

成功返回字符数，包含末尾换行但不包含 NUL；空间不足返回 `-1/ENOSPC`，事件类型或时间非法返回 `-1/EINVAL`。CPU 为负数时输出 0；comm 为空时使用 `ludp`，文本最多保留 16 个字符。probe 为空或其 ID 不匹配时使用名称 `probe`，`kernel_tid` 为 0 时文本以 pid 回退显示。

```text
my-app           1234/1237 [003] 9123.456789012: ludp:target_entry: probe_id=7 arg0=0x2a arg1=0x0
```

该示例仅说明格式。入口文本当前只导出前两个参数，返回文本导出 call_id 与 `retval`（RAX/X0）；需要全部参数或浮点返回时使用结构化事件。CPU 和缺失 TID 的回退值不代表实际采样值。输出不是 `perf.data`，也没有完成真实 perf/ftrace 事件合流验证。

`monotonic_ns` 根据事件生成时的硬件计数估算（x86_64 为 TSC，ARM64 为 CNTVCT），在消费阶段计算；它不是消费时刻，也不是热路径直接读取的纳秒时间。当前没有周期重校准或误差界，跨 CPU、长时运行和合流分析需独立验证。

## 9. 容量和环境变量

| 项目 | 默认值/范围 | 说明 |
| --- | --- | --- |
| 探针注册表 | 最多 256 | 包含禁用但尚未移除的探针 |
| 同地址入口链 | 最多 256 | 与注册表共用容量；不能混用返回探针 |
| 在途返回记录 | 4096 | 会话共享；耗尽增加 return-missed |
| `LUDP_RING_CAP` | 默认 `4194304` | 16 到 4194304 的 2 的幂；非法值使 attach 失败 |
| `LUDP_BULK_IO` | 默认启用 | `0` 关闭大块 `process_vm_readv` 优先路径及 x86_64 的批量写优先路径；ARM64 写入始终保留指令缓存同步路径 |

例如在低事件量的功能演示中可使用 `LUDP_RING_CAP=65536`。百万次入口+返回基准在运行后统一消费时需要容纳两百万条事件，不应直接套用小环配置；否则测量会包含大量丢弃事件，不能与完整采集结果比较。

## 10. 常见失败含义

| errno | 典型原因与处理 |
| --- | --- |
| `EINVAL` | 无效上下文、参数、flags、过滤器或容量；修正输入 |
| `ENOENT` | 探针 ID 或符号不存在；重新确认目标对象与 ID |
| `ENOTSUP` | IFUNC、同地址返回冲突、CET shadow stack、ARM64 活动 SVE/SME 或 GCS、不支持的指令等条件 |
| `EBUSY` | 在途返回/回调、无可用远程执行线程、已有 tracer 或当前状态不适宜；确认原因后重试 |
| `EACCES` / `EPERM` | 回调地址不可执行，或系统拒绝目标内存/ptrace 访问 |
| `ENOSPC` | 探针容量或格式化缓冲区不足 |
| `ERANGE` | 分支或 PC 相对操作数重定位超出编码范围；更换可达代码区或目标入口 |
| `EIO` | 底层内存操作、停止状态或回滚失败；保留会话并核对目标状态 |
| `ESRCH` | 目标/线程已经退出 |
| `ETIMEDOUT` | 等待停止等操作超过时限 |

该表列出主要分支，不是所有系统调用错误的穷举。底层内存、ELF 和指令处理仍可能返回其他错误；调用方应记录 API 名称、目标 PID、探针 ID 和即时 `errno`，避免把错误路径当作演示通过。失败契约回归见 [`api_contract_validation.c`](../Proj40/api_contract_validation.c)。

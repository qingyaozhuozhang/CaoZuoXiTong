/*
 * LUDP 公共接口：轻量级用户态动态探针。
 * 覆盖 F1–F7 和 A1/A2/A3/A4/A5 的实现接口；验收状态以测试报告为准。
 * 平台：Linux x86-64 / System V AMD64 ABI、ARM64 / AAPCS64。
 * 许可证：MIT。
 */
#ifndef LUDP_H
#define LUDP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t u64;

#define LUDP_EVENT_ENTRY  1u
#define LUDP_EVENT_RETURN 2u
#define LUDP_PROBE_ENTRY  1u
#define LUDP_PROBE_RETURN 2u

#define LUDP_FILTER_NONE     0u
#define LUDP_FILTER_EQ       1u
#define LUDP_FILTER_NE       2u
#define LUDP_FILTER_MASK_EQ  3u
#define LUDP_FILTER_LT       4u
#define LUDP_FILTER_LE       5u
#define LUDP_FILTER_GT       6u
#define LUDP_FILTER_GE       7u

/* A2：对前六个 INTEGER 类参数之一做无符号条件判断。MASK_EQ 判断
 * (arg & mask) == (value & mask)。NONE 表示无条件采集。 */
typedef struct {
    uint32_t op;
    uint32_t arg_index; /* 0..5：x86 RDI/RSI/RDX/RCX/R8/R9；ARM X0–X5 */
    u64 value;
    u64 mask;
} ludp_filter;

/* A4 回调在目标线程、目标进程地址空间内同步执行。context 只在回调期间
 * 有效；回调不得保存该指针、递归调用同一被探测入口、抛出异常、longjmp
 * 或永久阻塞。user_data 是目标进程可解释的原始值/地址。 */
typedef struct {
    u64 probe_id;
    u64 args[6];
    u64 user_data;
} ludp_call_context;

typedef void (*ludp_call_fn)(const ludp_call_context *context);

typedef struct {
    uintptr_t function; /* 目标进程内、具有执行权限的 ludp_call_fn 地址；0=关闭 */
    u64 user_data;
} ludp_call_action;

/* 公共事件为 144 字节：前 128 字节由目标进程用户态 trampoline 生成，
 * 最后两个字段由 tracer 消费事件时补充。更新库后必须重新编译调用方。 */
typedef struct {
    u64 tsc;          /* x86 RDTSC / ARM CNTVCT_EL0 原始计数；不是纳秒 */
    u64 tid;          /* TLS 基址；不是 Linux 的内核线程号 TID */
    u64 probe_id;     /* 探针唯一编号 */
    u64 args[6];      /* x86 INTEGER: RDI/RSI/RDX/RCX/R8/R9；ARM: X0–X5 */
    u64 ret_addr;     /* 原始调用者返回地址 */
    u64 type;         /* LUDP_EVENT_ENTRY 或 LUDP_EVENT_RETURN */
    u64 retval;       /* 返回时 RAX / X0 的原始值 */
    u64 retval_hi;    /* 返回时 RDX / X1，可用于双寄存器整数返回 */
    u64 call_id;      /* 非零时用于入口/返回事件配对 */
    union {
        uint8_t fp_ret[16]; /* 返回时 XMM0 / V0 原始字节，按目标 ABI 解读 */
        uint8_t xmm0[16];   /* 保留旧字段名，布局与 fp_ret 相同 */
    };
    u64 monotonic_ns; /* A3：由 CLOCK_MONOTONIC 校准的事件时间（纳秒） */
    u64 kernel_tid;   /* 可解析时为 Linux TID，否则为 0 */
} ludp_event;

typedef struct ludp_ctx ludp_ctx;

typedef struct {
    u64 id;
    char sym[128];
    char lib[128];
    uintptr_t addr;        /* 实际补丁地址；ARM BTI 入口保留时为符号地址+4 */
    uintptr_t trampoline;
    int orig_len;
    bool enabled;
    unsigned flags;
    ludp_filter filter;
    ludp_call_action call;
    u64 generation;
    u64 hits;
} ludp_probe_info;

/* A5：热更新后的完整探针配置。更新成功时 ID 不变、generation 递增；
 * 失败时尝试恢复旧配置；回滚失败保持目标暂停并返回 EIO，需处理后重试。 */
typedef struct {
    unsigned flags;
    bool enabled;
    ludp_filter filter;
    ludp_call_action call;
} ludp_probe_config;

/* 进程附加：附加所有线程并进入暂停状态。 */
ludp_ctx *ludp_attach(pid_t pid);
/* 在应用安全点恢复入口、释放本会话的映射并分离。成功返回 0 并销毁 ctx。
 * 有在途探针时返回 -1/EBUSY；待调用返回后重试。其他清理错误也保留 ctx。 */
int ludp_detach(ludp_ctx *ctx);

/* ELF 符号解析；lib 为 NULL/空串时表示主程序。 */
int ludp_resolve(ludp_ctx *ctx, const char *lib, const char *sym, uintptr_t *addr);

/* 仅入口探针：保留原有接口，便于兼容其他阶段代码。 */
int ludp_probe_add(ludp_ctx *ctx, const char *lib, const char *sym, u64 *id_out);
int ludp_probe_add_addr(ludp_ctx *ctx, const char *sym, uintptr_t addr, u64 *id_out);

/* 入口+返回：flags 只能是 ENTRY 或 ENTRY|RETURN。
 * 返回模式支持正常的 SysV AMD64 / AAPCS64 返回，不支持跨被探测帧异常展开、
 * longjmp、协程换栈、不返回的函数、CET shadow stack 或 ARM GCS。
 * 最多 4096 个同时在途的返回记录；耗尽不修改返回地址，只计数丢失。 */
int ludp_probe_add_ex(ludp_ctx *ctx, const char *lib, const char *sym,
                      unsigned flags, u64 *id_out);
int ludp_probe_add_addr_ex(ludp_ctx *ctx, const char *sym, uintptr_t addr,
                           unsigned flags, u64 *id_out);
int ludp_probe_add_filtered_ex(ludp_ctx *ctx, const char *lib, const char *sym,
                               unsigned flags, const ludp_filter *filter,
                               u64 *id_out);
int ludp_probe_add_addr_filtered_ex(ludp_ctx *ctx, const char *sym,
                                    uintptr_t addr, unsigned flags,
                                    const ludp_filter *filter, u64 *id_out);
/* A4：原子添加带目标进程回调的探针。filter 可为 NULL；call 必须非空且
 * function 指向目标进程的可执行映射。 */
int ludp_probe_add_call_ex(ludp_ctx *ctx, const char *lib, const char *sym,
                           unsigned flags, const ludp_filter *filter,
                           const ludp_call_action *call, u64 *id_out);
int ludp_probe_add_addr_call_ex(ludp_ctx *ctx, const char *sym,
                                uintptr_t addr, unsigned flags,
                                const ludp_filter *filter,
                                const ludp_call_action *call, u64 *id_out);
u64 ludp_return_missed(ludp_ctx *ctx);

/* F4/F5：在应用安全点移除或切换探针。重复 enable/disable 幂等。 */
int ludp_probe_remove(ludp_ctx *ctx, u64 id);
int ludp_probe_enable(ludp_ctx *ctx, u64 id);
int ludp_probe_disable(ludp_ctx *ctx, u64 id);
int ludp_probe_count(ludp_ctx *ctx);
int ludp_probe_list(ludp_ctx *ctx, ludp_probe_info *out, int max);

/* A2/A5：在安全点原子替换过滤条件或完整配置，不重新分配 probe ID。 */
int ludp_probe_set_filter(ludp_ctx *ctx, u64 id, const ludp_filter *filter);
/* call==NULL 清除 A4 回调。更新发生在安全点并沿用 A5 事务回滚。 */
int ludp_probe_set_call(ludp_ctx *ctx, u64 id, const ludp_call_action *call);
int ludp_probe_get_config(ludp_ctx *ctx, u64 id, ludp_probe_config *out);
int ludp_probe_update(ludp_ctx *ctx, u64 id, const ludp_probe_config *config);

/* 非阻塞事件读取；需单个消费者，已提交事件不得被覆盖。 */
int ludp_events_read(ludp_ctx *ctx, ludp_event *out, int max, int *n);
u64 ludp_overflow(ludp_ctx *ctx);  /* 事件缓冲区满导致的累计丢失数 */
u64 ludp_committed(ludp_ctx *ctx); /* 消费者已推进的水位 */

/* A3：生成 perf-script 风格的一行文本。cpu<0 时输出 CPU 0；comm 为空时
 * 使用 "ludp"。返回写入字符数，空间不足或参数非法时返回 -1。 */
int ludp_event_format_perf(const ludp_event *event,
                           const ludp_probe_info *probe, pid_t pid, int cpu,
                           const char *comm, char *buf, size_t cap);

/* 目标进程内存读写。 */
int ludp_mem_read(ludp_ctx *ctx, uintptr_t addr, void *buf, size_t len);
int ludp_mem_write(ludp_ctx *ctx, uintptr_t addr, const void *buf, size_t len);

/* 线程协调：与后续 F7 有关的兼容接口。修改指令前需在安全点使用。 */
int ludp_quiesce(ludp_ctx *ctx);
int ludp_resume(ludp_ctx *ctx);
int ludp_thread_count(ludp_ctx *ctx);
/* 在运行期处理 clone、退出及信号的 ptrace 停止；应周期调用。
 * 返回正数：处理的事件数；0：暂无；-1：追踪目标已退出。 */
int ludp_poll(ludp_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* LUDP_H */

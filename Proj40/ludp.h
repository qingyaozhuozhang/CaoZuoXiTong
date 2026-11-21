/*
 * LUDP 公共接口：轻量级用户态动态探针。
 * 本阶段重点：F1 入口跳转、F2 六参数、F3 返回值。
 * 平台：Linux x86-64 / System V AMD64 ABI。
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

/* 目标进程用户态 trampoline 生成的一条事件（固定 128 字节）。 */
typedef struct {
    u64 tsc;          /* RDTSC 周期计数；不是纳秒 */
    u64 tid;          /* TLS 基址；不是 Linux 的内核线程号 TID */
    u64 probe_id;     /* 探针唯一编号 */
    u64 args[6];      /* SysV INTEGER 类：RDI, RSI, RDX, RCX, R8, R9 */
    u64 ret_addr;     /* 原始调用者返回地址 */
    u64 type;         /* LUDP_EVENT_ENTRY 或 LUDP_EVENT_RETURN */
    u64 retval;       /* 返回时 RAX 的原始值 */
    u64 retval_hi;    /* 返回时 RDX，可用于双寄存器整数返回 */
    u64 call_id;      /* 非零时用于入口/返回事件配对 */
    uint8_t xmm0[16]; /* 返回时 XMM0 原始字节，调用者按 ABI 解读 */
} ludp_event;

typedef struct ludp_ctx ludp_ctx;

typedef struct {
    u64 id;
    char sym[128];
    char lib[128];
    uintptr_t addr;
    uintptr_t trampoline;
    int orig_len;
    bool enabled;
    u64 hits;
} ludp_probe_info;

/* 进程附加：附加所有线程并进入暂停状态。 */
ludp_ctx *ludp_attach(pid_t pid);
/* 清理当前入口补丁并分离；远程映射完全回收（F4）尚未完成。 */
void ludp_detach(ludp_ctx *ctx);

/* ELF 符号解析；lib 为 NULL/空串时表示主程序。 */
int ludp_resolve(ludp_ctx *ctx, const char *lib, const char *sym, uintptr_t *addr);

/* 仅入口探针：保留原有接口，便于兼容其他阶段代码。 */
int ludp_probe_add(ludp_ctx *ctx, const char *lib, const char *sym, u64 *id_out);
int ludp_probe_add_addr(ludp_ctx *ctx, const char *sym, uintptr_t addr, u64 *id_out);

/* 入口+返回：flags 只能是 ENTRY 或 ENTRY|RETURN。
 * 返回模式仅支持正常的 System V ABI 返回，不支持跨被探测帧异常展开、
 * longjmp、协程换栈、不返回的函数或 CET shadow stack。
 * 最多 4096 个同时在途的返回记录；耗尽不修改返回地址，只计数丢失。 */
int ludp_probe_add_ex(ludp_ctx *ctx, const char *lib, const char *sym,
                      unsigned flags, u64 *id_out);
int ludp_probe_add_addr_ex(ludp_ctx *ctx, const char *sym, uintptr_t addr,
                           unsigned flags, u64 *id_out);
u64 ludp_return_missed(ludp_ctx *ctx);

/* 与 F4–F6 有关的兼容接口：本阶段不宣称完成相应赛题验收。 */
int ludp_probe_remove(ludp_ctx *ctx, u64 id);
int ludp_probe_enable(ludp_ctx *ctx, u64 id);
int ludp_probe_disable(ludp_ctx *ctx, u64 id);
int ludp_probe_count(ludp_ctx *ctx);
int ludp_probe_list(ludp_ctx *ctx, ludp_probe_info *out, int max);

/* 非阻塞事件读取；需单个消费者，已提交事件不得被覆盖。 */
int ludp_events_read(ludp_ctx *ctx, ludp_event *out, int max, int *n);
u64 ludp_overflow(ludp_ctx *ctx);  /* 事件缓冲区满导致的累计丢失数 */
u64 ludp_committed(ludp_ctx *ctx); /* 消费者已推进的水位 */

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

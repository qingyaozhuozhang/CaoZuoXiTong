# LUDP —— 轻量级用户态动态探针（F6 / F7 模块）

本模块实现赛题《轻量级用户态动态探针（Lightweight Dynamic probe for User Space）》中的
**F6（同一进程 ≥16 个并发探针共存）** 与 **F7（多线程安全，不崩溃、不死锁）** 两个必选功能，
并向上支撑 F1–F5 的基础能力（入口跳转、参数采集、return probe、探针清理/开关）。

技术路线：借助 `ptrace` 修改目标进程内存，在函数入口写一条 `jmp rel32` 跳入目标进程内已注入的
**位置无关、可重入的 trampoline**，在用户态直接记录事件后跳回原函数。全程**不陷入内核**，
单次探针开销 **~110–125 ns**，远低于赛题 `< 1000 ns` 阈值，也低于内核 uprobe 的 1500–3000 ns。

---

## 1. 核心设计（也是本模块的“突出点”）

### 1.1 无提交自旋的 ready-flag 环形缓冲（F7 可扩展性的关键）

早期实现采用 `CAS 预留序号 → 写事件 → 自旋等 committed == 序号 → committed++` 的严格 FIFO 提交协议。
该协议在 2 vCPU 上跑 8 线程时发生**锁护送（convoy）**：任一线程被抢占，其后面所有线程都在提交自旋里空转，
实测 8 线程事件吞吐从 2 线程的 ~1900 万次暴跌到 ~7.6 万次，worker 严重饥饿。

重构后改为 **per-slot ready 标志** 协议：

- 生产者：`lock cmpxchg` 预留全局 `seq`（唯一串行点）→ 写入事件 → `xchg` 发布 `ready[idx]=i+1`。
- **生产者之间互不等待**：发布是各自独立的原子写，全屏障保证事件字段先于 ready 标志可见。
- 消费者（tracer）：批量读 ready 标志 → 找“连续已发布前缀” → 批量读事件 → 回写消费者水位。

环形缓冲布局（256 字节头 + ready 数组 + 事件数组）：

```
[0,256)     header   seq / 消费者水位 / cap / overflow / mask / events_off
[256, 256+cap*8)     ready 标志（每槽 8 字节，存逻辑序号+1）
[events_off, +cap*80) 事件负载（每事件 80 字节）
```

### 1.2 溢出恢复（优雅降级，绝不卡死）

当消费者落后超过一个 ring 容量、槽位被覆盖时，ready 标志会变成“未来”序号。旧实现会误判为
“未发布”而**永久停摆**。本实现检测到 `ready > expected` 即识别为越圈覆盖，直接跳到最新序号继续消费，
因此**即使极端过载也只会丢事件、不会死锁或崩溃**。生产者侧用 `lock incq` 累计溢出计数作为量化指标。

### 1.3 同地址链式多探针（F6 的“16+”亮点）

同一函数地址可叠加多个探针：后加入的探针把 trampoline 挂在链头，并把自己的 trampoline 地址写入
前一个探针的重定位槽，形成**探针链**。移除/开关任一探针时只重建受影响的一段链，其余探针零打扰。
demo 中 `getpid` 上链了第 2 个探针，两探针命中数完全相等，证明链式共存正确。

### 1.4 结构变更在 quiesce 下进行（F7 正确性）

所有“改写指令/重建链”的敏感操作都在 **quiesce**（对全部线程 `PTRACE_INTERRUPT` 并确认停止）下进行，
避免“某线程正在执行被改写指令”的窗口。quiesce 使用**待确认 tid 列表**方案，天然兼容线程中途退出与新增。

### 1.5 运行期动态线程处理（F7 动态 clone）

目标进程在 GO 之后 `pthread_create` 新线程时，新线程会停在 `PTRACE_EVENT_CLONE` 初始停止点。运行期
`ludp_poll()` 周期性 `waitpid` 处理 clone/exit/信号：clone 时取子 tid 并 CONTINUE，exit 时剔除，信号原样转发。
**调用方只需在运行循环里周期性调用 `ludp_poll()`**，动态创建的线程即可正常参与探针与退出。

---

## 2. 目录结构

| 文件 | 作用 |
|------|------|
| `ludp.h` | 公开 API |
| `ludp_internal.h` | 内部结构与 ring 常量 |
| `ludp.c` | attach/detach、线程枚举、quiesce/resume/poll、远程 mmap、内存读写、events_read |
| `probes.c` | 探针注册表、链式共存、enable/disable、rebuild_addr |
| `x86dec.c` | x86-64 指令长度/操作数解码器（用于覆盖 ≥5 字节入口） |
| `elf.c` | /proc/pid/maps + ELF dynsym 符号解析（含 GNU 版本化符号默认版本选择） |
| `trampoline.S` | 位置无关、可重入的探针 trampoline 模板 |
| `target.c` | 测试目标进程（stress / bench / ctx / sig / pie 模式） |
| `demo.c` | 测试 harness（F6/F7 演示、交叉验证、性能基准） |
| `verify.c` | 官方指标验证套件（context / signal / zero / latency / pie） |
| `ctxlib.c` → `libctx.so` | 供 `target` 链接的共享库校验函数（`ctx_check` / `sig_check`） |
| `Makefile` | 构建脚本 |

---

## 3. 构建

```bash
cd ludp
make            # 生成 libludp.a、libctx.so、target、demo、verify
```

依赖：Linux x86_64、gcc、glibc。CPU 需支持 `fsgsbase`（用于 `rdfsbase` 取线程 ID，
不支持时自动退化为 `rdtsc` + 0 线程 ID，不影响正确性）。

---

## 4. 运行 demo

### 4.1 F7 多线程压测

```bash
./demo stress 8 4      # 8 线程、4 秒
```

输出示例（节选）：

```
已添加探针: 30 (目标: >=16)
...
=== F6: 多探针命中统计 ===
id  symbol     address            hits
1   getpid     0x7f...           540886
...
30  getpid#2   0x7f...           540886      <- 链式双探针命中数完全相等
...
=== F7: 多线程安全 ===
目标进程: 正常退出 (DONE=yes, 线程完成=8/8)
事件总数: 18407398
事件涉及的独立线程数: 9
环形缓冲溢出计数: 0
```

### 4.2 性能基准

```bash
./demo bench 200000
```

输出示例：

| 指标 | 数值 |
|------|------|
| 无探针 (baseline) | ~82 ns/次 |
| 用户态探针 | ~197 ns/次 |
| 单次探针开销 | ~115 ns/次 |
| 阈值要求 | < 1000 ns/次 |
| 结论 | 达标 |

---

## 5. 实测数据（2 vCPU 云主机，Ubuntu 22.04，gcc 11.4）

- **30 个探针**（18 libc + 11 libm + 1 个 getpid 链式第二探针）共存，全部命中。
- `getpid` 链式双探针命中数**逐次完全相等**（同地址多探针正确）。
- **8 线程 4 秒**：~1800 万事件、8/8 线程正常退出、0 死锁、0 崩溃；worker 迭代数均衡（无饥饿）。
- **2 线程 3 秒**：~2000 万事件、0 溢出。
- **单次探针开销**：~110–125 ns（阈值 1000 ns，达标）。
- 符号解析（含 `exp`/`hypot` 的 GNU 默认版本）与目标进程 dlsym **逐项交叉验证一致**。

> 说明：运行在共享云主机上时，外部负载偶尔会导致微量溢出计数（~0.1% 量级）；这是**优雅降级**——
> 系统自动跳过已覆盖槽位继续消费，不影响线程完成与进程退出。在独占评审机上应稳定为 0。

### 5.1 官方指标验证套件（`verify`）实测

`verify` 逐项对齐官方评分点，需与 `target` 同目录运行：

| 模式 | 验证内容 | 官方要求 | 实测结果 |
|---|---|---|---|
| `verify context 1000000` | 上下文（GPR/标志/浮点）保持 | 带/不带探针结果逐位一致 | 一致，单次开销 ~100 ns ✅ |
| `verify signal 200000 200` | 信号安全 | 信号不丢、进程正常退出 | 200/200 全部处理、正常退出 ✅ |
| `verify zero 1000000` | 零桩开销 | 未挂探针函数入口无桩指令 | ~4 ns/次 ✅ |
| `verify latency 16` | 插拔延迟 | 插入/清理 < 10 ms | 插 1.0ms / 删 1.07ms / detach 1.1ms ✅ |
| `verify pie 200000` | 主程序（PIE）函数探针 | 可对非动态库函数下探针 | checksum 逐位一致 ✅ |

> 内核 uprobe 基线（官方参考 ~1500–3000 ns）因本开发沙箱无 root/perf、tracefs 只读而无法就地测量，
> 该项对比留待评审机在同机环境下补测；本模块 100–125 ns 的开销对应约 12–24× 的参考加速比。

---

## 6. 公开 API 速览（`ludp.h`）

```c
ludp_ctx *ludp_attach(pid_t pid);                 // 附加并 quiesce 目标进程
void ludp_detach(ludp_ctx *ctx);                  // 恢复原指令、清理、detach

int ludp_probe_add(ludp_ctx*, const char *lib, const char *sym, u64 *id);
int ludp_probe_add_addr(ludp_ctx*, const char *sym, uintptr_t addr, u64 *id);
int ludp_probe_remove(ludp_ctx*, u64 id);
int ludp_probe_enable(ludp_ctx*, u64 id);
int ludp_probe_disable(ludp_ctx*, u64 id);

int ludp_events_read(ludp_ctx*, ludp_event *out, int max, int *n);  // 非阻塞 drain
u64 ludp_overflow(ludp_ctx*);                      // 累计溢出计数

int ludp_quiesce(ludp_ctx*);                       // 停止全部线程
int ludp_resume(ludp_ctx*);                        // 恢复全部线程
int ludp_poll(ludp_ctx*);                          // 运行期处理 clone/exit/信号（F7 关键）

int ludp_mem_read(ludp_ctx*, uintptr_t, void*, size_t);
int ludp_mem_write(ludp_ctx*, uintptr_t, const void*, size_t);
```

---

## 7. 设计细节索引（便于答辩追问）

- **指令覆盖**：入口 `jmp rel32` 需要 5 字节；不足 5 字节时用自写 x86-64 解码器跨指令取整条边界。
- **重定位**：被覆盖的原指令统一搬到 trampoline 内的 32 字节槽（`memset 0x90` 后填入），
  执行完跳回 `probe_addr + orig_len`。对 `getpid` 这类 `endbr64; mov eax,0x27; syscall; ret`
  纯寄存器/立即数指令可直接搬移。
- **参数采集**：按 SysV ABI 在入口处从保存的栈帧回读 `rdi/rsi/rdx/rcx/r8/r9`（前 6 个参数）。
- **return probe**：`ret_addr` 字段记录返回地址；进阶需求可在此基础上加返回点 hook。
- **内存读写**：优先 `/proc/pid/mem` 的 `pread/pwrite`（运行中也可读），只读 text 尾部用
  `PTRACE_PEEKDATA/POKEDATA` 读-改-写兜底。
- **远程 mmap**：在 syscall-entry 停止点用 vDSO 定位 `syscall` 指令，改 RIP 实现远程 `mmap` 注入 ring 与 trampoline 池。
- **主程序（PIE）函数探针 / rel32 可达性**：入口 `jmp rel32` 只能达 ±2GB；主程序（PIE）常在 `0x55..`，
  而主 trampoline 池在 `0x7f..`（紧邻共享库）。当目标超出主池可达范围时，按 `/proc/pid/maps` 找空闲间隙、
  用 `MAP_FIXED_NOREPLACE` 就近 mmap 一个次级 RWX arena（`remote_mmap_near`），仍以 5 字节补丁跳入。
  动态库探针继续走主池，零额外成本；主程序/动态库两类目标因此都可探。

---

## 8. 已避开的坑

- libc/libm 的 **IFUNC**（`gettimeofday time strlen strcmp memcpy memset sin cos tan ceil floor atan` 等）
  静态 `st_value` 是解析器而非真实入口，已判定为不可安全探针目标并跳过。
- **GNU 版本化符号**：`exp`/`hypot` 在 dynsym 中有多个同名项，须选**默认版本（`@@`，无 hidden 位）**
  才能与 dlsym 一致，否则探针挂在旧实现上 0 命中。

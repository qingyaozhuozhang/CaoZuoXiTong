# A3 用户态与内核事件合流验证报告

状态：**PASS**；架构：`aarch64`；时间：2026-11-27T07:31:08.108062+00:00。

验证对象为真实 libc getpid 的 LUDP 入口/返回事件及 tracefs 的 syscalls:sys_enter_getpid 事件。

两路数据使用同一主机、同一次启动的 CLOCK_MONOTONIC 时间域；内核采集使用独立 tracefs instance，结束时销毁。

| 指标 | 实测值 |
|---|---:|
| 核验调用数 | 100 |
| LUDP / 内核事件数 | 200 / 100 |
| 内核事件超出对应入口/返回时间区间的最大距离 | 6091 ns |
| 预先设置的最大允许距离 | 100000 ns |
| 内核文本时间戳精度 | 1000 ns |
| 本次采集持续时间 | 105729697 ns |

[LUDP 原始事件](ludp.perf) · [内核原始事件](kernel.ftrace) · [合流结果](merged.perf) · [内核缓冲统计](kernel-buffer-stats.txt)

## 判定范围

- PASS 要求事件计数、目标 TID、固定 CPU、各来源时间单调性、入口/返回配对及时间容差检查通过，且两路均无事件丢失。
- 默认容差为 100,000 ns，包含用户态计数器校准误差与内核文本时间精度；报告保留实际最大偏差。该值不是探针执行开销门限。
- 合流结果按纳秒整数排序；时间戳相同或偏差范围内的跨来源先后顺序不作因果保证。
- 本项验证 ftrace syscall 事件合流，不宣称已验证所有 perf/kprobe 采集器、长期时钟漂移或任意线程迁移场景。
- SKIP 表示未取得真实内核事件。离线解析验证不代替本项原生采集。
- 格式为 perf-script 风格文本，不是 perf.data 二进制文件。

- 内核默认 syscall 文本 sys_getpid() 在合流时正规化为 ftrace:sys_enter_getpid；kernel.ftrace 保留原始输出。

tracefs instance 与 mono 时钟语义参见 [Linux ftrace 文档](https://docs.kernel.org/trace/ftrace.html)。

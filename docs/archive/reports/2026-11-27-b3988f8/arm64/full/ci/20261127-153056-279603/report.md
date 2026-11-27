# LUDP 功能、正确性与性能验证报告

生成时间：2026-11-27T15:31:08+08:00（Asia/Shanghai）。

执行模式：`full`；所选项目整体结果：**PASS**。

源码提交：`b3988f82b2108ce99bdd192855b367483477bde8`；工作树状态及源码 SHA-256 见 `summary.json`。

系统：`Linux-7.0.0-1017-azure-aarch64-with-glibc2.39`；Python：`3.12.3`。

本报告汇总所选模式的功能、正确性和量化检查；getpid 性能基准及 A3 合流验证另附专项报告。

## 执行结果

| 步骤 | 状态 | 耗时（秒） | 证据或原因 |
|---|---|---:|---|
| 平台与工具检查 | PASS | — | Linux aarch64 构建工具可用 |
| 重新构建库、验证程序与演示 | PASS | 2.035 | [原始日志](01-build.log) |
| 构建前后源码一致性 | PASS | — | 源码 SHA-256 一致 |
| F1/F2/F3：入口跳转、六参数与返回值 | PASS | 0.005 | [原始日志](02-entry-return.log) |
| F4/F5：恢复指令、回收映射与动态开关 | PASS | 0.156 | [原始日志](03-lifecycle.log) |
| F6：16 个地址与同址多探针 | PASS | 0.018 | [原始日志](04-multiprobe.log) |
| F7：多线程与信号交付 | PASS | 0.039 | [原始日志](05-threads.log) |
| 指令解码边界 | PASS | 0.001 | [原始日志](06-decoder.log) |
| F1/F2/F3：ABI、递归、信号与缓冲区边界 | PASS | 0.041 | [原始日志](07-abi.log) |
| 公共 API 与 ELF 契约 | PASS | 0.002 | [原始日志](08-api.log) |
| 独立 exec、ASLR 与 PIE 符号定位 | PASS | 0.031 | [原始日志](08-aslr.log) |
| A2/A3/A5：过滤、事件格式与热更新 | PASS | 0.005 | [原始日志](09-advanced.log) |
| A4：目标进程内回调 | PASS | 0.005 | [原始日志](10-callback.log) |
| A3：用户态事件导出与调用配对 | PASS | 0.012 | [原始日志](10-event-stream.log) |
| 事件合流工具的离线正确性检查 | PASS | 0.084 | [原始日志](event-merge-validation.log) |
| 百万次 getpid 基准与性能阈值 | PASS | 9.149 | [原始日志](11-benchmark.log) |
| 内核 uprobe 实测 | PASS | — | [原始日志](benchmark/benchmark.log)；有效样本 5/5； |
| A3：用户态与 ftrace 真实事件合流 | PASS | 0.234 | [原始日志](12-event-merge.log) |

## 赛题功能覆盖

下表说明本次检查覆盖的能力。PASS 不扩大为任意参数类型、运行时或线程调度的保证。

| 条目 | 本次状态 | 验证范围 |
|---|---|---|
| F1 | PASS | 动态库用户态跳转 |
| F2 | PASS | 前六个整数类 ABI 参数 |
| F3 | PASS | 正常 ABI 返回及事件配对 |
| F4 | PASS | 指令恢复与会话映射回收 |
| F5 | PASS | 安全点动态开关 |
| F6 | PASS | 至少 16 个不同地址探针 |
| F7 | PASS | 多线程、信号及安全点控制 |
| A1 | SKIP | 同一提交的 x86_64 与 ARM64 原生结果；本报告仅验证一种架构，需查看双架构 CI 汇总 |
| A2 | PASS | 单参数条件过滤 |
| A3 | PASS | CLOCK_MONOTONIC 用户态 / ftrace 真实事件合流 |
| A4 | PASS | 目标进程内受约束回调 |
| A5 | PASS | 安全点配置热更新 |

## 结果解释

- PASS 仅表示本次执行的相应检查通过；SKIP 表示未测，不计为已完成验收。
- 快速模式不测量性能。完整模式按每轮 1,000,000 次 getpid 调用生成独立基准。
- 本次验证架构：`aarch64`；A1 完整证据需同一提交的 x86_64 与 ARM64 两份原生运行结果。
- 本目录的日志和 JSON 是运行证据；录屏用于讲解，不替代测量数据。

[getpid 探针性能基准报告](benchmark/benchmark.md) · [性能原始数据](benchmark/benchmark.json) · [基准原始输出](benchmark/benchmark.log)

[A3 用户态与内核事件合流验证报告](event-merge/report.md)

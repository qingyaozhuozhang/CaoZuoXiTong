# LUDP 演示执行报告

生成时间：2026-11-27T13:32:01+08:00（Asia/Shanghai）。

执行模式：`full`；所选项目整体结果：**PASS**。

源码提交：`3ffd7368af3ac1ba1df4385ca4bf1f342b858903`；工作树状态及源码 SHA-256 见 `summary.json`。

系统：`Linux-7.0.0-1017-azure-aarch64-with-glibc2.39`；Python：`3.12.3`。

| 步骤 | 状态 | 耗时（秒） | 证据或原因 |
|---|---|---:|---|
| 平台与工具检查 | PASS | — | Linux aarch64 构建工具可用 |
| 重新构建库、验证程序与演示 | PASS | 1.770 | [原始日志](01-build.log) |
| 构建前后源码一致性 | PASS | — | 源码 SHA-256 一致 |
| F1/F2/F3：入口跳转、六参数与返回值 | PASS | 0.024 | [原始日志](02-entry-return.log) |
| F4/F5：恢复指令、回收映射与动态开关 | PASS | 0.217 | [原始日志](03-lifecycle.log) |
| F6：16 个地址与同址多探针 | PASS | 0.041 | [原始日志](04-multiprobe.log) |
| F7：多线程与信号交付 | PASS | 0.085 | [原始日志](05-threads.log) |
| 指令解码边界 | PASS | 0.001 | [原始日志](06-decoder.log) |
| F1/F2/F3：ABI、递归、信号与缓冲区边界 | PASS | 0.260 | [原始日志](07-abi.log) |
| 公共 API 与 ELF 契约 | PASS | 0.014 | [原始日志](08-api.log) |
| A2/A3/A5：过滤、事件格式与热更新 | PASS | 0.032 | [原始日志](09-advanced.log) |
| A4：目标进程内回调 | PASS | 0.035 | [原始日志](10-callback.log) |
| 百万次 getpid 基准与性能阈值 | PASS | 6.688 | [原始日志](11-benchmark.log) |
| 内核 uprobe 实测 | SKIP | — | [原始日志](benchmark/benchmark.log)；有效样本 0/5；SKIP uprobe: Permission denied |

## 结果解释

- PASS 仅表示本次执行的相应检查通过；SKIP 表示未测，不计为已完成验收。
- 快速模式不测量性能。完整模式按每轮 1,000,000 次 getpid 调用生成独立基准。
- 本次验证架构：`aarch64`；A1 完整证据需同一提交的 x86_64 与 ARM64 两份原生运行结果。
- 本目录的日志和 JSON 是运行证据；录屏用于讲解，不替代测量数据。

[性能报告](benchmark/benchmark.md) · [性能原始数据](benchmark/benchmark.json) · [基准原始输出](benchmark/benchmark.log)

# 验证报告归档

本目录保存经复核的固定测试快照，随仓库提交，供离线查阅功能、正确性与性能证据。当前结果采用 **`b3988f8` 的原生双架构验证**；`test/results/` 仍用于滚动下载，不影响本归档。

## 当前验证版本

- 被测提交：`b3988f82b2108ce99bdd192855b367483477bde8`，分支 `test`。
- GitHub Actions：[45958959682 / attempt 1](https://github.com/qingyaozhuozhang/CaoZuoXiTong/actions/runs/45958959682)。
- 下载时间：2026-11-27 15:33:40（UTC+8）；四个作业成功，无缺失产物或同步警告。
- [双架构 CI 汇总报告](reports/2026-11-27-b3988f8/report.md) · [运行、提交与产物清单](reports/2026-11-27-b3988f8/latest.json)。

四份摘要记录的工作区均干净，37 个源码 SHA-256 在四个作业中一致、构建前后不变，且与被测提交对应文件相符。正文中的结论指向该提交，不把后续文档修改计为另一次实测。

| 架构 | 完整验证 | UBSan 正确性验证 | 原生 Runner 环境 |
| --- | --- | --- | --- |
| x86_64 | [18 PASS / 0 FAIL / 0 SKIP](reports/2026-11-27-b3988f8/x86_64/full/ci/20261127-153059-814545/report.md) | [15 PASS / 0 FAIL / 3 SKIP](reports/2026-11-27-b3988f8/x86_64/ubsan/ci/20261127-153058-549791/report.md) | [Intel Xeon 6975P-C](reports/2026-11-27-b3988f8/x86_64/full/runner/environment.log) |
| ARM64 | [18 PASS / 0 FAIL / 0 SKIP](reports/2026-11-27-b3988f8/arm64/full/ci/20261127-153056-279603/report.md) | [15 PASS / 0 FAIL / 3 SKIP](reports/2026-11-27-b3988f8/arm64/ubsan/ci/20261127-153056-394408/report.md) | [Neoverse N2](reports/2026-11-27-b3988f8/arm64/full/runner/environment.log) |

表中计数是执行步骤状态。单个主报告的需求表将 A1 标为 `SKIP`，表示该作业只运行一种架构；同提交的 x86_64 与 ARM64 原生报告联合构成 A1 证据。UBSan 的三项跳过为性能基准、内核 uprobe 检查和真实事件合流，它们由 full 执行；UBSan 仍运行事件导出与离线合流检查。

## 专项报告与判定

| 专项 | x86_64 | ARM64 |
| --- | --- | --- |
| getpid 探针性能基准 | [报告](reports/2026-11-27-b3988f8/x86_64/full/ci/20261127-153059-814545/benchmark/benchmark.md) / [原始样本](reports/2026-11-27-b3988f8/x86_64/full/ci/20261127-153059-814545/benchmark/benchmark.json) | [报告](reports/2026-11-27-b3988f8/arm64/full/ci/20261127-153056-279603/benchmark/benchmark.md) / [原始样本](reports/2026-11-27-b3988f8/arm64/full/ci/20261127-153056-279603/benchmark/benchmark.json) |
| A3 用户态与内核事件合流 | [报告](reports/2026-11-27-b3988f8/x86_64/full/ci/20261127-153059-814545/event-merge/report.md) / [原始目录](reports/2026-11-27-b3988f8/x86_64/full/ci/20261127-153059-814545/event-merge/) | [报告](reports/2026-11-27-b3988f8/arm64/full/ci/20261127-153056-279603/event-merge/report.md) / [原始目录](reports/2026-11-27-b3988f8/arm64/full/ci/20261127-153056-279603/event-merge/) |
| 内核 uprobe 独立预检 | [1,000 次命中](reports/2026-11-27-b3988f8/x86_64/full/runner/uprobe-check.log) | [1,000 次命中](reports/2026-11-27-b3988f8/arm64/full/runner/uprobe-check.log) |

F1–F7 与 A2/A4/A5 专项验证通过，四个作业均覆盖 8 次独立 exec 的 ASLR/PIE 检查、线程/信号、ABI 现场和 seccomp 回归。A1 由双架构联合验证，A3 由真实采集与合流验证。各主报告旁保存 `summary.json` 和完整步骤日志。

性能基准每架构包含 25 个样本，即 5 种模式 × 5 轮，每个样本调用 `getpid` 1,000,000 次。入口与入口加返回增量均低于 1,000 ns，所有样本的插入和完整清理均低于 10 ms；两种架构均完成内核 uprobe 对照。开销统计是每轮调用均值与同轮基线的差值中位数，不代表单次最大值或 P99。内核组是 perf 命中计数，不能将其 `drops=0` 解释为采样日志全部消费且无丢失。

A3 每架构合流 200 条用户事件与 100 条 ftrace 事件，记录的用户事件溢出及内核缓冲丢失计数均为 0。内核时间戳超出相应入口/返回区间的最大距离为 x86_64 **1,623 ns**、ARM64 **6,091 ns**，小于预设 100,000 ns 容差。这是约 106 ms、固定 CPU 的采集结果；长期漂移与线程迁核不在该用例范围内。

功能支持范围见 [支持范围与限制](../limitations.md)，统计公式与完整清理计时范围见 [测试与性能评估](../testing.md)。

## 来源与完整性

当前快照从已下载的 `test/results/` 逐字节复制 **96 个原始文件**，包含元数据、报告、样本、事件流与日志。下载工具的目录管理标记不属于测试结果，未纳入归档；原始报告的标题、结论、路径记录和样本未改写。汇总原文中的“替换上一次结果”描述同步工具行为，本归档保持固定。

额外生成的 [SHA256SUMS](reports/2026-11-27-b3988f8/SHA256SUMS) 用于检查归档文件完整性。在仓库根目录执行：

```bash
(cd docs/archive/reports/2026-11-27-b3988f8 && sha256sum --check SHA256SUMS)
```

该清单验证文件内容未变化，源码身份另由摘要中的提交号和源码指纹核对。后续代码或验证流程变更需重新运行 CI，再复核并归档对应结果。

## 历史对照

保留一份前序双架构快照，用于追溯清理延迟及内核对照缺项的改进过程；当前功能与性能结论以上述 `b3988f8` 快照为准。

<details>
<summary>3ffd736：前序原生双架构结果</summary>

- [运行汇总](reports/2026-11-27-3ffd736/report.md) · [元数据](reports/2026-11-27-3ffd736/latest.json) · [SHA256SUMS](reports/2026-11-27-3ffd736/SHA256SUMS)。
- Actions 运行 `45946335169`，被测提交 `3ffd7368af3ac1ba1df4385ca4bf1f342b858903`；保留 62 个原始文件。
- 当时内核 uprobe 为 `SKIP`，没有真实 A3 合流证据；最大 detach 分别为 24.482 ms、21.206 ms，尚未测量并限制连续完整清理时间。
- 两轮 x86_64 Runner 的 CPU 型号不同，ARM64 也不是受控 A/B 实验；不以跨轮差值宣称代码优化比例。

</details>

更早的 F1–F3 调优记录和加入 A1 前的本机演示快照已从提交材料移除，可由 Git 历史追溯。

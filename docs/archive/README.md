# 固定验证报告

本目录保存与源码提交对应的完整测试快照，供评审复核功能、性能、运行环境和跳过项。它随源码提交，不随 `test/results/` 的下一次下载更新。

## 2026-11-27 原生双架构验证

- 被测提交：`3ffd7368af3ac1ba1df4385ca4bf1f342b858903`。
- GitHub 运行：[45946335169 / attempt 1](https://github.com/qingyaozhuozhang/CaoZuoXiTong/actions/runs/45946335169)，触发分支为 `test`。
- [运行汇总](reports/2026-11-27-3ffd736/report.md) · [提交、作业和产物清单](reports/2026-11-27-3ffd736/latest.json)。
- 四项作业均成功，报告无缺失；每份运行摘要记录的 30 个源码文件 SHA-256 均与被测提交一致，构建前后的源码指纹一致。

| 架构 | 完整功能与性能报告 | UBSan 报告 | Runner 环境 |
| --- | --- | --- | --- |
| x86_64 | [full](reports/2026-11-27-3ffd736/x86_64/full/ci/20261127-133151-547499/report.md) | [ubsan](reports/2026-11-27-3ffd736/x86_64/ubsan/ci/20261127-133151-815048/report.md) | [full](reports/2026-11-27-3ffd736/x86_64/full/runner/environment.log) / [ubsan](reports/2026-11-27-3ffd736/x86_64/ubsan/runner/environment.log) |
| ARM64 | [full](reports/2026-11-27-3ffd736/arm64/full/ci/20261127-133152-543325/report.md) | [ubsan](reports/2026-11-27-3ffd736/arm64/ubsan/ci/20261127-133152-099880/report.md) | [full](reports/2026-11-27-3ffd736/arm64/full/runner/environment.log) / [ubsan](reports/2026-11-27-3ffd736/arm64/ubsan/runner/environment.log) |

每份报告旁保留 `summary.json` 和全部步骤日志；两个 `full` 目录另有 `benchmark/`，包含五轮百万次 `getpid` 基准的 Markdown、JSON 和原始输出。UBSan 作业只验证正确性，不采集性能数据。

两种架构的入口与入口加返回探针开销、插入/移除门禁均通过。内核 uprobe 对照均因 `Permission denied` 为 **SKIP**，因此三组实测比较尚未完成；完整 detach 另行计时，未全部低于 10 ms。具体边界见 [测试与性能评估](../testing.md)。

## 来源与完整性

报告来自本机已下载的 `test/results/`，62 个报告、元数据和日志文件逐字节复制，未修改原始结论或样本。下载工具的目录管理标记不属于测试报告，未纳入归档。原始汇总中关于“替换上一轮”的文字描述下载行为，本归档保持固定。

归档新增 [SHA256SUMS](reports/2026-11-27-3ffd736/SHA256SUMS) 校验清单，可在仓库根目录验证：

```bash
(cd docs/archive/reports/2026-11-27-3ffd736 && sha256sum --check SHA256SUMS)
```

该快照证明的是上述提交在记录环境中的测试结果，后续修改需重新运行验证。持续下载的新结果仍留在被 Git 忽略的 `test/results/`，只有经复核的完整运行才加入固定归档。

## 已清理的旧材料

已移除 2026-11-21 的 F1–F3 基线与调优记录，以及 2026-11-27 加入 A1 前的本机 demo 快照。它们分别缺少完整功能覆盖或 ARM64 验证，不再作为当前提交文档的主要证据；原始内容可从 Git 历史追溯。清理按版本和覆盖范围进行，保留的双架构快照包含全部原始日志、跳过项和性能边界。

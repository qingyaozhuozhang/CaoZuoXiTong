# LUDP 双架构 CI 验证汇总报告

运行：[45958959682 / attempt 1](https://github.com/qingyaozhuozhang/CaoZuoXiTong/actions/runs/45958959682)
提交：`b3988f82b2108ce99bdd192855b367483477bde8`；分支：`test`；GitHub 结论：**success**。

本目录整体替换上一次结果；缺失报告不会沿用旧文件。

| 作业 | 结果 |
| --- | --- |
| x86_64 / ubsan | success |
| x86_64 / full | success |
| arm64 / full | success |
| arm64 / ubsan | success |

## 报告入口

| 架构 | 验证配置 | 功能、正确性与性能验证报告 |
|---|---|---|
| x86_64 | full | [查看报告](x86_64/full/ci/20261127-153059-814545/report.md) |
| x86_64 | ubsan | [查看报告](x86_64/ubsan/ci/20261127-153058-549791/report.md) |
| arm64 | full | [查看报告](arm64/full/ci/20261127-153056-279603/report.md) |
| arm64 | ubsan | [查看报告](arm64/ubsan/ci/20261127-153056-394408/report.md) |

各架构目录保留上传内容：`ci/<运行时间>/report.md`、`summary.json`、原始日志与 `runner/environment.log`。

专项报告：`benchmark/benchmark.md` 为 getpid 探针性能基准；`event-merge/report.md` 为 A3 用户态与内核事件合流验证。F1–F7、A1–A5 的检查范围与跳过项见各作业主报告，不能仅凭基准报告判断全部功能完成。

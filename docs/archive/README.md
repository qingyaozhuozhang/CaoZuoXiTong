# 历史证据归档

本目录保存固定的运行快照，结论仅适用于记录中的源码与环境，不自动证明后续工作树已经通过验收。新结果使用 `artifacts/` 下的独立运行目录，通过复核后可选择完整快照归档。

原始报告保留运行当时的程序名称、命令和源码指纹，不随文件重命名改写。当前程序名称与职责见 [源码目录说明](../../Proj40/README.md)；旧快照中的命令应结合该说明阅读，重新验证时使用当前构建与演示入口。

## 2026-11-27 完整演示快照

[演示报告](reports/2026-11-27-demo/report.md) 包含 9 个测试/演示程序、五轮百万次基准和全部步骤日志；[性能报告](reports/2026-11-27-demo/benchmark/benchmark.md) 保留插入、移除、attach 与 detach 的不同口径。[summary.json](reports/2026-11-27-demo/summary.json) 记录环境、编译选项、工作树状态和源码 SHA-256。

该快照在本轮文档与 demo 整理时实际执行，结果为所选检查 PASS，内核 uprobe 因权限为 SKIP。运行基于 `4a309dd` 加未提交修改，不能将其结果直接归到原始 `4a309dd`。归档文件从 `artifacts/demo/20261127-120547-897718/` 逐字节复制，没有改写结果；后续版本仍须重新运行。

沙箱中首次执行因 ptrace 权限被拒绝而失败，对应本地日志保留在 `artifacts/demo/20261127-120532-975511/`。本快照来自允许跟踪自建子进程的执行环境；没有修改主机 ptrace 或 perf 策略，也没有获取内核 uprobe 实测数据。

## 2026-11-21 原始基线

[原始报告](reports/2026-11-21/benchmark.md) 来自 F4–F7 完整整合之前的版本。文件从原 `Proj40/reports/` 整体迁入，内容保持不变；其中“F4 尚未实现”等描述仅适用于当时版本。精确源码身份未完整记录，不能追补为当前提交。

| 文件 | 用途 |
| --- | --- |
| [benchmark.json](reports/2026-11-21/benchmark.json) | 环境、每轮原始样本与指标 |
| [benchmark.log](reports/2026-11-21/benchmark.log) | 基准程序输出 |
| [tests.log](reports/2026-11-21/tests.log) | 当时的测试输出，不包含当前全部套件 |
| [优化前基准.json](reports/2026-11-21/优化前基准.json) | 历史版本对照输入 |
| [对照分析.md](reports/2026-11-21/对照分析.md) | 当时两批次结果比较 |
| [自动调优.md](reports/2026-11-21/自动调优.md)、[自动调优.json](reports/2026-11-21/自动调优.json) | tracer 数据复制路径对照 |

上述内核 uprobe 组为 `SKIP`，不提供与内核的实测加速比。原始数据与说明一并保留，不能只选有利数值引用。

## 历史 CI 记录索引

以下对应关系整理自旧版本文档，本轮未重新联网确认运行状态和产物可用性。它们是历史追溯入口，不代替本轮测试。

| 历史提交 | 记录中的运行 | 阶段 |
| --- | --- | --- |
| `19eb240` | [run #2](https://github.com/qingyaozhuozhang/CaoZuoXiTong/actions/runs/45433474571) | F1–F7 整合 |
| `231fffb` | [run #5](https://github.com/qingyaozhuozhang/CaoZuoXiTong/actions/runs/45445417086) | A2/A3/A5 |
| `36538a4` | [run #12](https://github.com/qingyaozhuozhang/CaoZuoXiTong/actions/runs/45523097229) | 线程与 ELF 加固 |
| `8a5343c` | [run #13](https://github.com/qingyaozhuozhang/CaoZuoXiTong/actions/runs/45524284713) | A4 向量状态 |
| `f04615e` | [run #14](https://github.com/qingyaozhuozhang/CaoZuoXiTong/actions/runs/45524720764) | UBSan 与完整验收 |

旧分支说明和硬化报告已整合进正式文档，其原文可在 Git 历史中查阅。Actions 产物可能有保留期限，正式提交应保存与实际提交对应的报告文件。

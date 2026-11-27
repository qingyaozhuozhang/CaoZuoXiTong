# 最新 GitHub 测试结果

本目录与 `Proj40/` 同级，专门保存一轮 GitHub Actions 原生 x86-64 / ARM64 验证结果。首次下载前，本目录只有说明文件；本说明不是通过报告。

在仓库根目录执行：

```bash
python3 scripts/sync_ci_results.py --wait
```

默认选择当前分支最新的 `ci.yml` 运行；也可用 `--run-id RUN_ID` 指定。脚本读取已有 `gh` 登录，或本机的 `GH_TOKEN` / `GITHUB_TOKEN` 环境变量。下载 artifact 需要仓库 Actions 读取权限。

完成后查看 `report.md` 和 `latest.json`，各架构的完整报告及 UBSan 日志分别位于 `x86_64/`、`arm64/`。失败运行也会保存。下一次同步整体替换旧结果；网络或下载失败不会破坏已有结果。重新运行部分作业时，仅保存当前 attempt 的结果，未重跑的报告明确标记缺失。

生成的日志和数据不进入 Git，避免结果提交再次触发 CI。本说明随源码维护。更多说明见 [ARM64 实现与验证](../docs/arm64.md)。

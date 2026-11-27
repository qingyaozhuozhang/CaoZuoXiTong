# GitHub 验证结果功能包

## 首次配置 GitHub（只需一次）

需要 Python 3.9 及以上，以及支持 `gh auth token` 的 [GitHub CLI](https://github.com/cli/cli/blob/trunk/docs/install_linux.md)。安装后，在本机完成登录：

```bash
gh auth login --hostname github.com --web
```

已登录后无需每次重复操作。也可使用本机的 `GH_TOKEN` / `GITHUB_TOKEN`，凭据需要仓库 Actions 读取权限。

## 常用命令

**推送代码后，在仓库根目录执行：**

```bash
python3 -m test sync --branch test --wait --proxy http://127.0.0.1:7890
```

等待 `test` 分支最新一轮 GitHub 测试结束，下载 **x86_64 和 ARM64** 的完整测试及 UBSan 报告。代理端口为本机的 `7890`。

完成后查看 **[结果汇总](results/report.md)**；详细数据分别位于 `results/x86_64/` 和 `results/arm64/`。无需执行 `git pull`。

每次同步只替换 `results/`，保留最新一轮结果，包内代码和文档不受影响。失败运行也会保存；网络或下载失败时保留旧结果。下载内容已由 `.gitignore` 忽略。

<details>
<summary>其他命令（按需使用）</summary>

```bash
# 查看同步选项
python3 -m test sync --help

# 指定 Actions 运行编号
python3 -m test sync --run-id RUN_ID --wait --proxy http://127.0.0.1:7890

# 离线验证同步工具本身，不访问 GitHub
python3 -m test validate
```

`--branch` 指定 Git 分支，省略时使用当前分支，与功能包目录名无绑定关系。项目功能和性能演示仍使用 `python3 scripts/run_demo.py`。

</details>

<details>
<summary>功能包目录</summary>

```text
test/
├── README.md                   本说明
├── __init__.py                 Python 包标识
├── __main__.py                 sync / validate 命令分发
├── sync_ci_results.py          GitHub 请求、产物下载与报告汇总
├── ci_results_validation.py    同步逻辑离线回归
└── results/                    首次成功同步后生成，仅保留最新一轮
    ├── report.md               作业结论、缺失项与日志入口
    ├── latest.json             提交、分支、run ID、attempt 和产物清单
    ├── x86_64/                 full / ubsan 原始报告与环境
    ├── arm64/                  full / ubsan 原始报告与环境
    └── failed-jobs/            可取得的失败作业日志
```

</details>

网络排查、超时重试和退出码见 [ARM64 实现与验证](../docs/arm64.md#4-test-功能包与最新结果)。

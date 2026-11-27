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

等待 `test` 分支最新一轮 GitHub 测试结束，下载 **x86_64 和 ARM64** 的完整测试及 UBSan 报告。示例使用 HTTP 代理端口 `7890`，请按本机代理实际端口填写。

完成后查看 **[结果汇总](results/report.md)**；详细数据分别位于 `results/x86_64/` 和 `results/arm64/`。无需执行 `git pull`。

汇总报告提供每个作业的主报告入口；主报告再链接 getpid 性能基准和 A3 合流专项报告。full 要求真实内核 uprobe、A3 合流及完整清理达标，失败或缺失项会如实保留；UBSan 只验证正确性，不采集性能或真实合流。

每次同步只替换 `results/`，保留最新一轮结果，包内代码和文档不受影响。失败运行和不完整报告也会保存。网络、下载或 ZIP 校验失败时保留旧结果。下载内容已由 `.gitignore` 忽略。

随仓库提交、可直接查阅的固定证据见 [验证报告归档](../docs/archive/README.md)。归档与此处的滚动下载目录独立。

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
    ├── report.md               双架构 CI 验证汇总、缺失项与报告入口
    ├── latest.json             提交、分支、run ID、attempt 和产物清单
    ├── x86_64/                 full / ubsan 原始报告与环境
    ├── arm64/                  full / ubsan 原始报告与环境
    └── failed-jobs/            可取得的失败作业日志
```

</details>

<details>
<summary>网络排查、超时与退出码</summary>

同步启动时打印仓库、分支、认证与代理状态；每次网络请求显示接口及尝试次数。`--wait` 表示等待选定运行结束，查询、等待和下载阶段分别显示进度。

| 提示 | 排查方式 |
| --- | --- |
| 连接重置、超时或拒绝连接 | 检查代理进程与 HTTP 端口；需要直连时使用 `--no-proxy` |
| HTTP 401 | 检查 `gh` 登录或环境变量中的凭据是否有效 |
| HTTP 403 / 429 | 检查 Actions 读取权限、组织策略及 API 限流；限流时等待后重试 |
| HTTP 404 或分支无运行记录 | 核对仓库读取权限、分支是否推送及 `ci.yml` 是否触发 |

能够 `git push` 不代表已经配置 GitHub API 认证，首次使用仍需完成本页开头的登录。脚本默认沿用终端代理，`--proxy` 显式指定 HTTP 代理，`--no-proxy` 忽略环境代理。

单次网络 I/O 超时 `--request-timeout` 默认为 15 秒，临时网络错误及 HTTP 500/502/503/504 默认额外重试两次，可用 `--retries` 调整。CI 等待时限由 `--timeout` 控制，默认为 1800 秒；它与单次 I/O 超时不同。

退出码 `0` 表示 GitHub 运行成功，四份产物齐全，且各报告的提交、架构、配置和状态均通过校验。`1` 表示已保存结果，但运行、报告或失败日志存在问题。`2` 表示同步未完成，有旧结果时会保留旧结果。GitHub 提供 SHA-256 时，下载的 ZIP 也会校验。查看 `results/latest.json` 中的提交、运行号及下载时间确认结果身份。更新脚本后，已启动的进程不会自动载入新代码，应先按 `Ctrl+C` 再运行。

</details>

原生 Runner、构建方式与多架构边界见 [A1 多架构实现与验证](../docs/arm64.md)。

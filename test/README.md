# GitHub 验证结果功能包

本目录与 `Proj40/` 同级，集中维护 GitHub Actions 运行查询、结果下载、报告汇总及同步逻辑的离线验证。包代码随仓库提交，最新一轮原生 x86-64 / ARM64 结果单独保存在 `results/`。本说明不是测试通过报告。

## 运行入口

所有命令从仓库根目录执行，使用 Python 3.9 及以上，无第三方 Python 包依赖。首次同步前安装支持 `gh auth token` 的 [GitHub CLI](https://github.com/cli/cli/blob/trunk/docs/install_linux.md)，并完成本机登录：

```bash
gh auth login --hostname github.com --web
```

```bash
# 查看功能包及同步选项
python3 -m test --help
python3 -m test sync --help

# 等待当前分支最新一轮 ci.yml 运行结束并下载结果
python3 -m test sync --wait

# 指定 test 分支及本机 HTTP 代理（端口按实际配置填写）
python3 -m test sync --branch test --wait --proxy http://127.0.0.1:7890

# 指定 Actions 运行编号
python3 -m test sync --run-id RUN_ID --wait

# 离线验证同步逻辑，不访问 GitHub，不写入真实结果目录
python3 -m test validate
```

`test` 是功能包目录名；`--branch test` 中的 `test` 是 Git 分支名，二者没有绑定关系。不指定 `--branch` 时，选择当前检出分支的最新运行。`sync` 下载已经触发的运行结果，`validate` 检查同步工具本身；项目功能和性能演示仍使用 `python3 scripts/run_demo.py`。

同步读取已有 `gh` 登录，或本机的 `GH_TOKEN` / `GITHUB_TOKEN` 环境变量，凭据需要仓库 Actions 读取权限。启动后立即显示分支、认证与代理状态，以及请求、等待和下载进度。默认单次网络 I/O 等待 15 秒，临时错误额外重试两次；API 限流会明确提示登录或稍后重试。已启动进程不会自动载入代码修改，更新后应先停止旧进程再执行新命令。

## 文件与结果

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

每次完整下载到暂存目录后，仅替换 `results/`，包代码与本说明始终保留。失败运行同样保存；缺失架构或部分重跑未生成的报告明确标记缺失，不沿用上一轮文件。网络或下载失败不会破坏已有结果。

生成的 `results/` 不进入 Git，避免结果提交再次触发 CI。同步完成即可读取本地 `test/results/report.md`，无需 `git pull`。本地演示结果保存在 `artifacts/`，历史快照保存在 `docs/archive/`。登录、网络排查与退出码见 [ARM64 实现与验证](../docs/arm64.md#4-test-功能包与最新结果)。

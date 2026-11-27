# LUDP 源码目录

项目介绍和功能状态见 [仓库 README](../README.md)，正式技术文档统一位于 [docs](../docs/README.md)。本目录保留 x86_64 / ARM64 核心实现、功能验证程序、演示与性能评测入口。

## 构建与运行

从仓库根目录执行：

```bash
make -C Proj40 -j4
make -C Proj40 check
python3 scripts/run_demo.py --correctness-only
python3 scripts/run_demo.py --full
```

上述命令在对应架构的原生 Linux 上执行。Makefile 根据 `$(CC) -dumpmachine` 选择 x86_64 或 AArch64 后端；交叉编译用于构建检查，不能代替原生运行。切换架构或手工改变编译选项前先运行 `make clean`。

构建输出 `libludp.a`，公共头文件为 [ludp.h](ludp.h)。调用方需使用同一版本头文件重新编译；接口及安全点要求见 [API 文档](../docs/api.md)。

## 文件职责

| 文件 | 职责 |
| --- | --- |
| `ludp.c`、`ludp.h`、`ludp_internal.h` | 进程控制、内存访问、事件消费、公共及内部接口 |
| `probes.c` | 探针注册、同地址入口链、开关与配置更新 |
| `elf.c` | ELF/ASLR 符号定位及目标架构检查 |
| `x86dec.c`、`aarch64.c` | 对应架构的指令检查、分支编码与重定位 |
| `trampoline.S`、`aarch64_trampoline.S` | 两套架构的参数采集、返回探针、现场保存与回调执行 |
| `probe_demo.c`、`probe_targets.c` | 真实动态库函数探针演示及目标函数 |
| `*_validation.c`、`validation_arch.h` | 功能、ABI、线程和错误路径验证；架构适配辅助函数 |
| `*_sentinel.S`、`abi_state_aarch64.S`、`simd_state_aarch64.S` | 对应架构的寄存器状态哨兵 |
| `getpid_benchmark.c`、`benchmark.py` | 百万次 getpid 标准基准及原始报告 |
| `event_stream_demo.c` | 导出真实用户态事件，配合 `scripts/event_merge_demo.py` 验证 A3 内核事件合流 |
| `benchmark_validation.py` | 离线检查性能门禁，拒绝缺失/重复样本、非法数值及不完整清理 |
| `evaluate.py` | 正确性与性能统一验收、版本结果对照 |
| `auto_tune.py` | tracer 侧批量内存复制路径对照调优 |

使用 `make -C Proj40 clean` 清理可再生成的二进制。本机报告写入仓库根目录 `artifacts/`；与 `Proj40/` 同级的 `test/` 功能包将 GitHub 最新一轮结果下载至 `test/results/`；固定提交的原始验证报告位于 [报告归档](../docs/archive/README.md)。

## 程序命名与职责

程序按实际功能命名，验证程序统一使用 `_validation` 后缀，x86_64 汇编状态检查使用 `_sentinel` 后缀，ARM64 专用版本使用 `_aarch64` 后缀。赛题编号保留在需求矩阵和运行输出中，便于对应评审条目。

| 源文件 / 可执行程序 | 内容 |
| --- | --- |
| `instruction_validation.c` / `instruction_validation` | x86_64 / ARM64 指令检查、重定位、位移边界及不支持编码拒绝 |
| `probe_abi_validation.c` / `probe_abi_validation` | 入口/返回事件、参数与 ABI 现场、递归、信号和事件缓冲 |
| `probe_lifecycle_validation.c` / `probe_lifecycle_validation` | 动态开关、移除、映射回收与在途返回保护 |
| `multiprobe_validation.c` / `multiprobe_validation` | 16 个地址共存、同地址探针链和链重建 |
| `probe_extensions_validation.c` / `probe_extensions_validation` | 条件过滤、事件格式与配置热更新 |
| `callback_validation.c` / `callback_validation` | 目标进程内回调、现场恢复与忙时保护 |
| `thread_signal_validation.c` / `thread_signal_validation` | 已有线程、阻塞状态、线程退出及信号交付 |
| `api_contract_validation.c` / `api_contract_validation` | 公共 API 错误契约及主程序 ELF 符号解析 |
| `aslr_pie_validation.c` / `aslr_pie_validation` | 8 次独立 exec 后的 PIE/动态库地址解析与实际探针验证 |
| `probe_demo.c` / `probe_demo` | 可读的动态库函数入口、六参数与返回值演示 |
| `event_stream_demo.c` / `event_stream_demo` | 固定 CPU 采集 libc getpid，导出带单调时间的 perf-script 风格事件 |
| `getpid_benchmark.c` / `getpid_benchmark` | 百万次 getpid 的无探针、用户态探针与内核 uprobe 基准 |
| `probe_targets.c` / `libprobe_targets.so` | 整数、指针、浮点、递归、栈参数和信号等被探测函数 |
| `abi_state_sentinel.S` | 通用寄存器、RFLAGS、浮点状态及 red zone 哨兵 |
| `avx_state_sentinel.S` | x86_64 AVX 高半区现场恢复哨兵 |
| `abi_state_aarch64.S` | ARM64 X0–X30、SP、NZCV、FPCR/FPSR 与 Q0–Q31 状态哨兵 |
| `simd_state_aarch64.S` | ARM64 回调后的 Q0–Q31 高半区恢复哨兵 |

完整验证使用 `make -C Proj40 check`；`make -C Proj40 test` 保留为兼容别名，不生成以 `test` 命名的程序。

## GitHub Actions 验证结果

[CI 工作流](../.github/workflows/ci.yml) 使用 `ubuntu-24.04` 与 `ubuntu-24.04-arm` 原生 runner，分别执行完整功能/性能和 UBSan 回归。提交 `3ffd736` 的两架构 full / UBSan 四个作业均已通过，原始证据见 [验证报告归档](../docs/archive/README.md)。后续提交需重新验证，交叉编译与模拟执行不替代原生性能测试。

推送后，从仓库根目录执行：

```bash
python3 -m test sync --wait
```

功能包将当前分支最新一轮的成功或失败结果下载至 `test/results/`，仅替换上一轮结果，保留提交和 run/attempt 信息；包代码与说明不会被覆盖。入口说明见 [test/README.md](../test/README.md)，回归口径见[测试规范](../docs/testing.md)，ARM64 约束见[架构文档](../docs/architecture.md)。

#!/usr/bin/env python3
"""功能正确性门禁、getpid 性能基准和可选版本对照。"""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parent


def run_cmd(cmd, log_path=None):
    result = subprocess.run(cmd, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    print(result.stdout, end="")
    if log_path is not None:
        log_path.write_text(result.stdout, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"命令失败（退出码 {result.returncode}）：{' '.join(map(str, cmd))}")


def summarize(data):
    samples = data.get("samples", [])
    metrics = {}
    for mode in ("entry", "return"):
        subset = [s for s in samples if s["mode"] == mode]
        metrics[mode] = {
            "overhead_ns": data.get("paired_overhead_ns", {}).get(mode),
            "median_insert_ms": statistics.median(s["insert_ms"] for s in subset) if subset else None,
            "max_insert_ms": max(s["insert_ms"] for s in subset) if subset else None,
            "max_remove_ms": max(s["remove_ms"] for s in subset) if subset else None,
            "max_cleanup_ms": max((s["cleanup_ms"] for s in subset if "cleanup_ms" in s), default=None),
        }
    return metrics


def write_compare(before, after, filename):
    old = summarize(before)
    new = summarize(after)
    lines = ["# getpid 探针性能版本对照报告", "", "对照数值来自两个独立批次；CPU 调度、频率、虚拟机负载会产生波动。",
             "单次基准不能证明差异具有统计显著性，必须结合正确性测试与多次采样判断。", "",
             "| 项目 | 优化前 | 优化后 | 改变 |", "|---|---:|---:|---:|"]
    for mode, label in (("entry", "入口探针"), ("return", "入口与返回探针")):
        for key, desc, unit in (("overhead_ns", "单次调用额外开销", "ns"),
                                ("median_insert_ms", "探针插入耗时中位数", "ms"),
                                ("max_cleanup_ms", "完整清理最大耗时", "ms")):
            a, b = old[mode][key], new[mode][key]
            if a is None or b is None:
                old_value = '未提供' if a is None else f'{a:.3f} {unit}'
                new_value = '未提供' if b is None else f'{b:.3f} {unit}'
                lines.append(f"| {label}：{desc} | {old_value} | {new_value} | — |")
                continue
            lines.append(f"| {label}：{desc} | {a:.3f} {unit} | {b:.3f} {unit} | {b-a:+.3f} {unit} |")
    lines += ["", "## 正确性和性能阈值", "",
              f"- 优化前正确性：{'通过' if before.get('correctness') else '未通过'}；性能阈值：{'达标' if before.get('performance') else '未达标'}。",
              f"- 优化后正确性：{'通过' if after.get('correctness') else '未通过'}；性能阈值：{'达标' if after.get('performance') else '未达标'}。",
              "- 内核 uprobe 如果显示 SKIP，则没有取得对比结果，不能补写参考数值。",
              "- 正确性门禁运行当前架构的全部验证程序；性能数据量化 getpid 入口及入口与返回路径。",
              "- x86_64 和 ARM64 必须分别原生运行；本对照不将不同架构或不同性能门禁口径的通过状态视为等价。",
              "- 旧报告若缺少完整清理耗时，表示当时未测该指标，不能按零耗时处理。", ""]
    filename.write_text("\n".join(lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description="运行功能正确性测试和 getpid 探针性能基准")
    parser.add_argument("--rounds", type=int, default=5, help="每种模式重复轮数")
    parser.add_argument("--iterations", type=int, default=1_000_000, help="单轮调用数")
    parser.add_argument("--output", type=Path, default=ROOT.parent / "artifacts" / "validation")
    parser.add_argument("--baseline-json", type=Path, help="优化前 benchmark.json，用于生成版本对照")
    parser.add_argument("--require-kernel", action="store_true", help="内核 uprobe 不允许跳过")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    try:
        run_cmd(["make", "-j4"])
        run_cmd(["make", "check"], output / "tests.log")
        cmd = [sys.executable, "benchmark.py", "--rounds", str(args.rounds),
               "--iterations", str(args.iterations), "--output", str(output)]
        if args.require_kernel:
            cmd.append("--require-kernel")
        run_cmd(cmd)
        optimized = json.loads((output / "benchmark.json").read_text(encoding="utf-8"))
        if not optimized["correctness"] or not optimized["performance"]:
            raise RuntimeError("正确性或性能指标未达标")
        if args.baseline_json:
            previous = json.loads(args.baseline_json.read_text(encoding="utf-8"))
            write_compare(previous, optimized, output / "version_comparison.md")
        print("评测完成：当前架构功能验证通过，getpid 探针执行、插入及完整清理性能阈值达标。")
        return 0
    except (RuntimeError, OSError, ValueError, KeyError) as exc:
        print(f"评测失败：{exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

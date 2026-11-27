#!/usr/bin/env python3
"""自动实测 bulk I/O 的两条实现路径，选择正确且插入延迟较低的一条。"""
import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description="F1–F3 自动参数调优：批量远程内存复制")
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--iterations", type=int, default=1_000_000)
    parser.add_argument("--output", type=Path, default=ROOT.parent / "artifacts" / "tuning")
    parser.add_argument("--apply", action="store_true", help="将推荐值写入本地 .ludp_tuning.env")
    args = parser.parse_args()
    if args.rounds < 2 or not 1 <= args.iterations <= 2_000_000:
        parser.error("rounds >= 2 且 iterations 在 1..2000000 范围内")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if subprocess.call(["make", "-j4"], cwd=ROOT) or subprocess.call(["make", "check"], cwd=ROOT):
        print("正确性回归未通过，停止性能调优", file=sys.stderr)
        return 1
    results = {}
    with tempfile.TemporaryDirectory(prefix="ludp_tune_") as tmp:
        for name, val in (("process_vm 批量复制", "1"), ("传统 /proc/pid/mem", "0")):
            env = os.environ.copy()
            env["LUDP_BULK_IO"] = val
            path = Path(tmp) / val
            cmd = [sys.executable, "benchmark.py", "--rounds", str(args.rounds),
                   "--iterations", str(args.iterations), "--output", str(path)]
            result = subprocess.run(cmd, cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT)
            print(f"[{name}] 退出码 {result.returncode}")
            if not (path / "benchmark.json").exists():
                print(result.stdout, file=sys.stderr)
                return 1
            data = json.loads((path / "benchmark.json").read_text(encoding="utf-8"))
            valid = result.returncode == 0 and data["correctness"] and data["performance"]
            slots = [s["insert_ms"] for s in data["samples"] if s["mode"] == "return"]
            results[val] = {"name": name, "valid": bool(valid),
                            "return_insert_median_ms": statistics.median(slots) if slots else None,
                            "entry_overhead_ns": data["paired_overhead_ns"].get("entry"),
                            "return_overhead_ns": data["paired_overhead_ns"].get("return"),
                            "skipped": data["skipped"]}
    candidates = [v for v in ("0", "1") if results[v]["valid"]]
    if not candidates:
        print("所有候选均不满足正确性和性能要求，不修改默认设置", file=sys.stderr)
        return 1
    best = min(candidates, key=lambda v: results[v]["return_insert_median_ms"])
    # 小于 0.2ms 的差异视为噪声，优先采用 process_vm 快速路径。
    if "1" in candidates and results["1"]["return_insert_median_ms"] <= results[best]["return_insert_median_ms"] + 0.2:
        best = "1"
    lines = ["# F1–F3 自动调优报告", "", f"基准：每轮 {args.iterations:,} 次 getpid，共 {args.rounds} 轮。", "",
             "| 测试路径 | 正确性与阈值 | F3 插入延迟中位数(ms) | F1 开销(ns) | F3 开销(ns) |",
             "|---|---|---:|---:|---:|"]
    for val in ("0", "1"):
        x = results[val]
        lines.append(f"| {x['name']} | {'通过' if x['valid'] else '失败'} | {x['return_insert_median_ms']:.3f} | {x['entry_overhead_ns']:.3f} | {x['return_overhead_ns']:.3f} |")
    lines += ["", f"**建议：`LUDP_BULK_IO={best}`。**", "",
              "参数只影响 tracer 侧大块跨进程内存读写；探针触发路径的汇编代码相同。",
              "该选择仅针对本机当次采样，不保证迁移到另一台机器后仍最优。", ""]
    (out / "自动调优.md").write_text("\n".join(lines), encoding="utf-8")
    (out / "自动调优.json").write_text(json.dumps({"recommended": best, "candidates": results}, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if args.apply:
        (ROOT / ".ludp_tuning.env").write_text(f"export LUDP_BULK_IO={best}\n", encoding="utf-8")
        print("已写入 .ludp_tuning.env，可执行：source .ludp_tuning.env")
    print(f"推荐配置：LUDP_BULK_IO={best}；完整报告：{out / '自动调优.md'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

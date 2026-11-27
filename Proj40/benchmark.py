#!/usr/bin/env python3
"""Reproducible local benchmark; unavailable kernel measurements remain SKIP."""
import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
from zoneinfo import ZoneInfo


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--iterations', type=int, default=1_000_000)
    parser.add_argument('--rounds', type=int, default=5)
    parser.add_argument('--output', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'artifacts' / 'benchmark')
    parser.add_argument('--require-kernel', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    if not 1 <= args.iterations <= 2_000_000 or not 1 <= args.rounds <= 100:
        parser.error('iterations must be 1..2000000; rounds 1..100')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cmd = ['./getpid_benchmark', str(args.iterations), str(args.rounds)]
    result = subprocess.run(cmd, cwd=root, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=180 * args.rounds)
    print(result.stdout, end='')
    (output / 'benchmark.log').write_text(result.stdout)
    samples = []
    skips = []
    for line in result.stdout.splitlines():
        if line.startswith('SKIP '):
            skips.append(line)
        if line.startswith('SAMPLE '):
            fields = dict(item.split('=', 1) for item in line.split()[1:])
            samples.append({k: v if k in ('mode', 'status') else float(v)
                            for k, v in fields.items()})
    groups = {mode: [s for s in samples if s['mode'] == mode]
              for mode in ('baseline', 'inert', 'entry', 'return', 'uprobe')}
    medians = {mode: statistics.median(s['ns_per_call'] for s in group)
               for mode, group in groups.items() if group}
    complete = all(len(groups[m]) == args.rounds for m in ('baseline', 'inert', 'entry', 'return'))
    correct = complete and result.returncode == 0 and all(s['status'] == 'PASS' for s in samples)
    overhead = {m: statistics.median(
        s['ns_per_call'] - next(b['ns_per_call'] for b in groups['baseline'] if b['round'] == s['round'])
        for s in groups[m]) for m in ('inert', 'entry', 'return', 'uprobe') if groups[m] and complete}
    active = groups['entry'] + groups['return']
    latency = max((max(s['insert_ms'], s['remove_ms']) for s in active), default=float('inf'))
    performance = complete and overhead['entry'] < 1000 and overhead['return'] < 1000 and latency < 10
    cpu = next((line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines()
                if line.startswith('model name')), 'unknown')
    metadata = dict(date=datetime.datetime.now(ZoneInfo('Asia/Shanghai')).isoformat(),
                    platform=platform.platform(), cpu=cpu,
                    affinity=sorted(os.sched_getaffinity(0)), iterations=args.iterations, rounds=args.rounds)
    payload = dict(environment=metadata, samples=samples, skipped=sorted(set(skips)),
                   median_ns=medians, paired_overhead_ns=overhead,
                   correctness=correct, performance=performance)
    (output / 'benchmark.json').write_text(json.dumps(payload, indent=2, ensure_ascii=False) + '\n')
    lines = ['# F1/F3 性能报告', '', f"生成时间：{metadata['date']}", '',
             f"CPU：{cpu}；系统：{metadata['platform']}", '',
             f'每轮 getpid 调用 {args.iterations:,} 次，共 {args.rounds} 轮；轮换执行顺序。', '',
             '| 模式 | 中位数 ns/次 | 同轮相对无探针的增量中位数 ns/次 | 范围 ns/次 |',
             '|---|---:|---:|---:|']
    for mode, group in groups.items():
        if group:
            costs = [s['ns_per_call'] for s in group]
            delta = f'{overhead[mode]:.3f}' if mode in overhead else '—'
            lines.append(f'| {mode} | {medians[mode]:.3f} | {delta} | {min(costs):.3f}–{max(costs):.3f} |')
        else:
            lines.append(f'| {mode} | SKIP | — | — |')
    lines += ['', f"事件/返回值/入口字节恢复/无运行期 ptrace 停止检查：{'PASS' if correct else 'FAIL'}。",
              f"入口和入口+返回增量 <1000 ns、每次插入/移除 <10 ms：{'PASS' if performance else 'FAIL'}。", '',
              '| 模式 | 最大插入 ms | 最大移除 ms | 最大 attach ms | 最大 detach ms |',
              '|---|---:|---:|---:|---:|']
    for mode in ('entry', 'return'):
        if groups[mode]:
            lines.append('| ' + mode + ' | ' + ' | '.join(f"{max(s[k] for s in groups[mode]):.3f}"
                          for k in ('insert_ms', 'remove_ms', 'attach_ms', 'detach_ms')) + ' |')
    lines += ['', '测量说明：', '',
              '- baseline 完全不附加 tracer；inert 仅附加、不插入任何探针，并验证入口字节不变。时间差包含调度噪声，不能把一次差值解释成严格零纳秒。',
              '- CLOCK_MONOTONIC 计时；子进程先预热 getpid 10000 次。用户态事件区首次触页成本包含在计时中，计时结束后读取事件。',
              '- entry 每次一条事件，return 每次入口和返回共两条事件。两者增量均按一次函数调用计。',
              '- 内核 uprobe 使用 perf PMU，采样 IP/TID，以计数器核验命中次数；样本缓冲可能溢出，不是完整日志吞吐对比，也没有宣称与用户态采集字段等价。',
              '- 移除延迟测量 ludp_probe_remove 全调用（含入口恢复及部分空闲存储释放）；最终 detach 的会话映射回收另行计时。F4 资源回收由 probe_lifecycle_validation 独立验收，不能用耗时替代泄漏检查。',
              '- 未运行本报告之外的完整正确性证明；请同时执行 make check。', '']
    lines.extend(sorted(set(skips)))
    (output / 'benchmark.md').write_text('\n'.join(lines) + '\n')
    print(f'Report: {output / "benchmark.md"}')
    return 0 if correct and performance and (not args.require_kernel or len(groups['uprobe']) == args.rounds) else 1


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Reproducible local benchmark; unavailable kernel measurements remain SKIP."""
import argparse
import datetime
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
from zoneinfo import ZoneInfo


MODES = ('baseline', 'inert', 'entry', 'return', 'uprobe')
INTEGER_FIELDS = ('round', 'iterations', 'total_ns', 'events', 'drops', 'missed', 'stops')
FLOAT_FIELDS = ('ns_per_call', 'attach_ms', 'insert_ms', 'remove_ms', 'detach_ms', 'cleanup_ms')


def read_optional(path):
    try:
        return Path(path).read_text(encoding='utf-8').strip()
    except OSError:
        return 'unavailable'


def parse_output(output):
    samples, skips, errors = [], [], []
    for line in output.splitlines():
        if line.startswith('SKIP '):
            skips.append(line)
        if not line.startswith('SAMPLE '):
            continue
        try:
            fields = dict(item.split('=', 1) for item in line.split()[1:])
            sample = {key: int(fields[key]) for key in INTEGER_FIELDS}
            sample.update({key: float(fields[key]) for key in FLOAT_FIELDS})
            sample.update(mode=fields['mode'], status=fields['status'])
            if sample['mode'] not in MODES or sample['status'] not in ('PASS', 'FAIL'):
                raise ValueError('unknown mode or status')
            if any(not math.isfinite(sample[key]) or sample[key] < 0 for key in FLOAT_FIELDS):
                raise ValueError('negative or non-finite timing')
            if any(sample[key] < 0 for key in INTEGER_FIELDS):
                raise ValueError('negative counter')
            samples.append(sample)
        except (KeyError, ValueError) as exc:
            errors.append(f'Invalid sample ({exc}): {line}')
    return samples, sorted(set(skips)), errors


def assess_samples(samples, rounds, iterations, returncode=0, require_kernel=False, parse_errors=()):
    """Fail closed on missing/duplicate rounds and on incomplete final cleanup."""
    groups = {mode: [s for s in samples if s['mode'] == mode] for mode in MODES}
    expected_rounds = list(range(1, rounds + 1))
    complete_modes = {mode: sorted(s['round'] for s in group) == expected_rounds
                      for mode, group in groups.items()}
    complete = all(complete_modes[m] for m in MODES[:-1])
    sample_integrity = complete and not parse_errors and all(
        1 <= s['round'] <= rounds and s['iterations'] == iterations for s in samples)
    if groups['uprobe'] and not complete_modes['uprobe']:
        sample_integrity = False
    medians = {mode: statistics.median(s['ns_per_call'] for s in group)
               for mode, group in groups.items() if group}
    baseline = {s['round']: s['ns_per_call'] for s in groups['baseline']}
    overhead = {mode: statistics.median(s['ns_per_call'] - baseline[s['round']] for s in groups[mode])
                for mode in MODES[1:] if complete_modes['baseline'] and complete_modes[mode]}
    active = groups['entry'] + groups['return']
    latency = {key: max((s[f'{key}_ms'] for s in active), default=None)
               for key in ('insert', 'remove', 'detach', 'cleanup')}
    events_correct = all(s['status'] == 'PASS' and not s['drops'] and not s['missed'] and not s['stops']
                         and s['events'] == iterations * {'baseline': 0, 'inert': 0, 'entry': 1,
                                                         'return': 2, 'uprobe': 1}[s['mode']]
                         for s in samples)
    correct = sample_integrity and returncode == 0 and events_correct
    kernel_complete = complete_modes['uprobe'] and all(s['status'] == 'PASS'
                        and s['iterations'] == iterations and s['events'] == iterations
                        and not s['drops'] and not s['missed'] and not s['stops']
                        for s in groups['uprobe'])
    checks = dict(sample_integrity=sample_integrity,
                  event_correctness=correct,
                  execution_overhead=complete and all(overhead[m] < 1000 for m in ('entry', 'return')),
                  insertion_latency=complete and latency['insert'] < 10,
                  removal_latency=complete and latency['remove'] < 10,
                  complete_cleanup_latency=complete and latency['cleanup'] < 10,
                  kernel_comparison=kernel_complete)
    performance = all(checks[key] for key in ('execution_overhead', 'insertion_latency',
                                             'removal_latency', 'complete_cleanup_latency'))
    return dict(samples=samples, median_ns=medians, paired_overhead_ns=overhead,
                correctness=correct, performance=performance, checks=checks,
                kernel_comparison_complete=kernel_complete, kernel_required=require_kernel,
                passed=correct and performance and (kernel_complete or not require_kernel),
                max_latency_ms=latency,
                limits=dict(overhead_ns=1000, insert_ms=10, remove_ms=10, cleanup_ms=10),
                process_returncode=returncode, parse_errors=list(parse_errors))


def main():
    parser = argparse.ArgumentParser(description='getpid 探针性能基准：执行开销、插入及完整清理延迟')
    parser.add_argument('--iterations', type=int, default=1_000_000)
    parser.add_argument('--rounds', type=int, default=5)
    parser.add_argument('--output', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'artifacts' / 'benchmark')
    parser.add_argument('--require-kernel', action='store_true', help='要求每轮取得内核 uprobe 实测，否则门禁失败')
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
    samples, skips, parse_errors = parse_output(result.stdout)
    payload = assess_samples(samples, args.rounds, args.iterations, result.returncode,
                             args.require_kernel, parse_errors)
    groups = {mode: [s for s in samples if s['mode'] == mode]
              for mode in MODES}
    medians, overhead = payload['median_ns'], payload['paired_overhead_ns']
    cpu = next((line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines()
                if line.split(':', 1)[0].strip() in ('model name', 'Hardware', 'CPU part')), 'unknown')
    metadata = dict(date=datetime.datetime.now(ZoneInfo('Asia/Shanghai')).isoformat(),
                    platform=platform.platform(), machine=platform.machine(), cpu=cpu,
                    affinity=sorted(os.sched_getaffinity(0)), iterations=args.iterations, rounds=args.rounds,
                    ludp_options={name: os.environ.get(name, 'default')
                                  for name in ('LUDP_BULK_IO', 'LUDP_RING_CAP', 'LUDP_HUGEPAGES')},
                    transparent_hugepage_policy=read_optional('/sys/kernel/mm/transparent_hugepage/enabled'))
    payload.update(environment=metadata, skipped=skips)
    (output / 'benchmark.json').write_text(json.dumps(payload, indent=2, ensure_ascii=False) + '\n')
    lines = ['# getpid 探针性能基准报告', '', f"生成时间：{metadata['date']}", '',
             f"架构：{metadata['machine']}；CPU：{cpu}；系统：{metadata['platform']}", '',
             f"透明大页内核策略：`{metadata['transparent_hugepage_policy']}`；"
             f"LUDP_HUGEPAGES：`{metadata['ludp_options']['LUDP_HUGEPAGES']}`。大页建议不保证实际分配成功，性能以实测为准。", '',
             '范围：比较无探针、仅附加会话、用户态入口、用户态入口与返回、内核 uprobe 五种模式。',
             '本报告量化 getpid 调用开销及探针生命周期延迟；F1–F7、A1–A5 的功能覆盖与限制见完整验证报告和对应日志。', '',
             f'每轮 getpid 调用 {args.iterations:,} 次，共 {args.rounds} 轮；轮换执行顺序。', '',
             '| 模式 | 中位数 ns/次 | 同轮相对无探针的增量中位数 ns/次 | 范围 ns/次 |',
             '|---|---:|---:|---:|']
    for mode, group in groups.items():
        if group:
            costs = [s['ns_per_call'] for s in group]
            delta = f'{overhead[mode]:.3f}' if mode in overhead else '—'
            lines.append(f'| {mode} | {medians[mode]:.3f} | {delta} | {min(costs):.3f}–{max(costs):.3f} |')
        else:
            lines.append(f"| {mode} | {'SKIP' if mode == 'uprobe' else 'FAIL（缺失）'} | — | — |")
    verdict = lambda value: 'PASS' if value else 'FAIL'
    kernel_status = ('PASS' if payload['kernel_comparison_complete'] else
                     'FAIL' if args.require_kernel or groups['uprobe'] else 'SKIP')
    lines += ['', f"当前运行配置门禁：**{verdict(payload['passed'])}**。",
              f"内核 uprobe 对比：**{kernel_status}**（{'必须取得每轮实测' if args.require_kernel else '允许因权限或平台不可用而跳过'}）。", '',
              '| 检查项 | 要求 | 结果 |', '|---|---|---|',
              f"| 样本完整性 | 每个必测模式每轮恰有一个样本 | {verdict(payload['checks']['sample_integrity'])} |",
              f"| 事件、返回值、入口恢复与运行期停止 | 数据一致，无丢失或运行期 ptrace 停止 | {verdict(payload['correctness'])} |",
              f"| 入口与入口+返回执行增量 | 同轮差值的中位数均 < 1000 ns/调用 | {verdict(payload['checks']['execution_overhead'])} |",
              f"| 插入延迟 | 每次 < 10 ms | {verdict(payload['checks']['insertion_latency'])} |",
              f"| 移除延迟 | 每次 < 10 ms | {verdict(payload['checks']['removal_latency'])} |",
              f"| 完整清理延迟 | remove 开始至 detach 返回，每次 < 10 ms | {verdict(payload['checks']['complete_cleanup_latency'])} |", '',
              '| 模式 | 最大插入 ms | 最大移除 ms | 最大 attach ms | 最大 detach ms | 最大完整清理 ms |',
              '|---|---:|---:|---:|---:|---:|']
    for mode in ('entry', 'return'):
        if groups[mode]:
            lines.append('| ' + mode + ' | ' + ' | '.join(f"{max(s[k] for s in groups[mode]):.3f}"
                          for k in ('insert_ms', 'remove_ms', 'attach_ms', 'detach_ms', 'cleanup_ms')) + ' |')
    lines += ['', '测量说明：', '',
              '- baseline 完全不附加 tracer；inert 仅附加、不插入任何探针，并验证入口字节不变。时间差包含调度噪声，不能把一次差值解释成严格零纳秒。',
              '- CLOCK_MONOTONIC 计时；子进程先预热 getpid 10000 次。用户态事件区首次触页成本包含在计时中，计时结束后读取事件。',
              '- entry 每次一条事件，return 每次入口和返回共两条事件。两者增量均按一次函数调用计。',
              '- 内核 uprobe 使用 perf PMU，采样 IP/TID，以计数器核验命中次数；样本缓冲可能溢出，不是完整日志吞吐对比，也没有宣称与用户态采集字段等价。',
              '- 完整清理从 ludp_probe_remove 调用前开始，覆盖入口恢复、恢复字节校验、ludp_detach 及最终会话映射回收；此指标纳入 <10 ms 门禁。F4 资源回收还需 probe_lifecycle_validation 验收，耗时不能替代泄漏检查。',
              '- 表中为每轮全部调用的均值及跨轮统计，不代表单次调用的最大值或 P99；虚拟机调度可能造成波动。',
              '- 内核 uprobe 未测时不生成加速倍数；严格模式 --require-kernel 将缺失对比判为失败。',
              '- 本报告不代替完整功能验证；请同时执行 make check，或运行 scripts/run_demo.py --full。', '']
    lines.extend(skips + parse_errors)
    (output / 'benchmark.md').write_text('\n'.join(lines) + '\n')
    print(f'Report: {output / "benchmark.md"}')
    return 0 if payload['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())

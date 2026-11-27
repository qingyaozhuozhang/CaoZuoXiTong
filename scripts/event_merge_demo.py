#!/usr/bin/env python3
"""Capture and validate real LUDP + ftrace getpid events in a private instance.

Requires an already mounted tracefs and permission to create an instance.
No global tracer settings, mounts, permissions, or sysctls are changed.
"""

import argparse
from contextlib import contextmanager
import datetime as dt
import errno
import json
import os
from pathlib import Path
import platform
import re
import selectors
import signal
import subprocess
import uuid

from merge_events import merge_events, parse_events


ROOT = Path(__file__).resolve().parents[1]
READY = re.compile(r"READY pid=(\d+) cpu=(\d+) calls=(\d+)\n")


class TraceUnavailable(RuntimeError):
    pass


def write_control(path, value):
    path.write_text(value + "\n", encoding="ascii")


@contextmanager
def trace_instance(root):
    instance = root / "instances" / f"ludp-a3-{os.getpid()}-{uuid.uuid4().hex[:12]}"
    created = False
    try:
        try:
            instance.mkdir()
            created = True
            write_control(instance / "tracing_on", "0")
            clocks = (instance / "trace_clock").read_text(encoding="ascii")
            if "mono" not in clocks.replace("[", "").replace("]", "").split():
                raise TraceUnavailable("tracefs does not provide trace_clock=mono")
            if not (instance / "events/syscalls/sys_enter_getpid/enable").is_file():
                raise TraceUnavailable("kernel does not expose syscalls:sys_enter_getpid")
            write_control(instance / "trace_clock", "mono")
            if "[mono]" not in (instance / "trace_clock").read_text(encoding="ascii"):
                raise RuntimeError("tracefs did not select the requested mono clock")
            write_control(instance / "buffer_size_kb", "64")
            write_control(instance / "trace", "")
        except OSError as exc:
            if exc.errno in {errno.EACCES, errno.EPERM, errno.ENOENT, errno.EROFS, errno.ENODEV}:
                raise TraceUnavailable(f"tracefs instance unavailable: {exc}") from exc
            raise
        yield instance
    finally:
        if created:
            # These writes and rmdir only concern the instance created above.
            # Cleanup errors propagate as FAIL, rather than hiding a leaked instance.
            try:
                write_control(instance / "tracing_on", "0")
                write_control(instance / "events/enable", "0")
            finally:
                instance.rmdir()


def stop_collector(process):
    if process.poll() is None:
        try:
            process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            # Only the collector started here. Its own child also has
            # PR_SET_PDEATHSIG set, so terminating it cannot orphan the child.
            process.kill()
            process.communicate()


def read_ready(process):
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        if not selector.select(timeout=20):
            raise RuntimeError("collector did not reach its ready point within 20 seconds")
        line = process.stdout.readline()
    match = READY.fullmatch(line)
    if not match:
        raise RuntimeError(f"collector failed before READY: {line.strip()!r}")
    return line, tuple(map(int, match.groups()))


def verify_streams(ludp, kernel, metadata, tolerance_ns):
    calls, pid, cpu = (metadata[key] for key in ("calls", "pid", "cpu"))
    entries = [event for event in ludp if event.name == "ludp:getpid_entry"]
    returns = [event for event in ludp if event.name == "ludp:getpid_return"]
    if len(ludp) != calls * 2 or len(entries) != calls or len(returns) != calls:
        raise ValueError(f"expected {calls} entry/return pairs, got {len(entries)}/{len(returns)}")
    if any(event.name != ("ludp:getpid_entry" if i % 2 == 0 else "ludp:getpid_return")
           for i, event in enumerate(ludp)):
        raise ValueError("single-thread collector entry/return events must alternate")
    if len(kernel) != calls or any(event.name != "ftrace:sys_enter_getpid" for event in kernel):
        raise ValueError(f"expected {calls} real sys_enter_getpid events, got {len(kernel)}")
    if any(event.tid != pid or event.cpu != cpu for event in ludp + kernel):
        raise ValueError("event TID/CPU differs from the collector's verified identity/affinity")
    for stream in (ludp, kernel):
        if any(a.monotonic_ns > b.monotonic_ns for a, b in zip(stream, stream[1:])):
            raise ValueError("a source's timestamps went backwards")
    skews, seen_ids = [], set()
    for index, (entry, syscall, ret) in enumerate(zip(entries, kernel, returns), 1):
        entry_ids = re.findall(r"(?:^|\s)call_id=(\d+)(?=\s|$)", entry.fields)
        return_ids = re.findall(r"(?:^|\s)call_id=(\d+)(?=\s|$)", ret.fields)
        if len(entry_ids) != 1 or len(return_ids) != 1 or entry_ids != return_ids or int(entry_ids[0]) == 0:
            raise ValueError(f"call {index}: missing or mismatched call_id")
        if int(entry_ids[0]) in seen_ids:
            raise ValueError(f"call {index}: duplicate call_id")
        seen_ids.add(int(entry_ids[0]))
        if entry.monotonic_ns > ret.monotonic_ns:
            raise ValueError(f"call {index}: entry timestamp is after return")
        skew = max(entry.monotonic_ns - syscall.monotonic_ns,
                   syscall.monotonic_ns - ret.monotonic_ns, 0)
        skews.append(skew)
        if skew > tolerance_ns:
            raise ValueError(f"call {index}: kernel event outside entry/return interval by {skew} ns; limit={tolerance_ns} ns")
    begin, end = metadata["begin_ns"], metadata["end_ns"]
    if end < begin or any(event.monotonic_ns < begin - tolerance_ns or
                          event.monotonic_ns > end + tolerance_ns for event in ludp + kernel):
        raise ValueError("event timestamp is outside the measured CLOCK_MONOTONIC capture window")
    return {"calls_verified": calls, "ludp_events": len(ludp), "kernel_events": len(kernel),
            "maximum_interval_skew_ns": max(skews, default=0),
            "clock_tolerance_ns": tolerance_ns,
            "kernel_text_precision_ns": max(event.precision_ns for event in kernel),
            "capture_duration_ns": end - begin}


def collect(args, output):
    with trace_instance(args.tracefs) as instance:
        collector = ROOT / "Proj40/event_stream_demo"
        if not collector.is_file():
            raise RuntimeError("event_stream_demo is missing; run make -C Proj40 first")
        stderr_path = output / "collector.stderr.log"
        with stderr_path.open("w", encoding="utf-8") as errors:
            process = subprocess.Popen([str(collector), str(args.calls)], cwd=collector.parent,
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors,
                                       text=True)
            try:
                ready, (pid, cpu, calls) = read_ready(process)
                event_dir = instance / "events/syscalls/sys_enter_getpid"
                write_control(event_dir / "filter", f"common_pid == {pid}")
                write_control(event_dir / "enable", "1")
                write_control(instance / "tracing_on", "1")
                captured, _ = process.communicate("GO\n", timeout=30)
                write_control(instance / "tracing_on", "0")
                (output / "collector.log").write_text(ready + captured, encoding="utf-8")
                trace = (instance / "trace").read_text(encoding="utf-8")
                (output / "kernel.ftrace").write_text(trace, encoding="utf-8")
                stats = (instance / f"per_cpu/cpu{cpu}/stats").read_text(encoding="utf-8")
                (output / "kernel-buffer-stats.txt").write_text(stats, encoding="utf-8")
                if process.returncode:
                    raise RuntimeError(f"collector exit status {process.returncode}; see collector.stderr.log")
                for key in ("overrun", "commit overrun", "dropped events"):
                    found = re.search(rf"^{re.escape(key)}:\s*(\d+)", stats, re.MULTILINE)
                    if not found:
                        raise ValueError(f"kernel buffer statistics omitted {key!r}")
                    if int(found.group(1)):
                        raise ValueError(f"kernel buffer reported {key}={found.group(1)}")
                summaries = [line[8:] for line in captured.splitlines() if line.startswith("SUMMARY ")]
                if len(summaries) != 1:
                    raise ValueError("collector must emit exactly one SUMMARY")
                metadata = json.loads(summaries[0])
                if (metadata["pid"], metadata["cpu"], metadata["calls"]) != (pid, cpu, calls):
                    raise ValueError("collector identity changed after READY")
                if any(metadata[key] for key in ("overflow", "return_missed", "runtime_stops")):
                    raise ValueError("collector reported dropped events or runtime ptrace stops")
                ludp_text = "\n".join(line for line in captured.splitlines() if not line.startswith("SUMMARY ")) + "\n"
                (output / "ludp.perf").write_text(ludp_text, encoding="utf-8")
                ludp = parse_events(ludp_text, "ludp", "perf")
                kernel = parse_events(trace, "kernel", "ftrace")
                metrics = verify_streams(ludp, kernel, metadata, args.clock_tolerance_ns)
                merged = merge_events(ludp, kernel)
                (output / "merged.perf").write_text(
                    "# CLOCK_MONOTONIC; real LUDP + real ftrace syscall events\n" +
                    "".join(event.perf_line() for event in merged), encoding="utf-8")
                return {"status": "PASS", "collector": metadata, "metrics": metrics,
                        "kernel_event": "syscalls:sys_enter_getpid", "trace_clock": "mono",
                        "tracefs": str(args.tracefs), "evidence_kind": "native_live_capture"}
            finally:
                stop_collector(process)


def write_report(output, payload):
    (output / "summary.json").write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    lines = ["# A3 用户态与内核事件合流验证报告", "",
             f"状态：**{payload['status']}**；架构：`{payload['architecture']}`；时间：{payload['created_at']}。", "",
             "验证对象为真实 libc getpid 的 LUDP 入口/返回事件及 tracefs 的 syscalls:sys_enter_getpid 事件。", "",
             "两路数据使用同一主机、同一次启动的 CLOCK_MONOTONIC 时间域；内核采集使用独立 tracefs instance，结束时销毁。", ""]
    if payload.get("reason"):
        lines += [f"原因：{payload['reason']}", ""]
    if payload["status"] == "PASS":
        m = payload["metrics"]
        lines += ["| 指标 | 实测值 |", "|---|---:|",
                  f"| 核验调用数 | {m['calls_verified']} |",
                  f"| LUDP / 内核事件数 | {m['ludp_events']} / {m['kernel_events']} |",
                  f"| 内核事件超出对应入口/返回时间区间的最大距离 | {m['maximum_interval_skew_ns']} ns |",
                  f"| 预先设置的最大允许距离 | {m['clock_tolerance_ns']} ns |",
                  f"| 内核文本时间戳精度 | {m['kernel_text_precision_ns']} ns |",
                  f"| 本次采集持续时间 | {m['capture_duration_ns']} ns |", "",
                  "[LUDP 原始事件](ludp.perf) · [内核原始事件](kernel.ftrace) · [合流结果](merged.perf) · [内核缓冲统计](kernel-buffer-stats.txt)", ""]
    lines += ["## 判定范围", "",
              "- PASS 要求事件计数、目标 TID、固定 CPU、各来源时间单调性、入口/返回配对及时间容差检查通过，且两路均无事件丢失。",
              "- 默认容差为 100,000 ns，包含用户态计数器校准误差与内核文本时间精度；报告保留实际最大偏差。该值不是探针执行开销门限。",
              "- 合流结果按纳秒整数排序；时间戳相同或偏差范围内的跨来源先后顺序不作因果保证。",
              "- 本项验证 ftrace syscall 事件合流，不宣称已验证所有 perf/kprobe 采集器、长期时钟漂移或任意线程迁移场景。",
              "- SKIP 表示未取得真实内核事件。离线解析验证不代替本项原生采集。",
              "- 格式为 perf-script 风格文本，不是 perf.data 二进制文件。", "",
              "- 内核默认 syscall 文本 sys_getpid() 在合流时正规化为 ftrace:sys_enter_getpid；kernel.ftrace 保留原始输出。", "",
              "tracefs instance 与 mono 时钟语义参见 [Linux ftrace 文档](https://docs.kernel.org/trace/ftrace.html)。", ""]
    (output / "report.md").write_text("\n".join(lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path,
                        default=ROOT / "artifacts/event-merge" / dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    parser.add_argument("--tracefs", type=Path, default=Path("/sys/kernel/tracing"))
    parser.add_argument("--calls", type=int, choices=range(1, 513), default=100, metavar="1..512")
    parser.add_argument("--clock-tolerance-ns", type=int, default=100_000)
    parser.add_argument("--require-kernel", action="store_true", help="treat missing tracefs capability as FAIL")
    args = parser.parse_args()
    if args.clock_tolerance_ns < 0:
        parser.error("--clock-tolerance-ns must not be negative")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error("--output must be empty to avoid mixing evidence from different runs")
    def interrupted(signum, _frame):
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        raise InterruptedError(f"capture interrupted by {signal.Signals(signum).name}")
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    try:
        payload = collect(args, output)
    except TraceUnavailable as exc:
        payload = {"status": "FAIL" if args.require_kernel else "SKIP", "reason": str(exc)}
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.TimeoutExpired) as exc:
        payload = {"status": "FAIL", "reason": str(exc)}
    payload.update(created_at=dt.datetime.now(dt.timezone.utc).isoformat(), architecture=platform.machine(),
                   clock_tolerance_ns=args.clock_tolerance_ns, required=args.require_kernel)
    write_report(output, payload)
    print(f"A3 real event merge: {payload['status']}" + (f" ({payload['reason']})" if payload.get("reason") else ""), flush=True)
    print(f"Report: {output / 'report.md'}", flush=True)
    return 1 if payload["status"] == "FAIL" else 0


if __name__ == "__main__":
    raise SystemExit(main())

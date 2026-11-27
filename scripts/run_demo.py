#!/usr/bin/env python3
"""Run the competition demo and preserve commands, results, and source identity."""

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import signal
import subprocess
import sys
import time
from zoneinfo import ZoneInfo


ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / "Proj40"
TIMEZONE = ZoneInfo("Asia/Shanghai")
DEFAULT_CFLAGS = "-O2 -g -Wall -Wextra -fno-strict-aliasing -pthread"
MAKE_CONTROL_ENV = ("MAKEFLAGS", "MFLAGS", "MAKEOVERRIDES", "GNUMAKEFLAGS")


class DemoInterrupted(KeyboardInterrupt):
    def __init__(self, signum):
        self.signum = signum


def request_stop(signum, _frame):
    # Finish child cleanup and evidence writing even if termination is repeated.
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    raise DemoInterrupted(signum)


def timestamp():
    return dt.datetime.now(TIMEZONE).isoformat(timespec="seconds")


def read_optional(path):
    try:
        return Path(path).read_text(encoding="utf-8").strip()
    except OSError:
        return "unavailable"


def identify_sources():
    files = [PROJECT / "Makefile", ROOT / ".github/workflows/ci.yml"]
    files.extend((ROOT / "scripts").glob("*.py"))
    for pattern in ("*.c", "*.h", "*.S", "*.py"):
        files.extend(PROJECT.glob(pattern))
    return {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(files)}


def git_output(*args):
    try:
        result = subprocess.run(["git", *args], cwd=ROOT, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                timeout=10)
        return result.stdout.strip() if result.returncode == 0 else "unavailable"
    except (OSError, subprocess.TimeoutExpired):
        return "unavailable"


def tool_version(command):
    try:
        result = subprocess.run([*command, "--version"], cwd=ROOT, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=10)
        return result.stdout.splitlines()[0] if result.returncode == 0 and result.stdout else "unavailable"
    except (OSError, subprocess.TimeoutExpired):
        return "unavailable"


def terminate_group(process):
    try:
        # Give tracing tools a chance to disable/remove their private instance.
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass  # The child may have exited between timeout/interrupt and killpg.
    try:
        captured, _ = process.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        captured, _ = process.communicate()
    return captured


def run_step(key, title, command, output, timeout=90, env=None, announce_result=True):
    print(f"\n>>> {title}\n$ {shlex.join(map(str, command))}", flush=True)
    started, start = timestamp(), time.monotonic()
    process, captured, code = None, "", 1
    try:
        process = subprocess.Popen(command, cwd=PROJECT, text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   start_new_session=True, env=env)
        captured, _ = process.communicate(timeout=timeout)
        code = process.returncode
    except subprocess.TimeoutExpired:
        captured = terminate_group(process)
        captured += f"\nFAIL: step exceeded {timeout} seconds; process group terminated.\n"
        code = 124
    except KeyboardInterrupt as exc:
        if process is not None:
            captured = terminate_group(process)
        signum = getattr(exc, "signum", signal.SIGINT)
        captured += f"\nFAIL: interrupted by {signal.Signals(signum).name}; process group terminated.\n"
        code = 128 + signum
    except OSError as exc:
        captured = f"FAIL: {exc}\n"
    elapsed = round(time.monotonic() - start, 3)
    status = "PASS" if code == 0 else "FAIL"
    log_name = f"{key}.log"
    header = (f"started: {started}\ncwd: {PROJECT}\n"
              f"command: {shlex.join(map(str, command))}\n\n")
    (output / log_name).write_text(header + captured +
        f"\nexit_code: {code}\nstatus: {status}\nelapsed_seconds: {elapsed}\n",
        encoding="utf-8")
    print(captured, end="" if captured.endswith("\n") else "\n")
    if announce_result:
        print(f"[{status}] {title} ({elapsed:.3f}s)", flush=True)
    return dict(id=key, title=title, status=status, exit_code=code,
                command=list(map(str, command)), cwd=str(PROJECT),
                started_at=started, elapsed_seconds=elapsed, log=log_name)


def skipped(key, title, reason):
    return dict(id=key, title=title, status="SKIP", reason=reason)


def requirement_results(payload):
    """Map requirements to executed checks without treating missing evidence as PASS."""
    by_id = {step["id"]: step for step in payload["steps"]}
    definitions = [
        ("F1", "动态库用户态跳转", ["02-entry-return"]),
        ("F2", "前六个整数类 ABI 参数", ["02-entry-return"]),
        ("F3", "正常 ABI 返回及事件配对", ["02-entry-return"]),
        ("F4", "指令恢复与会话映射回收", ["03-lifecycle"]),
        ("F5", "安全点动态开关", ["03-lifecycle"]),
        ("F6", "至少 16 个不同地址探针", ["04-multiprobe"]),
        ("F7", "多线程、信号及安全点控制", ["05-threads"]),
        ("A1", "同一提交的 x86_64 与 ARM64 原生结果", []),
        ("A2", "单参数条件过滤", ["09-advanced"]),
        ("A3", "CLOCK_MONOTONIC 用户态 / ftrace 真实事件合流", ["12-event-merge"]),
        ("A4", "目标进程内受约束回调", ["10-callback"]),
        ("A5", "安全点配置热更新", ["09-advanced", "10-callback"]),
    ]
    results = []
    for name, scope, dependencies in definitions:
        additional = {"F1": ["06-decoder", "08-aslr"], "F2": ["07-abi", "08-aslr"],
                      "F3": ["07-abi", "08-aslr"], "F7": ["07-abi"],
                      "A3": ["10-event-stream", "event-merge-validation"]}
        dependencies = dependencies + [key for key in additional.get(name, []) if key in by_id]
        checks = [by_id.get(key, {"status": "SKIP"}) for key in dependencies]
        status = ("FAIL" if any(check["status"] == "FAIL" for check in checks) else
                  "PASS" if checks and all(check["status"] == "PASS" for check in checks) else "SKIP")
        results.append(dict(id=name, scope=scope, status=status, checks=dependencies,
            reason="单架构报告；需结合另一架构同提交结果" if name == "A1" else
                   "仅按本次实际执行的检查判定，适用范围见 docs/limitations.md"))
    return results


def write_report(output, payload):
    payload["requirements"] = requirement_results(payload)
    (output / "summary.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    env = payload["environment"]
    lines = ["# LUDP 功能、正确性与性能验证报告", "",
             f"生成时间：{payload['finished_at']}（Asia/Shanghai）。", "",
             f"执行模式：`{payload['mode']}`；所选项目整体结果：**{payload['status']}**。", "",
             f"源码提交：`{env['git_commit']}`；工作树状态及源码 SHA-256 见 `summary.json`。", "",
             f"系统：`{env['platform']}`；Python：`{env['python']}`。", "",
             "本报告汇总所选模式的功能、正确性和量化检查；getpid 性能基准及 A3 合流验证另附专项报告。", "",
             "## 执行结果", "",
             "| 步骤 | 状态 | 耗时（秒） | 证据或原因 |",
             "|---|---|---:|---|"]
    for item in payload["steps"]:
        evidence = f"[原始日志]({item['log']})" if item.get("log") else item.get("reason", "")
        if item.get("log") and item.get("reason"):
            evidence += "；" + item["reason"]
        evidence = evidence.replace("|", "\\|").replace("\n", " ")
        elapsed = f"{item['elapsed_seconds']:.3f}" if "elapsed_seconds" in item else "—"
        lines.append(f"| {item['title']} | {item['status']} | {elapsed} | {evidence} |")
    lines += ["", "## 赛题功能覆盖", "",
              "下表说明本次检查覆盖的能力。PASS 不扩大为任意参数类型、运行时或线程调度的保证。", "",
              "| 条目 | 本次状态 | 验证范围 |", "|---|---|---|"]
    for item in payload["requirements"]:
        scope = item["scope"]
        if item["id"] == "A1":
            scope += "；本报告仅验证一种架构，需查看双架构 CI 汇总"
        lines.append(f"| {item['id']} | {item['status']} | {scope} |")
    lines += ["", "## 结果解释", "",
              "- PASS 仅表示本次执行的相应检查通过；SKIP 表示未测，不计为已完成验收。",
              "- 快速模式不测量性能。完整模式按每轮 1,000,000 次 getpid 调用生成独立基准。",
              f"- 本次验证架构：`{env['machine']}`；A1 完整证据需同一提交的 x86_64 与 ARM64 两份原生运行结果。",
              "- 本目录的日志和 JSON 是运行证据；录屏用于讲解，不替代测量数据。"]
    if (output / "benchmark" / "benchmark.md").exists():
        lines += ["", "[getpid 探针性能基准报告](benchmark/benchmark.md) · "
                  "[性能原始数据](benchmark/benchmark.json) · "
                  "[基准原始输出](benchmark/benchmark.log)"]
    if (output / "event-merge" / "report.md").exists():
        lines += ["", "[A3 用户态与内核事件合流验证报告](event-merge/report.md)"]
    (output / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description="LUDP 一键演示；所有结果均由本机真实运行生成")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--full", action="store_true", help="增加完整回归与百万次 getpid 性能基准")
    mode.add_argument("--correctness-only", action="store_true", help="执行全部正确性验证和演示，不测量性能")
    parser.add_argument("--rounds", type=int, default=5, help="完整模式基准轮数，默认 5")
    parser.add_argument("--require-kernel", action="store_true", help="要求完整模式取得内核 uprobe 实测")
    parser.add_argument("--require-merge", action="store_true", help="要求完整模式取得 A3 真实内核事件合流实测")
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts" / "demo",
                        help="结果父目录；每次新建独立子目录，保留旧结果")
    parser.add_argument("--jobs", type=int, default=2, help="并行构建数，默认 2")
    args = parser.parse_args()
    if not 1 <= args.rounds <= 100 or not 1 <= args.jobs <= 256:
        parser.error("--rounds 必须在 1..100；--jobs 必须在 1..256")
    if (args.require_kernel or args.require_merge) and not args.full:
        parser.error("--require-kernel / --require-merge 必须与 --full 一起使用")

    run_name = dt.datetime.now(TIMEZONE).strftime("%Y%m%d-%H%M%S-%f")
    output = args.output.resolve() / run_name
    output.mkdir(parents=True, exist_ok=False)
    build_variables = dict(CC=os.environ.get("CC") or "cc",
                           AR=os.environ.get("AR") or "ar",
                           CFLAGS=os.environ.get("CFLAGS", DEFAULT_CFLAGS),
                           LDFLAGS=os.environ.get("LDFLAGS", "-pthread"))
    compiler = shlex.split(build_variables["CC"]) or ["cc"]
    build_env = os.environ.copy()
    for name in MAKE_CONTROL_ENV:
        build_env.pop(name, None)
    payload = dict(schema_version=2, started_at=timestamp(),
                   mode="full" if args.full else "correctness-only" if args.correctness_only else "quick", status="FAIL", steps=[],
                   environment=dict(platform=platform.platform(), machine=platform.machine(),
                       python=platform.python_version(), git_commit="unavailable",
                       build_variables=build_variables,
                       input_variables={key: value for key, value in sorted(os.environ.items())
                           if key in ("CC", "AR", "CFLAGS", "CPPFLAGS", "LDFLAGS", *MAKE_CONTROL_ENV)
                           or key.startswith("LUDP_")},
                       ignored_make_control_variables=[key for key in MAKE_CONTROL_ENV if key in os.environ],
                       cpu=next((line.split(":", 1)[1].strip()
                                 for line in read_optional("/proc/cpuinfo").splitlines()
                                 if line.split(":", 1)[0].strip() in ("model name", "Hardware", "CPU part")), "unavailable"),
                       affinity=sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else [],
                       ptrace_scope=read_optional("/proc/sys/kernel/yama/ptrace_scope"),
                       perf_event_paranoid=read_optional("/proc/sys/kernel/perf_event_paranoid"),
                       uprobe_pmu_type=read_optional("/sys/bus/event_source/devices/uprobe/type")))
    steps = payload["steps"]
    previous_handlers = {sig: signal.signal(sig, request_stop) for sig in (signal.SIGINT, signal.SIGTERM)}
    print(f"证据目录：{output}", flush=True)
    try:
        payload["environment"].update(git_commit=git_output("rev-parse", "HEAD"),
            git_status=git_output("status", "--porcelain"),
            compiler=tool_version(compiler), make=tool_version(["make"]))
        problems = []
        if platform.system() != "Linux" or platform.machine() not in ("x86_64", "amd64", "aarch64", "arm64"):
            problems.append("仅支持 Linux x86_64 或 ARM64 原生运行")
        for tool in ("make", (shlex.split(build_variables["AR"]) or ["ar"])[0], compiler[0]):
            if shutil.which(tool) is None:
                problems.append(f"缺少构建工具：{tool}")
        steps.append(dict(id="preflight", title="平台与工具检查",
                          status="FAIL" if problems else "PASS",
                          reason="；".join(problems) if problems else f"Linux {platform.machine()} 构建工具可用"))
        if problems:
            print("\n".join(problems), file=sys.stderr)
            return 1

        payload["source_sha256"] = identify_sources()
        command = ["make", "-B", f"-j{args.jobs}", "all"]
        command += [f"{key}={value}" for key, value in build_variables.items()]
        build = run_step("01-build", "重新构建库、验证程序与演示", command, output, 180, env=build_env)
        steps.append(build)
        payload["source_sha256_after_build"] = identify_sources()
        before, after = payload["source_sha256"], payload["source_sha256_after_build"]
        changed = sorted(path for path in before.keys() | after.keys() if before.get(path) != after.get(path))
        steps.append(dict(id="source-integrity", title="构建前后源码一致性",
                          status="FAIL" if changed else "PASS",
                          reason="构建期间发生变化：" + ", ".join(changed) if changed else "源码 SHA-256 一致"))
        if build["exit_code"] in (130, 143):
            return build["exit_code"]
        if build["status"] != "PASS" or changed:
            return 1
        suites = [
            ("02-entry-return", "F1/F2/F3：入口跳转、六参数与返回值", "probe_demo"),
            ("03-lifecycle", "F4/F5：恢复指令、回收映射与动态开关", "probe_lifecycle_validation"),
            ("04-multiprobe", "F6：16 个地址与同址多探针", "multiprobe_validation"),
            ("05-threads", "F7：多线程与信号交付", "thread_signal_validation"),
        ]
        if args.full or args.correctness_only:
            suites += [
                ("06-decoder", "指令解码边界", "instruction_validation"),
                ("07-abi", "F1/F2/F3：ABI、递归、信号与缓冲区边界", "probe_abi_validation"),
                ("08-api", "公共 API 与 ELF 契约", "api_contract_validation"),
                ("08-aslr", "独立 exec、ASLR 与 PIE 符号定位", "aslr_pie_validation"),
                ("09-advanced", "A2/A3/A5：过滤、事件格式与热更新", "probe_extensions_validation"),
                ("10-callback", "A4：目标进程内回调", "callback_validation"),
                ("10-event-stream", "A3：用户态事件导出与调用配对", "event_stream_demo"),
            ]
        for key, title, binary in suites:
            command = [f"./{binary}"]
            if binary == "event_stream_demo":
                command.append("--standalone")
            result = run_step(key, title, command, output)
            steps.append(result)
            if result["exit_code"] in (130, 143):
                return result["exit_code"]

        if args.full or args.correctness_only:
            result = run_step("event-merge-validation", "事件合流工具的离线正确性检查",
                              [sys.executable, str(ROOT / "scripts/merge_events_validation.py")], output)
            steps.append(result)
            if result["exit_code"] in (130, 143):
                return result["exit_code"]

        correct = all(step["status"] == "PASS" for step in steps)
        if args.full and correct:
            command = [sys.executable, "benchmark.py", "--iterations", "1000000",
                       "--rounds", str(args.rounds), "--output", str(output / "benchmark")]
            if args.require_kernel:
                command.append("--require-kernel")
            benchmark = run_step("11-benchmark", "百万次 getpid 基准与性能阈值", command,
                                 output, 180 * args.rounds + 30)
            steps.append(benchmark)
            if benchmark["exit_code"] in (130, 143):
                return benchmark["exit_code"]
            data_path = output / "benchmark" / "benchmark.json"
            if data_path.exists():
                data = json.loads(data_path.read_text(encoding="utf-8"))
                samples = [sample for sample in data["samples"] if sample["mode"] == "uprobe"]
                complete = data.get("kernel_comparison_complete") is True
                status = "PASS" if complete else "FAIL" if samples or args.require_kernel else "SKIP"
                steps.append(dict(id="kernel-uprobe", title="内核 uprobe 实测", status=status,
                                  reason=f"有效样本 {len(samples)}/{args.rounds}；" +
                                  "；".join(data.get("skipped", [])),
                                  log="benchmark/benchmark.log"))
            else:
                steps.append(dict(id="kernel-uprobe", title="内核 uprobe 实测", status="FAIL",
                                  reason="基准未生成可核验的数据文件"))
        else:
            reason = ("仅正确性模式不测量性能" if args.correctness_only else
                      "快速模式未执行；使用 --full" if not args.full else
                      "正确性检查失败，未执行性能测量")
            steps.append(skipped("benchmark", "百万次 getpid 基准与性能阈值", reason))
            steps.append(skipped("kernel-uprobe", "内核 uprobe 实测", reason))
        if args.full and correct:
            command = [sys.executable, str(ROOT / "scripts/event_merge_demo.py"),
                       "--output", str(output / "event-merge")]
            if args.require_merge:
                command.append("--require-kernel")
            result = run_step("12-event-merge", "A3：用户态与 ftrace 真实事件合流", command, output, 120,
                              announce_result=False)
            if result["status"] == "PASS":
                merge_path = output / "event-merge" / "summary.json"
                if not merge_path.exists():
                    result.update(status="FAIL", reason="未生成合流验证摘要")
                else:
                    merge = json.loads(merge_path.read_text(encoding="utf-8"))
                    merge_status = merge.get("status")
                    if merge_status not in ("PASS", "SKIP") or (args.require_merge and merge_status != "PASS"):
                        result.update(status="FAIL", reason="合流验证未满足要求")
                    else:
                        result.update(status=merge_status, reason=merge.get("reason", ""))
            steps.append(result)
            print(f"[{result['status']}] {result['title']} ({result['elapsed_seconds']:.3f}s)", flush=True)
            if result["exit_code"] in (130, 143):
                return result["exit_code"]
        else:
            steps.append(skipped("12-event-merge", "A3：用户态与 ftrace 真实事件合流",
                                 "需要 --full 且所有前置正确性检查通过"))
        payload["status"] = "FAIL" if any(step["status"] == "FAIL" for step in steps) else "PASS"
        return 0 if payload["status"] == "PASS" else 1
    except KeyboardInterrupt as exc:
        signum = getattr(exc, "signum", signal.SIGINT)
        steps.append(dict(id="interrupted", title="演示中断", status="FAIL",
                          reason=f"收到 {signal.Signals(signum).name}"))
        return 128 + signum
    except (OSError, ValueError, KeyError) as exc:
        steps.append(dict(id="runner", title="演示执行器", status="FAIL", reason=str(exc)))
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    finally:
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        payload["finished_at"] = timestamp()
        write_report(output, payload)
        print(f"\n所选检查：{payload['status']}；报告：{output / 'report.md'}", flush=True)
        for sig, handler in previous_handlers.items():
            signal.signal(sig, handler)


if __name__ == "__main__":
    sys.exit(main())

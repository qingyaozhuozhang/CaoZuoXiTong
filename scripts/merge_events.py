#!/usr/bin/env python3
"""Merge supported perf-script/ftrace text records using exact integer times.

All inputs must already use CLOCK_MONOTONIC from the same host and boot.
This tool does not guess or repair clock domains, synthesize missing events,
or claim that the resulting text can be imported as a binary perf.data file.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import re


PERF = re.compile(r"^\s*(?P<comm>.+?)\s+(?P<pid>\d+)(?:/(?P<tid>\d+))?"
                  r"\s+\[(?P<cpu>\d+)\]\s+(?P<sec>\d+)\.(?P<frac>\d{1,9}):"
                  r"\s+(?P<event>\S+):\s*(?P<fields>.*)$")
FTRACE = re.compile(r"^\s*(?P<comm>.+)-(?P<tid>\d+)\s+"
                    r"(?:\(\s*(?P<pid>\d+|-----)\s*\)\s+)?"
                    r"\[(?P<cpu>\d+)\]\s+(?:\S+\s+)?"
                    r"(?P<sec>\d+)\.(?P<frac>\d{1,9}):"
                    r"\s+(?P<body>.*)$")
FTRACE_EVENT = re.compile(r"(?P<event>\S+):\s*(?P<fields>.*)")
GETPID_SYSCALL = re.compile(r"sys_getpid\(\s*\)")


@dataclass(frozen=True)
class Event:
    monotonic_ns: int
    source: str
    comm: str
    pid: int | None
    tid: int
    cpu: int
    name: str
    fields: str
    precision_ns: int

    def perf_line(self):
        # A ftrace record without TGID only establishes TID. Do not invent PID.
        identity = str(self.tid) if self.pid is None else f"{self.pid}/{self.tid}"
        sec, ns = divmod(self.monotonic_ns, 1_000_000_000)
        return (f"{self.comm:<16.16} {identity} [{self.cpu:03}] "
                f"{sec}.{ns:09}: {self.name}: {self.fields}\n")


def parse_events(text, source, format_name):
    if format_name not in {"perf", "ftrace"}:
        raise ValueError(f"unsupported format: {format_name}")
    regex = PERF if format_name == "perf" else FTRACE
    result = []
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        match = regex.fullmatch(line)
        if not match:
            raise ValueError(f"{source}:{number}: unsupported or malformed event: {line[:120]}")
        value = match.groupdict()
        if format_name == "ftrace":
            # Linux kernel/trace/trace_syscalls.c has a dedicated human text
            # printer: sys_enter_getpid is normally rendered as sys_getpid(),
            # without the generic event_name: prefix. Preserve that raw trace
            # separately and normalize its known event identity for merging.
            body = value["body"].strip()
            event_match = FTRACE_EVENT.fullmatch(body)
            if event_match:
                value.update(event_match.groupdict())
            elif GETPID_SYSCALL.fullmatch(body):
                value.update(event="sys_enter_getpid", fields=body)
            else:
                raise ValueError(f"{source}:{number}: unsupported ftrace event body: {body[:120]}")
        frac = value["frac"]
        ns = int(value["sec"]) * 1_000_000_000 + int(frac.ljust(9, "0"))
        pid = int(value["pid"]) if value.get("pid", "-----") not in {None, "-----"} else None
        tid = int(value.get("tid") or value["pid"])
        name = value["event"]
        if format_name == "ftrace" and name == "syscalls:sys_enter_getpid":
            name = "sys_enter_getpid"
        if format_name == "ftrace" and ":" not in name:
            name = "ftrace:" + name
        result.append(Event(ns, source, value["comm"].strip(), pid, tid,
                            int(value["cpu"]), name, value["fields"],
                            10 ** (9 - len(frac))))
    return result


def merge_events(*streams):
    # Stable ties preserve each source's input order, without claiming which
    # source occurred first below the timestamp's recorded precision.
    return sorted((event for stream in streams for event in stream),
                  key=lambda event: event.monotonic_ns)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--perf", type=Path, action="append", default=[], help="perf-script style CLOCK_MONOTONIC text")
    parser.add_argument("--ftrace", type=Path, action="append", default=[], help="ftrace trace captured with trace_clock=mono")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.perf and not args.ftrace:
        parser.error("at least one --perf or --ftrace input is required")
    try:
        sources = [(path, "perf") for path in args.perf] + [(path, "ftrace") for path in args.ftrace]
        if any(path.resolve() == args.output.resolve() for path, _ in sources):
            raise ValueError("output must not overwrite an input")
        streams = [parse_events(path.read_text(encoding="utf-8"), str(path), fmt)
                   for path, fmt in sources]
        merged = merge_events(*streams)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text("# clock: CLOCK_MONOTONIC; caller must verify same host/boot\n" +
                               "".join(event.perf_line() for event in merged), encoding="utf-8")
        print(json.dumps({"status": "PASS", "events": len(merged), "output": str(args.output)}))
    except (OSError, ValueError) as exc:
        parser.exit(1, f"merge failed: {exc}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

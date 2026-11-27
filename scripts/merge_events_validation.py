#!/usr/bin/env python3
"""Offline parser/merge validation using explicitly synthetic fixtures.

No fixture here is evidence of actual kernel collection or A3 completion.
"""

import unittest
from unittest.mock import MagicMock, call, patch
import subprocess

from event_merge_demo import TraceUnavailable, stop_collector, trace_instance, verify_streams
from merge_events import merge_events, parse_events


class EventMergeValidation(unittest.TestCase):
    def pair(self):
        return parse_events("worker 12/12 [004] 7.000001000: ludp:getpid_entry: probe_id=1 call_id=1\n"
                            "worker 12/12 [004] 7.000004000: ludp:getpid_return: call_id=1 retval=0xc\n", "synthetic-ludp", "perf")

    def kernel(self, ts="7.000002", tid=12, cpu=4):
        return parse_events(f"worker-thread-{tid} [{cpu:03}] .... {ts}: sys_enter_getpid: ()\n",
                            "synthetic-kernel", "ftrace")

    def metadata(self):
        return {"calls": 1, "pid": 12, "cpu": 4, "begin_ns": 7_000_000_000, "end_ns": 7_001_000_000}

    def test_exact_nanoseconds_and_realistic_ftrace_header(self):
        event = parse_events("# tracer: nop\nworker-12 ( 12) [004] d... 9999999.000000001: sys_enter_getpid: ()\n", "fixture", "ftrace")[0]
        self.assertEqual(event.monotonic_ns, 9_999_999_000_000_001)
        self.assertEqual((event.pid, event.tid, event.cpu, event.precision_ns), (12, 12, 4, 1))

    def test_merge_and_round_trip_preserve_clock_and_identity(self):
        merged = merge_events(self.pair(), self.kernel())
        self.assertEqual([e.source for e in merged], ["synthetic-ludp", "synthetic-kernel", "synthetic-ludp"])
        reread = parse_events("".join(e.perf_line() for e in merged), "round-trip", "perf")
        self.assertEqual([(e.monotonic_ns, e.tid, e.cpu, e.name) for e in reread],
                         [(e.monotonic_ns, e.tid, e.cpu, e.name) for e in merged])
        self.assertIsNone(merged[1].pid)  # ftrace did not provide a TGID.

    def test_submicrosecond_ties_are_stable(self):
        a, b = self.kernel(), self.kernel()
        self.assertIs(merge_events(a, b)[0], a[0])

    def test_valid_capture_and_measured_skew(self):
        result = verify_streams(self.pair(), self.kernel(), self.metadata(), 100_000)
        self.assertEqual(result["maximum_interval_skew_ns"], 0)
        result = verify_streams(self.pair(), self.kernel("7.000000"), self.metadata(), 100_000)
        self.assertEqual(result["maximum_interval_skew_ns"], 1000)

    def test_wrong_clock_domain_fails(self):
        with self.assertRaisesRegex(ValueError, "outside"):
            verify_streams(self.pair(), self.kernel("1007.000002"), self.metadata(), 100_000)

    def test_missing_kernel_event_fails(self):
        with self.assertRaisesRegex(ValueError, "expected 1 real"):
            verify_streams(self.pair(), [], self.metadata(), 100_000)

    def test_wrong_identity_or_cpu_fails(self):
        for stream in (self.kernel(tid=13), self.kernel(cpu=5)):
            with self.assertRaisesRegex(ValueError, "TID/CPU"):
                verify_streams(self.pair(), stream, self.metadata(), 100_000)

    def test_malformed_or_lost_event_not_silently_discarded(self):
        for line in ("CPU:4 [LOST 2 EVENTS]", "worker-12 [004] .... 7.1234567890: sys_enter_getpid: ()", "not an event"):
            with self.assertRaises(ValueError):
                parse_events(line, "fixture", "ftrace")

    def test_event_count_not_silently_truncated(self):
        with self.assertRaisesRegex(ValueError, "expected 1 real"):
            verify_streams(self.pair(), self.kernel() * 2, self.metadata(), 100_000)

    def test_linux_syscall_specific_printer_is_normalized(self):
        # Shape taken from the Linux trace_syscalls.c print_syscall_enter
        # printer. The numbers remain synthetic; this is not a live capture.
        stream = parse_events("event_stream_de-12 [004] d... 7.000002: sys_getpid()\n", "linux-printer-shape", "ftrace")
        self.assertEqual(stream[0].name, "ftrace:sys_enter_getpid")
        self.assertEqual(verify_streams(self.pair(), stream, self.metadata(), 100_000)["calls_verified"], 1)

    def test_mismatched_and_duplicate_call_ids_fail(self):
        from dataclasses import replace
        pair = self.pair()
        pair[1] = replace(pair[1], fields="call_id=2 retval=0xc")
        with self.assertRaisesRegex(ValueError, "mismatched call_id"):
            verify_streams(pair, self.kernel(), self.metadata(), 100_000)
        pair = self.pair()
        repeated = pair + [replace(event, monotonic_ns=event.monotonic_ns + 10_000) for event in pair]
        kernels = self.kernel() + self.kernel("7.000012")
        metadata = dict(self.metadata(), calls=2)
        with self.assertRaisesRegex(ValueError, "duplicate call_id"):
            verify_streams(repeated, kernels, metadata, 100_000)


class CaptureCleanupValidation(unittest.TestCase):
    """Fault injection for ownership/cleanup, not a simulated A3 PASS result."""

    def instance_fixture(self):
        root, instance = MagicMock(), MagicMock()
        root.__truediv__.return_value.__truediv__.return_value = instance
        nodes = {}
        def child(name):
            return nodes.setdefault(name, MagicMock())
        instance.__truediv__.side_effect = child
        child("trace_clock").read_text.side_effect = ["[local] mono", "local [mono]"]
        child("events/syscalls/sys_enter_getpid/enable").is_file.return_value = True
        return root, instance, nodes

    def test_collector_failure_still_disables_and_removes_owned_instance(self):
        root, instance, nodes = self.instance_fixture()
        with patch("event_merge_demo.write_control") as write:
            with self.assertRaisesRegex(RuntimeError, "injected collection failure"):
                with trace_instance(root):
                    raise RuntimeError("injected collection failure")
            self.assertEqual(write.call_args_list[-2:],
                             [call(nodes["tracing_on"], "0"), call(nodes["events/enable"], "0")])
        instance.rmdir.assert_called_once_with()

    def test_missing_kernel_event_is_skip_but_instance_is_removed(self):
        root, instance, nodes = self.instance_fixture()
        nodes["events/syscalls/sys_enter_getpid/enable"].is_file.return_value = False
        with patch("event_merge_demo.write_control"):
            with self.assertRaises(TraceUnavailable):
                with trace_instance(root):
                    self.fail("unavailable instance must not reach capture")
        instance.rmdir.assert_called_once_with()

    def test_failed_creation_does_not_remove_an_unowned_instance(self):
        root, instance, _ = self.instance_fixture()
        instance.mkdir.side_effect = PermissionError(13, "denied")
        with self.assertRaises(TraceUnavailable):
            with trace_instance(root):
                self.fail("failed creation must not reach capture")
        instance.rmdir.assert_not_called()

    def test_cleanup_failure_is_not_reported_as_success(self):
        root, instance, _ = self.instance_fixture()
        instance.rmdir.side_effect = OSError("injected cleanup failure")
        with patch("event_merge_demo.write_control"):
            with self.assertRaisesRegex(OSError, "injected cleanup failure"):
                with trace_instance(root):
                    pass

    def test_stuck_collector_terminates_only_its_own_process(self):
        process = MagicMock()
        process.poll.return_value = None
        process.communicate.side_effect = [subprocess.TimeoutExpired("collector", 5), ("", None)]
        stop_collector(process)
        process.kill.assert_called_once_with()
        self.assertEqual(process.communicate.call_count, 2)


if __name__ == "__main__":
    print("Offline synthetic-fixture validation; no real kernel capture is performed.", flush=True)
    unittest.main(verbosity=2)

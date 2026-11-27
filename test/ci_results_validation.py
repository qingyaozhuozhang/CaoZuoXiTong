#!/usr/bin/env python3
"""Offline regression checks for networking and atomic CI-result synchronization.

All synthetic runs and artifacts stay in TemporaryDirectory instances. No
GitHub connection is made and the repository's real test/ is never written.
"""
from __future__ import annotations

import importlib.util
import http.client
import io
import json
from pathlib import Path
import ssl
import stat
import sys
import tempfile
import unittest
from unittest.mock import patch
import urllib.error
import urllib.request
import zipfile


SPEC = importlib.util.spec_from_file_location(
    "ludp_ci_sync", Path(__file__).with_name("sync_ci_results.py"))
SYNC = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SYNC)
REPO = "fixture/repository"


def zip_payload(entries):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in entries:
            archive.writestr(name, data)
    return buffer.getvalue()


def run_fixture(run_id=101, attempt=1, conclusion="success"):
    return {
        "id": run_id, "run_attempt": attempt, "head_sha": f"{run_id:040x}",
        "head_branch": "fixture-branch", "event": "push", "status": "completed",
        "conclusion": conclusion, "path": SYNC.WORKFLOW,
        "html_url": f"https://github.com/{REPO}/actions/runs/{run_id}",
        "updated_at": "2026-01-01T00:00:00Z",
    }


class FixtureAPI:
    """Only the explicit in-memory fixture routes are permitted."""

    def __init__(self, run, included=None):
        self.run = run
        self.artifacts = []
        self.payloads = {}
        self.calls = []
        self.jobs = []
        included = SYNC.MATRIX if included is None else included
        for index, (arch, profile) in enumerate(included, start=1):
            artifact_id = run["id"] * 100 + run["run_attempt"] * 10 + index
            self.artifacts.append({
                "id": artifact_id, "expired": False,
                "name": f"ludp-{arch}-{profile}-{run['id']}-{run['run_attempt']}",
            })
            evidence = {"fixture": True, "architecture": arch, "profile": profile,
                        "run_id": run["id"], "attempt": run["run_attempt"],
                        "status": "PASS",
                        "mode": "full" if profile == "full" else "correctness-only",
                        "environment": {"git_commit": run["head_sha"],
                                        "machine": "aarch64" if arch == "arm64" else arch}}
            self.payloads[self.artifact_route(artifact_id)] = zip_payload([
                ("ci/fixture-run/summary.json", json.dumps(evidence)),
                ("ci/fixture-run/report.md", "Synthetic fixture report\n"),
                ("ci/fixture-run/01-build.log", "Synthetic fixture build output\n"),
                ("runner/environment.log", f"Synthetic fixture: {arch}\n"),
            ])
            self.jobs.append({"id": artifact_id + 100000, "name": f"{arch} / {profile}",
                              "status": "completed", "conclusion": "success"})

    @staticmethod
    def artifact_route(artifact_id):
        return f"/repos/{REPO}/actions/artifacts/{artifact_id}/zip"

    def pages(self, path, key):
        self.calls.append(("pages", path, key))
        base = f"/repos/{REPO}/actions/runs/{self.run['id']}"
        if path == f"{base}/attempts/{self.run['run_attempt']}/jobs" and key == "jobs":
            return self.jobs
        if path == f"{base}/artifacts" and key == "artifacts":
            return self.artifacts
        raise AssertionError(f"Unexpected fixture pagination route: {path}")

    def request(self, path, binary=False):
        self.calls.append(("request", path, binary))
        if not binary or path not in self.payloads:
            raise AssertionError(f"Unexpected fixture download route: {path}")
        result = self.payloads[path]
        if isinstance(result, BaseException):
            raise result
        return result


class CIResultsValidation(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ludp-ci-validation-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.package = self.root / "test"
        self.package.mkdir()
        self.destination = self.package / "results"
        self.package_files = {
            "README.md": b"Fixture package documentation\n",
            "__init__.py": b'"""Fixture package."""\n',
            "__main__.py": b'print("Fixture CLI")\n',
            "sync_ci_results.py": b'"""Fixture synchronization source."""\n',
            "ci_results_validation.py": b'"""Fixture validation source."""\n',
        }
        for name, content in self.package_files.items():
            (self.package / name).write_bytes(content)
        self.addCleanup(patch.stopall)
        patch.object(SYNC, "ROOT", self.root).start()
        patch.object(SYNC, "PACKAGE_ROOT", self.package).start()
        patch.object(SYNC, "DEST", self.destination).start()
        patch.object(SYNC, "progress").start()
        patch.object(urllib.request.OpenerDirector, "open",
                     side_effect=AssertionError("Network is forbidden in offline validation")).start()

    def tearDown(self):
        # Every synchronization scenario must preserve the installed tool.
        for name, content in self.package_files.items():
            self.assertEqual((self.package / name).read_bytes(), content)

    def synchronize(self, run=None, api=None):
        run = run_fixture() if run is None else run
        api = FixtureAPI(run) if api is None else api
        return SYNC.synchronize(api, REPO, run)

    def snapshot(self):
        return {str(path.relative_to(self.destination)): path.read_bytes()
                for path in self.destination.rglob("*") if path.is_file()}

    def metadata(self):
        return json.loads((self.destination / "latest.json").read_text(encoding="utf-8"))

    def assert_no_temporary_results(self):
        self.assertEqual(list(self.root.glob(".ci-results-*")), [])
        self.assertEqual(list(self.package.glob(".ci-results-*")), [])

    def symlink_or_skip(self, link, target, **kwargs):
        """Create a symlink, or explicitly skip when the host forbids them."""
        try:
            link.symlink_to(target, **kwargs)
        except (NotImplementedError, OSError) as exc:
            self.skipTest(f"symbolic links are unavailable on this host: {exc}")

    def test_complete_run_preserves_four_artifacts_and_source_identity(self):
        run = run_fixture()
        self.assertEqual(self.synchronize(run), 0)
        metadata = self.metadata()
        self.assertEqual(metadata["run"]["head_sha"], run["head_sha"])
        self.assertEqual(metadata["run"]["run_attempt"], 1)
        self.assertEqual(metadata["missing_artifacts"], [])
        self.assertEqual(len(metadata["artifacts"]), 4)
        self.assertIn("101", (self.destination / "report.md").read_text(encoding="utf-8"))
        self.assertFalse((self.destination / "README.md").exists())
        for arch, profile in SYNC.MATRIX:
            directory = self.destination / arch / profile
            summary = json.loads(
                (directory / "ci/fixture-run/summary.json").read_text(encoding="utf-8"))
            self.assertEqual((summary["architecture"], summary["profile"]), (arch, profile))
            self.assertTrue((directory / "runner/environment.log").is_file())
            self.assertTrue((directory / "ci/fixture-run/01-build.log").is_file())
        self.assert_no_temporary_results()

    def test_invalid_report_is_retained_but_returns_failure(self):
        run = run_fixture()
        for issue in ("missing", "commit", "machine", "source"):
            with self.subTest(issue=issue):
                api = FixtureAPI(run)
                route = api.artifact_route(api.artifacts[0]["id"])
                entries = [("ci/fixture-run/report.md", "Fixture report")]
                evidence = {
                    "mode": "full", "status": "PASS",
                    "environment": {"git_commit": run["head_sha"], "machine": "x86_64"},
                }
                if issue == "commit":
                    evidence["environment"]["git_commit"] = "0" * 40
                if issue == "machine":
                    evidence["environment"]["machine"] = "aarch64"
                if issue == "source":
                    evidence.update(source_sha256={"file": "before"},
                                    source_sha256_after_build={"file": "after"})
                if issue != "missing":
                    entries.append(("ci/fixture-run/summary.json", json.dumps(evidence)))
                api.payloads[route] = zip_payload(entries)
                self.assertEqual(self.synchronize(run, api), 1)
                result = self.metadata()["report_checks"]["x86_64/full"]
                self.assertEqual(result["status"], "FAIL")
                self.assertTrue(result["issue"])
                self.assertTrue((self.destination / "x86_64/full/ci/fixture-run/report.md").is_file())
                self.assert_no_temporary_results()

    def test_zip_digest_mismatch_keeps_previous_result(self):
        self.assertEqual(self.synchronize(), 0)
        previous = self.snapshot()
        run = run_fixture(105)
        api = FixtureAPI(run)
        api.artifacts[0]["digest"] = "sha256:" + "0" * 64
        with self.assertRaisesRegex(RuntimeError, "ZIP"):
            self.synchronize(run, api)
        self.assertEqual(self.snapshot(), previous)
        self.assert_no_temporary_results()

    def test_new_run_replaces_all_previous_files(self):
        self.assertEqual(self.synchronize(), 0)
        (self.destination / "stale-pass.md").write_text("Previous run only\n", encoding="utf-8")
        obsolete = self.destination / "arm64/full/obsolete"
        obsolete.mkdir()
        (obsolete / "passed.log").write_text("Previous run only\n", encoding="utf-8")
        self.assertEqual(self.synchronize(run_fixture(102)), 0)
        self.assertFalse((self.destination / "stale-pass.md").exists())
        self.assertFalse(obsolete.exists())
        self.assertEqual(self.metadata()["run"]["id"], 102)
        self.assert_no_temporary_results()

    def test_failed_missing_architecture_removes_old_success_and_retains_job_log(self):
        self.assertEqual(self.synchronize(), 0)
        run = run_fixture(103, conclusion="failure")
        api = FixtureAPI(run, included=[("x86_64", "full")])
        api.jobs.append({"id": 500, "name": "arm64 / full", "status": "completed",
                         "conclusion": "failure"})
        api.payloads[f"/repos/{REPO}/actions/jobs/500/logs"] = b"Synthetic compiler failure\n"
        self.assertEqual(self.synchronize(run, api), 1)
        self.assertFalse((self.destination / "arm64").exists())
        self.assertFalse((self.destination / "x86_64/ubsan").exists())
        self.assertEqual(len(self.metadata()["missing_artifacts"]), 3)
        self.assertEqual(self.metadata()["run"]["conclusion"], "failure")
        self.assertEqual((self.destination / "failed-jobs/500.log").read_bytes(),
                         b"Synthetic compiler failure\n")
        self.assert_no_temporary_results()

    def test_download_or_archive_failure_keeps_previous_result_byte_for_byte(self):
        self.assertEqual(self.synchronize(), 0)
        previous = self.snapshot()
        for failure in (urllib.error.URLError("Synthetic disconnection"), b"Not a ZIP archive"):
            with self.subTest(failure=type(failure).__name__):
                run = run_fixture(104)
                api = FixtureAPI(run)
                second = api.artifacts[1]["id"]
                api.payloads[api.artifact_route(second)] = failure
                with self.assertRaises((urllib.error.URLError, zipfile.BadZipFile)):
                    self.synchronize(run, api)
                self.assertEqual(self.snapshot(), previous)
                self.assert_no_temporary_results()

    def test_exhausted_network_retries_keep_previous_result_byte_for_byte(self):
        self.assertEqual(self.synchronize(), 0)
        previous = self.snapshot()
        run = run_fixture(104)
        fixture = FixtureAPI(run)
        second_route = fixture.artifact_route(fixture.artifacts[1]["id"])
        with patch.dict(SYNC.os.environ, {"GH_TOKEN": "fixture-token"}, clear=True), \
                patch.object(SYNC.time, "sleep"), patch.object(SYNC, "progress"):
            network = SYNC.GitHub(request_timeout=2, retries=1, proxy="")
            original_request = fixture.request

            def download(path, binary=False):
                if path == second_route:
                    return network.request(path, binary=binary)
                return original_request(path, binary=binary)

            fixture.request = download
            with patch.object(network.opener, "open", side_effect=ConnectionResetError(104, "Fixture reset")) as opened:
                with self.assertRaisesRegex(RuntimeError, "已尝试 2 次"):
                    self.synchronize(run, fixture)
                self.assertEqual(opened.call_count, 2)
        self.assertEqual(self.snapshot(), previous)
        self.assert_no_temporary_results()

    def test_unsafe_archive_paths_and_symlinks_are_rejected(self):
        self.assertEqual(self.synchronize(), 0)
        previous = self.snapshot()
        symlink = zipfile.ZipInfo("ci/symlink")
        symlink.create_system = 3
        symlink.external_attr = (stat.S_IFLNK | 0o777) << 16
        unsafe_names = ["../escape", "/absolute", "ci/../../escape", symlink]
        # zipfile normalizes backslashes to '/' on Windows before extraction,
        # so the raw-backslash case is meaningful only on hosts that preserve it.
        if SYNC.os.sep != "\\":
            unsafe_names.append("ci\\escape")
        for name in unsafe_names:
            label = name.filename if isinstance(name, zipfile.ZipInfo) else name
            with self.subTest(name=label):
                run = run_fixture(105)
                api = FixtureAPI(run)
                first = api.artifacts[0]["id"]
                api.payloads[api.artifact_route(first)] = zip_payload([(name, "unsafe")])
                with self.assertRaises(RuntimeError):
                    self.synchronize(run, api)
                self.assertEqual(self.snapshot(), previous)
                self.assertFalse((self.root / "escape").exists())
                self.assert_no_temporary_results()

    def test_failed_install_rename_restores_previous_directory(self):
        self.assertEqual(self.synchronize(), 0)
        previous = self.snapshot()
        staged = self.package / ".ci-results-candidate"
        staged.mkdir()
        (staged / "latest.json").write_text("Synthetic replacement\n", encoding="utf-8")
        original_rename = Path.rename

        def fail_candidate_install(source, target):
            if source == staged and Path(target) == self.destination:
                raise OSError("Synthetic install rename failure")
            return original_rename(source, target)

        with patch.object(Path, "rename", fail_candidate_install):
            with self.assertRaisesRegex(OSError, "install rename failure"):
                SYNC.replace_results(staged)
        self.assertEqual(self.snapshot(), previous)
        SYNC.shutil.rmtree(staged)
        self.assert_no_temporary_results()

    def test_failed_rollback_keeps_previous_result_in_reported_backup(self):
        self.assertEqual(self.synchronize(), 0)
        previous = self.snapshot()
        original_rename = Path.rename

        def fail_install_and_restore(source, target):
            if Path(target) == self.destination:
                raise OSError("Synthetic install and rollback failure")
            return original_rename(source, target)

        with patch.object(Path, "rename", fail_install_and_restore):
            with self.assertRaisesRegex(RuntimeError, "旧结果保留在") as raised:
                self.synchronize(run_fixture(102))
        backups = list(self.package.glob(".ci-results-old-*"))
        self.assertEqual(len(backups), 1)
        backup = backups[0]
        self.assertIn(str(backup), str(raised.exception))
        self.assertEqual({str(path.relative_to(backup)): path.read_bytes()
                          for path in backup.rglob("*") if path.is_file()}, previous)
        self.assertFalse(self.destination.exists())
        self.assertEqual(list(self.package.glob(".ci-results-new-*")), [])

    def test_staging_and_backups_stay_inside_package(self):
        original_mkdtemp = tempfile.mkdtemp
        with patch.object(SYNC.tempfile, "mkdtemp", wraps=original_mkdtemp) as created:
            self.assertEqual(self.synchronize(), 0)
            self.assertEqual(self.synchronize(run_fixture(102)), 0)
        self.assertEqual(created.call_count, 4)
        self.assertTrue(all(Path(call.kwargs["dir"]) == self.package
                            for call in created.call_args_list))
        self.assert_no_temporary_results()

    def test_partial_rerun_never_reuses_previous_attempt_artifacts(self):
        self.assertEqual(self.synchronize(), 0)
        run = run_fixture(101, attempt=2)
        current = FixtureAPI(run, included=[("arm64", "full")])
        old = FixtureAPI(run_fixture(101, attempt=1))
        current.artifacts.extend(old.artifacts)
        current.payloads.update(old.payloads)
        self.assertEqual(self.synchronize(run, current), 1)
        metadata = self.metadata()
        self.assertEqual(metadata["run"]["run_attempt"], 2)
        self.assertEqual(len(metadata["artifacts"]), 1)
        self.assertEqual(len(metadata["missing_artifacts"]), 3)
        self.assertFalse((self.destination / "x86_64").exists())
        self.assertFalse((self.destination / "arm64/ubsan").exists())
        requested = [call[1] for call in current.calls if call[0] == "request"]
        self.assertEqual(requested, [current.artifact_route(current.artifacts[0]["id"])])
        self.assert_no_temporary_results()

    def test_unmanaged_destination_is_not_overwritten(self):
        self.destination.mkdir()
        (self.destination / "user-data.txt").write_text(
            "Preserve user content\n", encoding="utf-8")
        previous = self.snapshot()
        with self.assertRaises(RuntimeError):
            self.synchronize()
        self.assertEqual(self.snapshot(), previous)
        self.assert_no_temporary_results()

    def test_symlink_destination_is_not_followed(self):
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "user-data.txt").write_text(
            "Preserve outside content\n", encoding="utf-8")
        (outside / SYNC.MARKER).write_text("Synthetic marker\n", encoding="utf-8")
        self.symlink_or_skip(self.destination, outside, target_is_directory=True)
        with self.assertRaisesRegex(RuntimeError, "符号链接"):
            self.synchronize()
        self.assertTrue(self.destination.is_symlink())
        self.assertEqual((outside / "user-data.txt").read_text(encoding="utf-8"),
                         "Preserve outside content\n")
        self.assert_no_temporary_results()

    def test_plain_file_destination_is_not_replaced(self):
        self.destination.write_text("Preserve file\n", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "不是目录"):
            self.synchronize()
        self.assertEqual(self.destination.read_text(encoding="utf-8"), "Preserve file\n")
        self.assert_no_temporary_results()

    def test_symlink_marker_does_not_authorize_replacement(self):
        self.destination.mkdir()
        (self.destination / "user-data.txt").write_text(
            "Preserve user content\n", encoding="utf-8")
        self.symlink_or_skip(self.destination / SYNC.MARKER, self.package / "README.md")
        previous = self.snapshot()
        with self.assertRaisesRegex(RuntimeError, "非本工具管理"):
            self.synchronize()
        self.assertEqual(self.snapshot(), previous)
        self.assert_no_temporary_results()

    def test_existing_empty_results_directory_can_be_initialized(self):
        self.destination.mkdir()
        self.assertEqual(self.synchronize(), 0)
        self.assertEqual(self.metadata()["run"]["id"], 101)
        self.assert_no_temporary_results()

    def test_redirect_drops_credentials_when_host_changes(self):
        handler = SYNC.SafeRedirect()
        request = urllib.request.Request(
            "https://api.github.com/artifact", headers={"Authorization": "Bearer fixture-token"})
        redirected = handler.redirect_request(
            request, None, 302, "Found", {}, "https://artifact.example.invalid/report.zip")
        self.assertIsNone(redirected.get_header("Authorization"))
        same_host = handler.redirect_request(
            request, None, 302, "Found", {}, "https://api.github.com/another-artifact")
        self.assertEqual(same_host.get_header("Authorization"), "Bearer fixture-token")
        with self.assertRaises(RuntimeError):
            handler.redirect_request(request, None, 302, "Found", {}, "http://artifact.example.invalid/file")


class FixtureResponse(io.BytesIO):
    def __init__(self, body=b'{"ok": true}', headers=None, read_error=None):
        super().__init__(body)
        self.headers = {} if headers is None else headers
        self.read_error = read_error

    def read(self, size=-1):
        if self.read_error is not None:
            raise self.read_error
        return super().read(size)


class NetworkValidation(unittest.TestCase):
    """Exercise the real request loop with synthetic open/read responses only."""

    def setUp(self):
        self.addCleanup(patch.stopall)
        patch.dict(SYNC.os.environ, {"GH_TOKEN": "fixture-secret-token"}, clear=True).start()
        patch.object(SYNC.shutil, "which", return_value=None).start()
        patch.object(urllib.request.OpenerDirector, "open",
                     side_effect=AssertionError("Network is forbidden in offline validation")).start()
        self.sleep = patch.object(SYNC.time, "sleep").start()
        self.stderr = io.StringIO()
        patch.object(SYNC.sys, "stderr", self.stderr).start()

    def api(self, **kwargs):
        return SYNC.GitHub(**kwargs)

    def test_reset_is_retried_with_configured_timeout_and_visible_progress(self):
        api = self.api(request_timeout=2.5, retries=2)
        reset = urllib.error.URLError(ConnectionResetError(104, "Fixture reset"))
        with patch.object(api.opener, "open", side_effect=[reset, FixtureResponse()]) as opened:
            self.assertEqual(api.request("/fixture?secret=signed-query"), {"ok": True})
        self.assertEqual(opened.call_count, 2)
        self.assertTrue(all(call.kwargs["timeout"] == 2.5 for call in opened.call_args_list))
        self.sleep.assert_called_once_with(1)
        output = self.stderr.getvalue()
        self.assertIn("[请求 1/3]", output)
        self.assertIn("[请求 2/3]", output)
        self.assertNotIn("fixture-secret-token", output)
        self.assertNotIn("signed-query", output)

    def test_response_read_failure_retries_the_entire_download(self):
        failures = [http.client.IncompleteRead(b"partial", 10),
                    ConnectionResetError(104, "Fixture reset"), TimeoutError("Fixture timeout"),
                    http.client.RemoteDisconnected("Fixture closed")]
        for failure in failures:
            with self.subTest(failure=type(failure).__name__):
                api = self.api(retries=1)
                partial = FixtureResponse(read_error=failure)
                with patch.object(api.opener, "open", side_effect=[partial, FixtureResponse(b"complete")]) as opened:
                    self.assertEqual(api.request("/artifact", binary=True), b"complete")
                self.assertEqual(opened.call_count, 2)
                self.assertTrue(partial.closed)

    def test_short_content_length_response_retries_instead_of_installing_partial_bytes(self):
        api = self.api(retries=1)
        partial = FixtureResponse(b"part", {"Content-Length": "8"})
        complete = FixtureResponse(b"complete", {"Content-Length": "8"})
        with patch.object(api.opener, "open", side_effect=[partial, complete]) as opened:
            self.assertEqual(api.request("/artifact", binary=True), b"complete")
        self.assertEqual(opened.call_count, 2)

    def test_exhausted_timeouts_have_a_finite_attempt_count(self):
        api = self.api(request_timeout=3, retries=2)
        with patch.object(api.opener, "open", side_effect=TimeoutError("Fixture timeout")) as opened:
            with self.assertRaisesRegex(RuntimeError, "已尝试 3 次"):
                api.request("/fixture")
        self.assertEqual(opened.call_count, 3)
        self.assertEqual([call.args for call in self.sleep.call_args_list], [(1,), (2,)])

    def test_zero_retries_makes_one_attempt(self):
        api = self.api(retries=0)
        with patch.object(api.opener, "open", side_effect=ConnectionResetError(104, "Fixture reset")) as opened:
            with self.assertRaisesRegex(RuntimeError, "已尝试 1 次"):
                api.request("/fixture")
        self.assertEqual(opened.call_count, 1)
        self.sleep.assert_not_called()

    def test_certificate_and_local_permission_failures_are_not_retried(self):
        failures = [ssl.SSLCertVerificationError(1, "Fixture certificate failure"),
                    PermissionError(13, "Fixture permission failure")]
        for failure in failures:
            for wrapped in (False, True):
                with self.subTest(failure=type(failure).__name__, wrapped=wrapped):
                    api = self.api(retries=2)
                    error = urllib.error.URLError(failure) if wrapped else failure
                    with patch.object(api.opener, "open", side_effect=error) as opened:
                        with self.assertRaises(RuntimeError):
                            api.request("/fixture")
                    self.assertEqual(opened.call_count, 1)
        self.sleep.assert_not_called()

    def test_http_auth_permission_missing_and_rate_limits_have_distinct_errors_without_retry(self):
        cases = [(401, {}, "认证失败"), (403, {}, "拒绝访问"), (404, {}, "资源不可见"),
                 (429, {}, "限流"), (403, {"X-RateLimit-Remaining": "0"}, "限流"),
                 (403, {"Retry-After": "60"}, "限流")]
        for status, headers, hint in cases:
            with self.subTest(status=status, headers=headers):
                api = self.api(retries=2)
                error = urllib.error.HTTPError("https://api.github.com/fixture", status,
                                               "Fixture HTTP error", headers, io.BytesIO())
                with patch.object(api.opener, "open", side_effect=error) as opened:
                    with self.assertRaisesRegex(RuntimeError, hint):
                        api.request("/fixture")
                self.assertEqual(opened.call_count, 1)
        self.sleep.assert_not_called()

    def test_transient_server_failures_are_retried(self):
        for status in (500, 502, 503, 504):
            with self.subTest(status=status):
                api = self.api(retries=1)
                error = urllib.error.HTTPError("https://api.github.com/fixture", status,
                                               "Fixture server error", {}, io.BytesIO())
                with patch.object(api.opener, "open", side_effect=[error, FixtureResponse()]) as opened:
                    self.assertEqual(api.request("/fixture"), {"ok": True})
                self.assertEqual(opened.call_count, 2)

    def test_oversized_content_length_fails_without_retry_or_read(self):
        api = self.api(retries=2)
        response = FixtureResponse(headers={"Content-Length": str(256 * 1024 * 1024 + 1)},
                                   read_error=AssertionError("Oversized body must not be read"))
        with patch.object(api.opener, "open", return_value=response) as opened:
            with self.assertRaisesRegex(RuntimeError, "size limit"):
                api.request("/artifact", binary=True)
        self.assertEqual(opened.call_count, 1)
        self.sleep.assert_not_called()

    def test_environment_explicit_and_disabled_proxy_modes(self):
        with patch.dict(SYNC.os.environ, {"http_proxy": "http://environment.invalid:8090",
                                         "https_proxy": "http://environment.invalid:8091"}):
            api = self.api()
            proxies = [handler.proxies for handler in api.opener.handlers
                       if isinstance(handler, urllib.request.ProxyHandler)]
            self.assertEqual(proxies, [{"http": "http://environment.invalid:8090",
                                        "https": "http://environment.invalid:8091"}])
            direct = self.api(proxy="")
            self.assertFalse(any(isinstance(handler, urllib.request.ProxyHandler)
                                 and handler.proxies for handler in direct.opener.handlers))
            explicit = self.api(proxy="http://username:fixture-proxy-secret@proxy.invalid:8080")
            proxies = [handler.proxies for handler in explicit.opener.handlers
                       if isinstance(handler, urllib.request.ProxyHandler)]
            self.assertEqual(proxies, [{"http": "http://username:fixture-proxy-secret@proxy.invalid:8080",
                                       "https": "http://username:fixture-proxy-secret@proxy.invalid:8080"}])
        self.assertNotIn("fixture-proxy-secret", self.stderr.getvalue())

    def test_invalid_proxy_addresses_are_rejected(self):
        for proxy in ("socks5://proxy.invalid:1080", "http://", "http://proxy.invalid:0",
                      "http://proxy.invalid:invalid", "http://proxy.invalid:70000"):
            with self.subTest(proxy=proxy), self.assertRaisesRegex(ValueError, "HTTP 代理"):
                self.api(proxy=proxy)

    def test_progress_is_flushed_to_stderr(self):
        with patch("builtins.print") as printed:
            SYNC.progress("Fixture status")
        printed.assert_called_once_with("Fixture status", file=SYNC.sys.stderr, flush=True)


def main(argv=None):
    arguments = sys.argv[1:] if argv is None else argv
    program = unittest.main(module=__name__, argv=[sys.argv[0], *arguments], verbosity=2, exit=False)
    return 0 if program.result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())

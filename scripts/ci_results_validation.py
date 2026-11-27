#!/usr/bin/env python3
"""Offline regression checks for atomic CI-result synchronization.

All synthetic runs and artifacts stay in TemporaryDirectory instances. No
GitHub connection is made and the repository's real test/ is never written.
"""
from __future__ import annotations

import importlib.util
import io
import json
from pathlib import Path
import stat
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
                        "status": "PASS"}
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
        self.destination = self.root / "test"
        self.destination.mkdir()
        (self.destination / "README.md").write_text("Fixture directory only\n", encoding="utf-8")
        self.addCleanup(patch.stopall)
        patch.object(SYNC, "ROOT", self.root).start()
        patch.object(SYNC, "DEST", self.destination).start()
        patch.object(urllib.request.OpenerDirector, "open",
                     side_effect=AssertionError("Network is forbidden in offline validation")).start()

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

    def test_complete_run_preserves_four_artifacts_and_source_identity(self):
        run = run_fixture()
        self.assertEqual(self.synchronize(run), 0)
        metadata = self.metadata()
        self.assertEqual(metadata["run"]["head_sha"], run["head_sha"])
        self.assertEqual(metadata["run"]["run_attempt"], 1)
        self.assertEqual(metadata["missing_artifacts"], [])
        self.assertEqual(len(metadata["artifacts"]), 4)
        self.assertIn("101", (self.destination / "report.md").read_text())
        self.assertEqual((self.destination / "README.md").read_text(), "Fixture directory only\n")
        for arch, profile in SYNC.MATRIX:
            directory = self.destination / arch / profile
            summary = json.loads((directory / "ci/fixture-run/summary.json").read_text())
            self.assertEqual((summary["architecture"], summary["profile"]), (arch, profile))
            self.assertTrue((directory / "runner/environment.log").is_file())
            self.assertTrue((directory / "ci/fixture-run/01-build.log").is_file())
        self.assert_no_temporary_results()

    def test_new_run_replaces_all_previous_files(self):
        self.assertEqual(self.synchronize(), 0)
        (self.destination / "stale-pass.md").write_text("Previous run only\n")
        obsolete = self.destination / "arm64/full/obsolete"
        obsolete.mkdir()
        (obsolete / "passed.log").write_text("Previous run only\n")
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

    def test_unsafe_archive_paths_and_symlinks_are_rejected(self):
        self.assertEqual(self.synchronize(), 0)
        previous = self.snapshot()
        symlink = zipfile.ZipInfo("ci/symlink")
        symlink.create_system = 3
        symlink.external_attr = (stat.S_IFLNK | 0o777) << 16
        for name in ("../escape", "/absolute", "ci/../../escape", "ci\\escape", symlink):
            with self.subTest(name=str(name)):
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
        staged = self.root / "candidate"
        staged.mkdir()
        (staged / "latest.json").write_text("Synthetic replacement\n")
        original_rename = Path.rename

        def fail_candidate_install(source, target):
            if source == staged and Path(target) == self.destination:
                raise OSError("Synthetic install rename failure")
            return original_rename(source, target)

        with patch.object(Path, "rename", fail_candidate_install):
            with self.assertRaisesRegex(OSError, "install rename failure"):
                SYNC.replace_results(staged)
        self.assertEqual(self.snapshot(), previous)
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
        (self.destination / "user-data.txt").write_text("Preserve user content\n")
        previous = self.snapshot()
        with self.assertRaises(RuntimeError):
            self.synchronize()
        self.assertEqual(self.snapshot(), previous)
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


if __name__ == "__main__":
    unittest.main(verbosity=2)

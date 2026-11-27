#!/usr/bin/env python3
"""Replace root/test with one GitHub Actions run, including failed jobs.

Uses the GitHub REST API; authentication comes from GH_TOKEN/GITHUB_TOKEN or
an existing gh login. Tokens are never written into reports or command lines.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / "test"
MARKER = ".ludp-ci-results"
WORKFLOW = ".github/workflows/ci.yml"
MATRIX = [(arch, profile) for arch in ("x86_64", "arm64")
          for profile in ("full", "ubsan")]


class SafeRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if urllib.parse.urlsplit(newurl).scheme != "https":
            raise RuntimeError("GitHub download redirected to a non-HTTPS URL")
        redirected = super().redirect_request(req, fp, code, msg, headers, newurl)
        if redirected and urllib.parse.urlsplit(req.full_url).netloc != urllib.parse.urlsplit(newurl).netloc:
            redirected.remove_header("Authorization")
        return redirected


class GitHub:
    def __init__(self):
        self.token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
        if not self.token and shutil.which("gh"):
            auth = subprocess.run(["gh", "auth", "token", "--hostname", "github.com"],
                                  capture_output=True, text=True, check=False)
            if auth.returncode == 0:
                self.token = auth.stdout.strip()
        self.opener = urllib.request.build_opener(SafeRedirect())

    def request(self, path, binary=False):
        headers = {"Accept": "application/vnd.github+json", "User-Agent": "ludp-ci-results",
                   "X-GitHub-Api-Version": "2022-11-28"}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        req = urllib.request.Request("https://api.github.com" + path, headers=headers)
        try:
            with self.opener.open(req, timeout=60) as response:
                limit = 256 * 1024 * 1024 if binary else 16 * 1024 * 1024
                data = response.read(limit + 1)
                if len(data) > limit:
                    raise RuntimeError("GitHub response exceeds download size limit")
                return data if binary else json.loads(data)
        except urllib.error.HTTPError as exc:
            hint = "；请用 gh auth login 登录，或在本机环境配置 Actions 读取权限的 GH_TOKEN" if exc.code in (401, 403) else ""
            raise RuntimeError(f"GitHub API 返回 HTTP {exc.code}{hint}") from None

    def pages(self, path, key):
        items = []
        for page in range(1, 101):
            sep = "&" if "?" in path else "?"
            batch = self.request(f"{path}{sep}per_page=100&page={page}")[key]
            items.extend(batch)
            if len(batch) < 100:
                return items
        raise RuntimeError("GitHub pagination exceeded limit")


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True).strip()


def repository():
    remote = git("remote", "get-url", "origin")
    match = re.fullmatch(r"(?:https://github\.com/|git@github\.com:)([\w.-]+/[\w.-]+?)(?:\.git)?", remote)
    if not match:
        raise RuntimeError("无法识别 origin；请指定 --repository OWNER/REPO")
    return match[1]


def extract_zip(data, destination):
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        entries = archive.infolist()
        if sum(item.file_size for item in entries) > 1024 * 1024 * 1024:
            raise RuntimeError("Artifact uncompressed size exceeds limit")
        for item in entries:
            path = PurePosixPath(item.filename)
            mode = item.external_attr >> 16
            if path.is_absolute() or ".." in path.parts or "\\" in item.filename or stat.S_ISLNK(mode):
                raise RuntimeError("Artifact contains an unsafe archive path")
            target = destination.joinpath(*path.parts)
            if item.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(item) as source, target.open("wb") as output:
                    shutil.copyfileobj(source, output)


def replace_results(staged):
    if DEST.is_symlink():
        raise RuntimeError("test/ 是符号链接，无法替换")
    if DEST.exists() and not (DEST / MARKER).is_file():
        if any(path.name != "README.md" for path in DEST.iterdir()):
            raise RuntimeError("test/ 包含非本脚本管理的文件；请先移至其他目录")
    if (DEST / "README.md").exists():
        shutil.copy2(DEST / "README.md", staged / "README.md")
    (staged / MARKER).write_text("Managed by scripts/sync_ci_results.py\n", encoding="utf-8")
    backup = Path(tempfile.mkdtemp(prefix=".ci-results-old-", dir=ROOT))
    backup.rmdir()
    had_previous = DEST.exists()
    installed = False
    try:
        if had_previous:
            DEST.rename(backup)
        try:
            staged.rename(DEST)
            installed = True
        except BaseException:
            if had_previous:
                backup.rename(DEST)
            raise
    finally:
        if installed and backup.exists():
            shutil.rmtree(backup)


def synchronize(api, repo, run):
    run_id, attempt = run["id"], run["run_attempt"]
    base = f"/repos/{repo}/actions/runs/{run_id}"
    # An attempt-specific list avoids mixing a partial rerun with old jobs.
    jobs = api.pages(f"{base}/attempts/{attempt}/jobs", "jobs")
    artifacts = api.pages(f"{base}/artifacts", "artifacts")
    staged = Path(tempfile.mkdtemp(prefix=".ci-results-new-", dir=ROOT))
    missing = []
    downloaded = []
    try:
        for arch, profile in MATRIX:
            name = f"ludp-{arch}-{profile}-{run_id}-{attempt}"
            matches = [a for a in artifacts if a["name"] == name and not a["expired"]]
            if len(matches) != 1:
                missing.append(name)
                continue
            artifact = matches[0]
            data = api.request(f"/repos/{repo}/actions/artifacts/{artifact['id']}/zip", binary=True)
            extract_zip(data, staged / arch / profile)
            downloaded.append({"name": name, "id": artifact["id"], "path": f"{arch}/{profile}"})
        # Even a checkout/build failure can have no artifact; retain its job log.
        warnings = []
        for job in jobs:
            if job.get("conclusion") not in ("success", "skipped"):
                try:
                    log = api.request(f"/repos/{repo}/actions/jobs/{job['id']}/logs", binary=True)
                    logs = staged / "failed-jobs"
                    logs.mkdir(exist_ok=True)
                    (logs / f"{job['id']}.log").write_bytes(log)
                except (RuntimeError, urllib.error.URLError) as exc:
                    warnings.append(f"job {job['id']}: {exc}")
        metadata = {"repository": repo, "downloaded_at": datetime.now(timezone.utc).isoformat(),
                    "run": {key: run.get(key) for key in
                            ("id", "run_attempt", "head_sha", "head_branch", "event", "status", "conclusion", "html_url", "path", "updated_at")},
                    "artifacts": downloaded, "missing_artifacts": missing, "warnings": warnings,
                    "jobs": jobs}
        (staged / "latest.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        lines = ["# 最新 GitHub Actions 验证结果", "", f"运行：[{run_id} / attempt {attempt}]({run['html_url']})",
                 f"提交：`{run['head_sha']}`；分支：`{run['head_branch']}`；GitHub 结论：**{run['conclusion']}**。", "",
                 "本目录整体替换上一次结果；缺失报告不会沿用旧文件。", "", "| 作业 | 结果 |", "| --- | --- |"]
        for job in jobs:
            lines.append(f"| {job['name'].replace('|', '/')} | {job.get('conclusion') or job['status']} |")
        lines.extend(["", "各架构目录保留上传内容：`ci/<运行时间>/report.md`、`summary.json`、原始日志与 `runner/environment.log`。"])
        if missing:
            lines.extend(["", "缺失或过期的报告（不能视为通过）：", "", *[f"- `{name}`" for name in missing]])
        if warnings:
            lines.extend(["", "部分作业日志未能下载；原因见 `latest.json`。"])
        (staged / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
        replace_results(staged)
        return 0 if run["conclusion"] == "success" and not missing and not warnings else 1
    finally:
        if staged.exists():
            shutil.rmtree(staged)


def main():
    parser = argparse.ArgumentParser(description="下载一轮 GitHub CI 到根目录 test/，整体覆盖旧结果（含失败结果）")
    parser.add_argument("--repository", help="默认读取 origin：OWNER/REPO")
    parser.add_argument("--branch", help="默认使用当前 Git 分支")
    parser.add_argument("--run-id", type=int, help="指定运行编号；默认选择该分支最新一轮")
    parser.add_argument("--wait", action="store_true", help="等待选定运行结束后下载")
    parser.add_argument("--timeout", type=int, default=1800, help="等待上限秒数，默认 1800")
    args = parser.parse_args()
    repo = args.repository or repository()
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", repo) or args.timeout <= 0:
        parser.error("仓库名或超时参数无效")
    api = GitHub()
    base = f"/repos/{repo}/actions"
    if args.run_id:
        run = api.request(f"{base}/runs/{args.run_id}")
    else:
        branch = args.branch or git("symbolic-ref", "--short", "HEAD")
        query = urllib.parse.urlencode({"branch": branch, "per_page": 1})
        runs = api.request(f"{base}/workflows/ci.yml/runs?{query}")["workflow_runs"]
        if not runs:
            raise RuntimeError(f"分支 {branch} 尚无 ci.yml 运行记录；请先推送代码")
        run = runs[0]
    if run.get("path", "").split("@")[0] != WORKFLOW:
        raise RuntimeError("指定运行不属于 .github/workflows/ci.yml")
    deadline = time.monotonic() + args.timeout
    while run["status"] != "completed":
        if not args.wait:
            raise RuntimeError("最新运行尚未结束；请使用 --wait，旧结果保持不变")
        if time.monotonic() >= deadline:
            raise RuntimeError("等待 GitHub CI 超时；旧结果保持不变")
        print(f"Run {run['id']}: {run['status']}", flush=True)
        time.sleep(min(15, max(0.1, deadline - time.monotonic())))
        run = api.request(f"{base}/runs/{run['id']}")
    status = synchronize(api, repo, run)
    print(f"已更新 {DEST}；运行 {run['id']}；结论 {run['conclusion']}")
    return status


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.SubprocessError) as exc:
        print(f"同步失败：{exc}", file=sys.stderr)
        sys.exit(2)

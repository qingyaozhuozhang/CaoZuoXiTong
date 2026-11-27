#!/usr/bin/env python3
"""Replace test/results with one GitHub Actions run, including failed jobs.

Uses the GitHub REST API; authentication comes from GH_TOKEN/GITHUB_TOKEN or
an existing gh login. Tokens are never written into reports or command lines.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import errno
import http.client
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import socket
import ssl
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
PACKAGE_ROOT = Path(__file__).resolve().parent
DEST = PACKAGE_ROOT / "results"
MARKER = ".ludp-ci-results"
WORKFLOW = ".github/workflows/ci.yml"
MATRIX = [(arch, profile) for arch in ("x86_64", "arm64")
          for profile in ("full", "ubsan")]


def progress(message):
    print(message, file=sys.stderr, flush=True)


def network_reason(error):
    reason = error.reason if isinstance(error, urllib.error.URLError) else error
    if isinstance(reason, ssl.SSLCertVerificationError):
        return "TLS 证书验证失败；请检查系统时间、证书或代理配置，不要关闭证书校验"
    if isinstance(reason, (TimeoutError, socket.timeout)):
        return "连接或读取超时"
    if isinstance(reason, ConnectionResetError):
        return "连接被重置（Connection reset by peer）"
    if isinstance(reason, ConnectionRefusedError):
        return "连接被拒绝；请检查代理是否运行及端口是否正确"
    if isinstance(reason, (http.client.IncompleteRead, http.client.RemoteDisconnected)):
        return "连接提前关闭或下载不完整"
    if isinstance(reason, socket.gaierror):
        return "DNS 解析失败"
    # Never include raw exception URLs: redirects can carry signed credentials.
    return f"网络连接失败（{type(reason).__name__}）"


def retryable_network(error):
    reason = error.reason if isinstance(error, urllib.error.URLError) else error
    return not (isinstance(reason, ssl.SSLCertVerificationError) or
                isinstance(reason, PermissionError) or
                getattr(reason, "errno", None) in (errno.EACCES, errno.EPERM))


class SafeRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if urllib.parse.urlsplit(newurl).scheme != "https":
            raise RuntimeError("GitHub download redirected to a non-HTTPS URL")
        redirected = super().redirect_request(req, fp, code, msg, headers, newurl)
        if redirected and urllib.parse.urlsplit(req.full_url).netloc != urllib.parse.urlsplit(newurl).netloc:
            redirected.remove_header("Authorization")
        return redirected


class GitHub:
    def __init__(self, *, request_timeout=15, retries=2, proxy=None):
        self.request_timeout = request_timeout
        self.retries = retries
        self.token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
        if not self.token and shutil.which("gh"):
            progress("正在读取 GitHub CLI 登录状态…")
            try:
                auth = subprocess.run(["gh", "auth", "token", "--hostname", "github.com"],
                                      capture_output=True, text=True, check=False, timeout=10)
            except subprocess.TimeoutExpired:
                raise RuntimeError("读取 gh 登录状态超时；请在终端检查 gh auth status") from None
            if auth.returncode == 0:
                self.token = auth.stdout.strip()
        handlers = [SafeRedirect()]
        if proxy is not None:
            if proxy:
                parsed = urllib.parse.urlsplit(proxy)
                try:
                    valid = parsed.scheme in ("http", "https") and parsed.hostname and parsed.port != 0
                except ValueError:
                    valid = False
                if not valid:
                    raise ValueError("--proxy 必须是有效的 HTTP 代理地址，例如 http://127.0.0.1:7890")
            handlers.append(urllib.request.ProxyHandler({"http": proxy, "https": proxy} if proxy else {}))
        self.opener = urllib.request.build_opener(*handlers)
        progress("GitHub 认证：已配置" if self.token else
                 "GitHub 认证：未配置；匿名 API 容易限流，下载附件需要 gh 登录或 GH_TOKEN")
        progress("网络：直连（忽略环境代理）" if proxy == "" else
                 "网络：使用指定 HTTP(S) 代理" if proxy else
                 "网络：沿用当前终端的 HTTP(S) 代理配置")

    def request(self, path, binary=False):
        headers = {"Accept": "application/vnd.github+json", "User-Agent": "ludp-ci-results",
                   "X-GitHub-Api-Version": "2022-11-28"}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        attempts = self.retries + 1
        for attempt in range(1, attempts + 1):
            progress(f"[请求 {attempt}/{attempts}] api.github.com{path.split('?')[0]} "
                     f"（网络等待 {self.request_timeout:g}s）")
            req = urllib.request.Request("https://api.github.com" + path, headers=headers)
            try:
                with self.opener.open(req, timeout=self.request_timeout) as response:
                    limit = 256 * 1024 * 1024 if binary else 16 * 1024 * 1024
                    length = response.headers.get("Content-Length", "")
                    expected = int(length) if length.isdigit() else None
                    if expected is not None and expected > limit:
                        raise RuntimeError("GitHub response exceeds download size limit")
                    chunks, size = [], 0
                    last_notice = time.monotonic()
                    while True:
                        chunk = response.read(min(1024 * 1024, limit + 1 - size))
                        if not chunk:
                            break
                        chunks.append(chunk)
                        size += len(chunk)
                        if size > limit:
                            raise RuntimeError("GitHub response exceeds download size limit")
                        if binary and time.monotonic() - last_notice >= 5:
                            progress(f"  已接收 {size / 1024 / 1024:.1f} MiB")
                            last_notice = time.monotonic()
                    data = b"".join(chunks)
                    if expected is not None and size < expected:
                        raise http.client.IncompleteRead(data, expected - size)
                    progress(f"[完成] 已接收 {len(data):,} 字节")
                    return data if binary else json.loads(data)
            except urllib.error.HTTPError as exc:
                status = exc.code
                limited = status == 429 or (status == 403 and (
                    exc.headers.get("X-RateLimit-Remaining") == "0" or exc.headers.get("Retry-After")))
                exc.close()
                if limited:
                    raise RuntimeError(f"GitHub API 限流（HTTP {status}）；" +
                                       ("请稍后重试，避免频繁请求" if self.token else
                                        "当前未认证，请先配置 gh auth login 或 GH_TOKEN，再重试")) from None
                if status == 401:
                    raise RuntimeError("GitHub 认证失败（HTTP 401）；请检查 gh 登录或 GH_TOKEN 是否有效") from None
                if status == 403:
                    raise RuntimeError("GitHub 拒绝访问（HTTP 403）；请检查登录、Actions 读取权限、组织策略或限流状态") from None
                if status == 404:
                    raise RuntimeError("GitHub 资源不可见（HTTP 404）；请检查仓库、ci.yml 是否已推送，以及私有仓库读取权限") from None
                if status not in (500, 502, 503, 504) or attempt == attempts:
                    raise RuntimeError(f"GitHub API 返回 HTTP {status}") from None
                reason = f"GitHub 暂时不可用（HTTP {status}）"
            except (urllib.error.URLError, OSError, http.client.IncompleteRead,
                    http.client.RemoteDisconnected) as exc:
                reason = network_reason(exc)
                if not retryable_network(exc) or attempt == attempts:
                    raise RuntimeError(f"{reason}；已尝试 {attempt} 次。"
                                       "请检查终端代理，可用 --proxy http://127.0.0.1:端口 指定 HTTP 代理；"
                                       "下载未完成时保留已有 test/results/ 结果") from None
            delay = min(2 ** (attempt - 1), 4)
            progress(f"[重试] {reason}；{delay}s 后重试")
            time.sleep(delay)

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


def validate_destination():
    if DEST.is_symlink():
        raise RuntimeError("test/results/ 是符号链接，无法替换")
    if DEST.exists():
        if not DEST.is_dir():
            raise RuntimeError("test/results 不是目录，无法替换")
        marker = DEST / MARKER
        if any(DEST.iterdir()) and (marker.is_symlink() or not marker.is_file()):
            raise RuntimeError("test/results/ 包含非本工具管理的文件；请先移至其他目录")


def replace_results(staged):
    validate_destination()
    (staged / MARKER).write_text("Managed by test.sync_ci_results\n", encoding="utf-8")
    backup = Path(tempfile.mkdtemp(prefix=".ci-results-old-", dir=PACKAGE_ROOT))
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
                try:
                    backup.rename(DEST)
                except OSError as exc:
                    raise RuntimeError(f"替换结果失败，自动恢复也未完成；旧结果保留在 {backup}") from exc
            raise
    finally:
        if installed and backup.exists():
            shutil.rmtree(backup)


def synchronize(api, repo, run):
    validate_destination()
    run_id, attempt = run["id"], run["run_attempt"]
    base = f"/repos/{repo}/actions/runs/{run_id}"
    progress(f"正在读取运行 {run_id} / attempt {attempt} 的作业与附件列表…")
    # An attempt-specific list avoids mixing a partial rerun with old jobs.
    jobs = api.pages(f"{base}/attempts/{attempt}/jobs", "jobs")
    artifacts = api.pages(f"{base}/artifacts", "artifacts")
    staged = Path(tempfile.mkdtemp(prefix=".ci-results-new-", dir=PACKAGE_ROOT))
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
            progress(f"正在下载 {arch} / {profile}…")
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
        lines = ["# LUDP 双架构 CI 验证汇总报告", "", f"运行：[{run_id} / attempt {attempt}]({run['html_url']})",
                 f"提交：`{run['head_sha']}`；分支：`{run['head_branch']}`；GitHub 结论：**{run['conclusion']}**。", "",
                 "本目录整体替换上一次结果；缺失报告不会沿用旧文件。", "", "| 作业 | 结果 |", "| --- | --- |"]
        for job in jobs:
            lines.append(f"| {job['name'].replace('|', '/')} | {job.get('conclusion') or job['status']} |")
        lines.extend(["", "## 报告入口", "",
                      "| 架构 | 验证配置 | 功能、正确性与性能验证报告 |",
                      "|---|---|---|"])
        for arch, profile in MATRIX:
            reports = sorted((staged / arch / profile / "ci").glob("*/report.md"))
            links = " · ".join(f"[查看报告]({path.relative_to(staged).as_posix()})" for path in reports)
            lines.append(f"| {arch} | {profile} | {links or '缺失，不能视为通过'} |")
        lines.extend(["", "各架构目录保留上传内容：`ci/<运行时间>/report.md`、`summary.json`、原始日志与 `runner/environment.log`。"])
        lines.extend(["", "专项报告：`benchmark/benchmark.md` 为 getpid 探针性能基准；"
                      "`event-merge/report.md` 为 A3 用户态与内核事件合流验证。"
                      "F1–F7、A1–A5 的检查范围与跳过项见各作业主报告，不能仅凭基准报告判断全部功能完成。"])
        if missing:
            lines.extend(["", "缺失或过期的报告（不能视为通过）：", "", *[f"- `{name}`" for name in missing]])
        if warnings:
            lines.extend(["", "部分作业日志未能下载；原因见 `latest.json`。"])
        (staged / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
        progress("下载完成，正在替换本地 test/results/ 结果…")
        replace_results(staged)
        return 0 if run["conclusion"] == "success" and not missing and not warnings else 1
    finally:
        if staged.exists():
            shutil.rmtree(staged)


def main(argv=None, *, prog=None):
    parser = argparse.ArgumentParser(prog=prog,
                                     description="下载一轮 GitHub CI 到 test/results/，整体覆盖旧结果（含失败结果）")
    parser.add_argument("--repository", help="默认读取 origin：OWNER/REPO")
    parser.add_argument("--branch", help="默认使用当前 Git 分支")
    parser.add_argument("--run-id", type=int, help="指定运行编号；默认选择该分支最新一轮")
    parser.add_argument("--wait", action="store_true", help="等待选定运行结束后下载")
    parser.add_argument("--timeout", type=int, default=1800, help="等待上限秒数，默认 1800")
    parser.add_argument("--request-timeout", type=float, default=15,
                        help="单次网络 I/O 等待秒数，默认 15；与 CI 等待上限分开")
    parser.add_argument("--retries", type=int, default=2, help="临时网络/服务错误重试次数，默认 2")
    network = parser.add_mutually_exclusive_group()
    network.add_argument("--proxy", help="本次使用的 HTTP(S) 代理，例如 http://127.0.0.1:7890")
    network.add_argument("--no-proxy", action="store_true", help="本次直连，忽略环境中的代理")
    args = parser.parse_args(argv)
    progress("开始同步 GitHub Actions 结果…")
    repo = args.repository or repository()
    if (not re.fullmatch(r"[\w.-]+/[\w.-]+", repo) or args.timeout <= 0 or
            not 0 < args.request_timeout <= 300 or not 0 <= args.retries <= 5 or
            args.run_id is not None and args.run_id <= 0):
        parser.error("仓库名或运行编号无效；timeout 必须为正数，request-timeout 为 (0,300]，retries 为 0..5")
    branch = args.branch or (git("symbolic-ref", "--short", "HEAD") if not args.run_id else None)
    progress(f"仓库：{repo}；" + (f"运行：{args.run_id}" if args.run_id else f"分支：{branch}"))
    progress(f"结果目录：{DEST}")
    api = GitHub(request_timeout=args.request_timeout, retries=args.retries,
                 proxy="" if args.no_proxy else args.proxy)
    base = f"/repos/{repo}/actions"
    if args.run_id:
        run = api.request(f"{base}/runs/{args.run_id}")
    else:
        query = urllib.parse.urlencode({"branch": branch, "per_page": 1})
        runs = api.request(f"{base}/workflows/ci.yml/runs?{query}")["workflow_runs"]
        if not runs:
            raise RuntimeError(f"分支 {branch} 尚无 ci.yml 运行记录；请先推送代码")
        run = runs[0]
    if run.get("path", "").split("@")[0] != WORKFLOW:
        raise RuntimeError("指定运行不属于 .github/workflows/ci.yml")
    progress(f"已找到运行 {run['id']}：{run['status']}；{run['html_url']}")
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


def cli(argv=None, *, prog=None):
    try:
        return main(argv, prog=prog)
    except KeyboardInterrupt:
        print("同步已取消；未完成的下载不会替换已有结果。", file=sys.stderr)
        return 130
    except (RuntimeError, OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.SubprocessError) as exc:
        print(f"同步失败：{exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(cli())

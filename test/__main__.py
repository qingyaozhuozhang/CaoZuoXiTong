"""Command-line entry point for the isolated CI tooling package."""
from __future__ import annotations

import argparse
import sys


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    parser = argparse.ArgumentParser(prog="python3 -m test", description="GitHub CI 结果同步与离线验证工具")
    parser.add_argument("command", choices=("sync", "validate"),
                        help="sync：同步到 test/results/；validate：运行工具自身的离线验证")
    if not argv or argv[0] not in ("sync", "validate"):
        parser.parse_args(argv)
    command, arguments = argv[0], argv[1:]
    if command == "sync":
        from .sync_ci_results import cli
        return cli(arguments, prog="python3 -m test sync")
    from .ci_results_validation import main as validate
    return validate(arguments)


if __name__ == "__main__":
    sys.exit(main())

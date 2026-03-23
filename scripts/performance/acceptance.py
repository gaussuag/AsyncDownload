#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from typing import Any

from performance_common import (
    DEFAULT_BENCHMARK_EXE,
    DEFAULT_OPTIONS,
    REPO_ROOT,
    log_info,
    make_run_root,
    resolve_repo_path,
    write_config,
    write_json,
)


DEFAULT_OUTPUT_ROOT = Path("build/performance_acceptance")
DEFAULT_TEST_EXE = Path("build/tests/Release/AsyncDownload_tests.exe")
DEFAULT_LONG_RUN_CASES = "baseline_default,balanced_candidate,memory_guard,scheduler_stress"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=["resource", "resume-interruption", "crc-resume", "long-run", "all"])
    parser.add_argument("--url")
    parser.add_argument("--exe", default=str(DEFAULT_BENCHMARK_EXE))
    parser.add_argument("--tests-exe", default=str(DEFAULT_TEST_EXE))
    parser.add_argument("--output-root", default=str(DEFAULT_OUTPUT_ROOT))
    parser.add_argument("--label", default="")
    parser.add_argument("--repeats", type=int, default=10)
    parser.add_argument("--case-list", default=DEFAULT_LONG_RUN_CASES)
    parser.add_argument("--keep-downloads", action="store_true")
    return parser.parse_args()


def run_process(command: list[str], *, cwd: Path = REPO_ROOT) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=str(cwd),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )


def write_process_logs(run_dir: Path, prefix: str, completed: subprocess.CompletedProcess[str]) -> dict[str, str]:
    stdout_path = run_dir / f"{prefix}.stdout.txt"
    stderr_path = run_dir / f"{prefix}.stderr.txt"
    stdout_path.write_text(completed.stdout, encoding="utf-8")
    stderr_path.write_text(completed.stderr, encoding="utf-8")
    return {
        "stdout_path": str(stdout_path),
        "stderr_path": str(stderr_path),
    }


def resolve_tests_exe(path_text: str) -> Path:
    requested = resolve_repo_path(path_text)
    if requested.exists():
        return requested
    debug_fallback = REPO_ROOT / "build" / "tests" / "Debug" / "AsyncDownload_tests.exe"
    if debug_fallback.exists():
        return debug_fallback
    raise RuntimeError(f"test executable not found: {requested}")


def ensure_cli_exists(path_text: str) -> Path:
    exe_path = resolve_repo_path(path_text)
    if not exe_path.exists():
        raise RuntimeError(f"CLI executable not found: {exe_path}")
    return exe_path


def run_gtest_case(run_root: Path, tests_exe: Path, case_name: str, gtest_filter: str) -> dict[str, Any]:
    run_dir = run_root / case_name
    run_dir.mkdir(parents=True, exist_ok=False)
    command = [str(tests_exe), f"--gtest_filter={gtest_filter}"]
    completed = run_process(command)
    logs = write_process_logs(run_dir, case_name, completed)
    return {
        "case_name": case_name,
        "command": command,
        "exit_code": completed.returncode,
        "status": "passed" if completed.returncode == 0 else "failed",
        **logs,
    }


def run_resource_diagnostic(
    run_root: Path,
    *,
    url: str,
    exe_path: Path,
    keep_downloads: bool,
) -> dict[str, Any]:
    run_dir = run_root / "resource"
    run_dir.mkdir(parents=True, exist_ok=False)
    output_path = run_dir / "resource.bin"
    summary_path = run_dir / "resource.summary.txt"
    diagnostic_path = run_dir / "resource.diagnostics.json"
    config_path = run_dir / "resource.config.json"
    write_config(config_path, DEFAULT_OPTIONS)
    command = [
        str(exe_path),
        url,
        str(output_path),
        "--config",
        str(config_path),
        "--summary-file",
        str(summary_path),
        "--diagnostic-file",
        str(diagnostic_path),
    ]
    completed = run_process(command)
    logs = write_process_logs(run_dir, "resource", completed)
    diagnostics_payload: dict[str, Any] = {}
    if diagnostic_path.exists():
        diagnostics_payload = json.loads(diagnostic_path.read_text(encoding="utf-8"))
    if not keep_downloads:
        for artifact in (
            output_path,
            Path(str(output_path) + ".part"),
            Path(str(output_path) + ".config.json"),
        ):
            if artifact.exists():
                artifact.unlink()
    return {
        "case_name": "resource",
        "command": command,
        "exit_code": completed.returncode,
        "status": "passed" if completed.returncode == 0 else "failed",
        "summary_path": str(summary_path),
        "diagnostic_path": str(diagnostic_path),
        "resource_diagnostics": diagnostics_payload.get("resource_diagnostics", {}),
        **logs,
    }


def run_long_run(
    run_root: Path,
    *,
    url: str,
    exe_path: Path,
    repeats: int,
    case_list: str,
) -> dict[str, Any]:
    run_dir = run_root / "long_run"
    run_dir.mkdir(parents=True, exist_ok=False)
    benchmark_script = REPO_ROOT / "scripts" / "performance" / "benchmark.py"
    command = [
        sys.executable,
        str(benchmark_script),
        "--url",
        url,
        "--exe",
        str(exe_path),
        "--benchmark-suite",
        "regression_v2",
        "--case-list",
        case_list,
        "--repeats",
        str(repeats),
        "--label",
        "acceptance-long-run",
    ]
    completed = run_process(command)
    logs = write_process_logs(run_dir, "long_run", completed)
    results_dir = ""
    for line in completed.stdout.splitlines():
        if "Results dir:" in line:
            results_dir = line.split("Results dir:", 1)[1].strip()
    return {
        "case_name": "long_run",
        "command": command,
        "exit_code": completed.returncode,
        "status": "passed" if completed.returncode == 0 else "failed",
        "results_dir": results_dir,
        "repeats": repeats,
        "case_list": case_list,
        **logs,
    }


def render_report(run_root: Path, results: list[dict[str, Any]]) -> Path:
    report_path = run_root / "acceptance_report.md"
    lines = [
        f"# Performance Acceptance Report: {run_root.name}",
        "",
        "| Check | Status | Exit Code | Notes |",
        "| --- | --- | --- | --- |",
    ]
    for result in results:
        note = ""
        if result["case_name"] == "resource":
            resource = result.get("resource_diagnostics", {})
            note = (
                f"threads={resource.get('peak_thread_count', 0)} "
                f"handles={resource.get('peak_handle_count', 0)} "
                f"cpu_avg={resource.get('average_cpu_utilization_pct', 0.0):.2f}%"
            )
        elif result["case_name"] == "long_run":
            note = f"cases={result.get('case_list', '')} repeats={result.get('repeats', 0)}"
        lines.append(
            f"| {result['case_name']} | {result['status']} | {result['exit_code']} | {note} |"
        )
    lines.append("")
    lines.append("## Files")
    lines.append("")
    lines.append("| Check | Stdout | Stderr |")
    lines.append("| --- | --- | --- |")
    for result in results:
        lines.append(
            f"| {result['case_name']} | {result['stdout_path']} | {result['stderr_path']} |"
        )
    report_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return report_path


def main() -> int:
    args = parse_args()
    output_root = resolve_repo_path(args.output_root)
    label = args.label or f"acceptance-{args.mode}"
    run_root = make_run_root(output_root, label)
    metadata_path = run_root / "acceptance_metadata.json"

    modes = [args.mode]
    if args.mode == "all":
        modes = ["resource", "resume-interruption", "crc-resume", "long-run"]

    needs_url = any(mode in {"resource", "long-run"} for mode in modes)
    if needs_url and not args.url:
        print("Error: --url is required for resource and long-run modes", file=sys.stderr)
        return 1

    try:
        exe_path = ensure_cli_exists(args.exe)
        tests_exe = resolve_tests_exe(args.tests_exe)
    except RuntimeError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    results: list[dict[str, Any]] = []
    log_info(f"Acceptance results dir: {run_root}")
    for mode in modes:
        if mode == "resource":
            log_info("Running resource diagnostics")
            results.append(
                run_resource_diagnostic(
                    run_root,
                    url=args.url,
                    exe_path=exe_path,
                    keep_downloads=args.keep_downloads,
                )
            )
        elif mode == "resume-interruption":
            log_info("Running interrupted resume validation")
            results.append(
                run_gtest_case(
                    run_root,
                    tests_exe,
                    "resume_interruption",
                    "DownloadIntegrationTest.ResumeAfterInterruptedCliDownload",
                )
            )
        elif mode == "crc-resume":
            log_info("Running CRC/VDL resume validation")
            results.append(
                run_gtest_case(
                    run_root,
                    tests_exe,
                    "crc_resume",
                    "DownloadIntegrationTest.ResumesAfterCrcRollbackPastVdl",
                )
            )
        elif mode == "long-run":
            log_info("Running long-run diagnostic benchmark")
            results.append(
                run_long_run(
                    run_root,
                    url=args.url,
                    exe_path=exe_path,
                    repeats=args.repeats,
                    case_list=args.case_list,
                )
            )

    report_path = render_report(run_root, results)
    metadata = {
        "generated_at": datetime.now().isoformat(timespec="seconds"),
        "mode": args.mode,
        "run_root": str(run_root),
        "report_path": str(report_path),
        "results": results,
    }
    write_json(metadata_path, metadata)
    failures = [result for result in results if result["exit_code"] != 0]
    if failures:
        log_info(f"Acceptance finished with failures. Report: {report_path}")
        return 1
    log_info(f"Acceptance finished successfully. Report: {report_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

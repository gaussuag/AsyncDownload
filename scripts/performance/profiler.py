#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

from performance_common import (
    DEFAULT_OPTIONS,
    DEFAULT_PROFILE_REPEATS,
    DEFAULT_PROFILER_EXE,
    DEFAULT_PROFILER_OUTPUT_ROOT,
    RAW_FIELD_ORDER,
    build_benchmark_suites,
    build_case_option_summary,
    build_execution_plan,
    build_sweeps,
    default_metadata,
    execute_download_run,
    format_float,
    log_info,
    make_run_root,
    parse_case_list,
    perform_head_request,
    resolve_repo_path,
    total_pause_count,
    validate_execution_inputs,
    write_csv,
    write_json,
)


DEFAULT_WPR_PROFILES = ["GeneralProfile", "CPU", "DiskIO", "FileIO"]
DEFAULT_WPA_PROFILE = Path(
    r"C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\Catalog\WpaRuleMatchExporter.wpaProfile"
)
DEFAULT_WPAEXPORTER = Path(
    r"C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\wpaexporter.exe"
)
DEFAULT_WPR = Path(r"C:\Windows\System32\wpr.exe")
DEFAULT_SYMBOL_PATH = Path("build/src/Release")

PROFILE_RAW_FIELD_ORDER = RAW_FIELD_ORDER + [
    "trace_status",
    "wpr_profiles",
    "wpr_start_exit_code",
    "wpr_stop_exit_code",
    "trace_path",
    "trace_size_bytes",
    "wpa_export_exit_code",
    "wpa_profile_path",
    "exported_files_count",
    "exports_dir",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", required=True)
    parser.add_argument("--output-root", default=str(DEFAULT_PROFILER_OUTPUT_ROOT))
    parser.add_argument("--exe", default=str(DEFAULT_PROFILER_EXE))
    parser.add_argument("--repeats", type=int, default=DEFAULT_PROFILE_REPEATS)
    parser.add_argument("--only-sweep")
    parser.add_argument("--benchmark-suite")
    parser.add_argument("--case-list")
    parser.add_argument("--label", default="")
    parser.add_argument("--keep-downloads", action="store_true")
    parser.add_argument("--overwrite-existing", choices=["true", "false"])
    parser.add_argument("--wpr-path", default=str(DEFAULT_WPR))
    parser.add_argument("--wpr-profiles", default=",".join(DEFAULT_WPR_PROFILES))
    parser.add_argument("--wpaexporter-path", default=str(DEFAULT_WPAEXPORTER))
    parser.add_argument("--wpa-profile", default=str(DEFAULT_WPA_PROFILE))
    parser.add_argument("--symbol-path", default=str(DEFAULT_SYMBOL_PATH))
    parser.add_argument("--skip-export", action="store_true")
    return parser.parse_args()


def parse_profile_names(text: str) -> list[str]:
    parsed = [item.strip() for item in text.split(",")]
    parsed = [item for item in parsed if item]
    if not parsed:
        raise RuntimeError("--wpr-profiles must contain at least one profile name")
    return parsed


def run_process(
    command: list[str],
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=str(cwd) if cwd is not None else None,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
        env=env,
    )


def write_process_logs(
    run_dir: Path,
    prefix: str,
    result: subprocess.CompletedProcess[str],
) -> tuple[Path, Path]:
    stdout_path = run_dir / f"{prefix}.stdout.txt"
    stderr_path = run_dir / f"{prefix}.stderr.txt"
    stdout_path.write_text(result.stdout, encoding="utf-8")
    stderr_path.write_text(result.stderr, encoding="utf-8")
    return stdout_path, stderr_path


def ensure_tool_exists(path_text: str, display_name: str) -> Path:
    path = Path(path_text)
    if not path.is_absolute():
        if path.exists():
            return path.resolve()
        repo_relative = (Path.cwd() / path).resolve()
        if repo_relative.exists():
            return repo_relative
        resolved = shutil.which(path_text)
        if resolved is None:
            raise RuntimeError(f"{display_name} not found: {path_text}")
        return Path(resolved)
    if not path.exists():
        raise RuntimeError(f"{display_name} not found: {path}")
    return path


def ensure_directory_exists(path_text: str, display_name: str) -> Path:
    path = resolve_repo_path(path_text)
    if not path.exists():
        raise RuntimeError(f"{display_name} not found: {path}")
    if not path.is_dir():
        raise RuntimeError(f"{display_name} is not a directory: {path}")
    return path


def ensure_wpr_idle(wpr_path: Path) -> None:
    status = run_process([str(wpr_path), "-status"])
    status_text = f"{status.stdout}\n{status.stderr}"
    if "WPR is not recording" in status_text:
        return
    raise RuntimeError("WPR is already recording. Stop the active session before running profiler.py")


def start_wpr_trace(
    *,
    wpr_path: Path,
    profiles: list[str],
    run_dir: Path,
) -> dict[str, Any]:
    command = [str(wpr_path), "-start", profiles[0]]
    for profile in profiles[1:]:
        command.extend(["-start", profile])
    command.append("-filemode")
    result = run_process(command)
    stdout_path, stderr_path = write_process_logs(run_dir, "wpr_start", result)
    return {
        "command": command,
        "exit_code": result.returncode,
        "stdout_path": stdout_path,
        "stderr_path": stderr_path,
    }


def stop_wpr_trace(
    *,
    wpr_path: Path,
    trace_path: Path,
    run_dir: Path,
) -> dict[str, Any]:
    command = [str(wpr_path), "-stop", str(trace_path)]
    result = run_process(command)
    stdout_path, stderr_path = write_process_logs(run_dir, "wpr_stop", result)
    return {
        "command": command,
        "exit_code": result.returncode,
        "stdout_path": stdout_path,
        "stderr_path": stderr_path,
    }


def export_trace(
    *,
    wpaexporter_path: Path,
    wpa_profile_path: Path,
    symbol_path: Path,
    trace_path: Path,
    exports_dir: Path,
    run_id: str,
    run_dir: Path,
) -> dict[str, Any]:
    exports_dir.mkdir(parents=True, exist_ok=False)
    command = [
        str(wpaexporter_path),
        "-i",
        str(trace_path),
        "-profile",
        str(wpa_profile_path),
        "-symbols",
        "-outputfolder",
        str(exports_dir),
        "-prefix",
        f"{run_id}_",
        "-outputformat",
        "CSV",
    ]
    exporter_env = dict(os.environ)
    exporter_env["_NT_SYMBOL_PATH"] = str(symbol_path)
    result = run_process(command, env=exporter_env)
    stdout_path, stderr_path = write_process_logs(run_dir, "wpaexporter", result)
    exported_files = sorted(path.name for path in exports_dir.glob("*") if path.is_file())
    return {
        "command": command,
        "exit_code": result.returncode,
        "stdout_path": stdout_path,
        "stderr_path": stderr_path,
        "exported_files": exported_files,
    }


def summarize_exports(exports_dir: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for path in sorted(exports_dir.glob("*.csv")):
        rows.append(
            {
                "file_name": path.name,
                "size_bytes": path.stat().st_size,
            }
        )
    return rows


def render_profile_report(
    report_path: Path,
    metadata: dict[str, Any],
    raw_rows: list[dict[str, Any]],
    export_indexes: dict[str, list[dict[str, Any]]],
    failure: dict[str, Any] | None,
) -> None:
    lines: list[str] = []
    lines.append(f"# Profiler Report: {metadata['label']}")
    lines.append("")
    lines.append("## Overview")
    lines.append("")
    lines.append("| Item | Value |")
    lines.append("| --- | --- |")
    lines.append(f"| Generated At | {metadata['generated_at']} |")
    lines.append(f"| URL | {metadata['head']['requested_url']} |")
    lines.append(f"| Resolved URL | {metadata['head']['resolved_url']} |")
    lines.append(f"| Content Length | {metadata['head']['content_length']} |")
    lines.append(f"| CLI | {metadata['exe_path']} |")
    lines.append(f"| Repeats | {metadata['repeats']} |")
    lines.append(f"| WPR | {metadata['wpr_path']} |")
    lines.append(f"| WPR Profiles | {', '.join(metadata['wpr_profiles'])} |")
    lines.append(f"| WPA Exporter | {metadata['wpaexporter_path'] or '(disabled)'} |")
    lines.append(f"| WPA Profile | {metadata['wpa_profile_path'] or '(disabled)'} |")
    lines.append(f"| Symbol Path | {metadata['symbol_path']} |")
    lines.append("")

    if failure is not None:
        lines.append("## Failure")
        lines.append("")
        lines.append("| Item | Value |")
        lines.append("| --- | --- |")
        lines.append(f"| Sweep | {failure['sweep_name']} |")
        lines.append(f"| Case | {failure['case_name']} |")
        lines.append(f"| Repeat | {failure['repeat_index']} |")
        lines.append(f"| Stage | {failure['stage']} |")
        lines.append(f"| Reason | {failure['reason']} |")
        lines.append("")

    lines.append("## Run Summary")
    lines.append("")
    lines.append("| Case | Repeat | Avg Network MB/s | Avg Disk MB/s | Queue Full Pauses | Gap Pauses | Max Memory Bytes | Trace | Exported CSVs |")
    lines.append("| --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    for row in raw_rows:
        lines.append(
            f"| {row['case_name']} | {row['repeat_index']} | "
            f"{format_float(float(row['avg_network_speed_mb_s']))} | "
            f"{format_float(float(row['avg_disk_speed_mb_s']))} | "
            f"{int(row['queue_full_pause_count'])} | "
            f"{int(row['gap_pause_count'])} | "
            f"{int(row['max_memory_bytes'])} | "
            f"{row['trace_path']} | "
            f"{int(row['exported_files_count'])} |"
        )
    lines.append("")

    lines.append("## Export Targets")
    lines.append("")
    lines.append("| Focus | Exported View Family | Why It Matters |")
    lines.append("| --- | --- | --- |")
    lines.append("| CPU hotspots | CpuSampling / CpuUsagePrecise | Confirms whether packet handling and persistence code dominate CPU time. |")
    lines.append("| Waits and blocking | WaitAnalysis / ThreadLifetimes | Confirms whether persistence is stalled on locks or waits instead of running. |")
    lines.append("| Disk and file I/O | DiskIO / DiskWrites / DiskWriteStats | Confirms whether write or flush behavior is the practical bottleneck. |")
    lines.append("| Generic events and process context | GenericEvents / Processes / Images | Keeps enough context to line up ETW data with the benchmark run. |")
    lines.append("")

    lines.append("## Exported Files")
    lines.append("")
    for run_id, files in export_indexes.items():
        lines.append(f"### {run_id}")
        lines.append("")
        if not files:
            lines.append("(no CSV exports)")
            lines.append("")
            continue
        lines.append("| File | Size Bytes |")
        lines.append("| --- | --- |")
        for item in files:
            lines.append(f"| {item['file_name']} | {item['size_bytes']} |")
        lines.append("")

    lines.append("## Files")
    lines.append("")
    lines.append("| File | Path |")
    lines.append("| --- | --- |")
    lines.append(f"| Metadata | {metadata['metadata_path']} |")
    lines.append(f"| Raw Runs CSV | {metadata['raw_csv_path']} |")
    lines.append(f"| Report | {metadata['report_path']} |")
    report_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()
    sweeps = build_sweeps()
    benchmark_suites = build_benchmark_suites()
    sweep_names = {sweep.name for sweep in sweeps}
    benchmark_suite_names = set(benchmark_suites)

    try:
        requested_case_names = parse_case_list(args.case_list)
        context = validate_execution_inputs(
            exe_text=args.exe,
            output_root_text=args.output_root,
            repeats=args.repeats,
            only_sweep=args.only_sweep,
            benchmark_suite_name=args.benchmark_suite,
            overwrite_existing_text=args.overwrite_existing,
            sweep_names=sweep_names,
            benchmark_suite_names=benchmark_suite_names,
        )
        head_metadata = perform_head_request(args.url)
        effective_suite = args.benchmark_suite or ("regression" if args.only_sweep is None else None)
        execution_mode, benchmark_suite, execution_plan, executed_sweeps = build_execution_plan(
            sweeps=sweeps,
            benchmark_suites=benchmark_suites,
            only_sweep=args.only_sweep,
            benchmark_suite_name=effective_suite,
            requested_case_names=requested_case_names,
        )
        profile_names = parse_profile_names(args.wpr_profiles)
        wpr_path = ensure_tool_exists(args.wpr_path, "WPR")
        ensure_wpr_idle(wpr_path)
        wpaexporter_path = None if args.skip_export else ensure_tool_exists(args.wpaexporter_path, "WPA Exporter")
        wpa_profile_path = None if args.skip_export else ensure_tool_exists(args.wpa_profile, "WPA profile")
        symbol_path = ensure_directory_exists(args.symbol_path, "symbol path")
    except RuntimeError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    label = args.label or "profile"
    run_root = make_run_root(context.output_root, label)
    metadata_path = run_root / "profile_metadata.json"
    raw_csv_path = run_root / "raw_runs.csv"
    report_path = run_root / "profile_report.md"

    metadata = default_metadata(
        label=label,
        scenario="loopback_external_url",
        exe_path=context.exe_path,
        run_root=run_root,
        repeats=args.repeats,
        keep_downloads=args.keep_downloads,
        head_metadata=head_metadata,
        execution_mode=execution_mode,
        benchmark_suite=benchmark_suite,
        requested_case_names=requested_case_names,
        executed_sweeps=executed_sweeps,
        overwrite_existing=context.overwrite_existing,
    )
    metadata["wpr_path"] = str(wpr_path)
    metadata["wpr_profiles"] = profile_names
    metadata["wpaexporter_path"] = "" if wpaexporter_path is None else str(wpaexporter_path)
    metadata["wpa_profile_path"] = "" if wpa_profile_path is None else str(wpa_profile_path)
    metadata["symbol_path"] = str(symbol_path)
    write_json(metadata_path, metadata)

    raw_rows: list[dict[str, Any]] = []
    export_indexes: dict[str, list[dict[str, Any]]] = {}
    failure: dict[str, Any] | None = None
    total_runs = len(execution_plan) * args.repeats
    run_sequence = 1

    log_info(f"CLI: {context.exe_path}")
    log_info(f"Results dir: {run_root}")
    log_info(f"Execution plan: {len(execution_plan)} cases x {args.repeats} repeats = {total_runs} profiled runs")
    log_info(f"WPR profiles: {', '.join(profile_names)}")
    log_info(f"Symbol path: {symbol_path}")
    if requested_case_names:
        log_info(f"Selected cases: {', '.join(requested_case_names)}")

    for sweep_name, case, overrides in execution_plan:
        for repeat_index in range(1, args.repeats + 1):
            options = dict(DEFAULT_OPTIONS)
            options["overwrite_existing"] = context.overwrite_existing
            options.update(overrides)
            run_id = f"{sweep_name}__{case.name}__r{repeat_index:02d}"
            run_dir = run_root / "runs" / run_id
            run_dir.mkdir(parents=True, exist_ok=False)
            trace_path = run_dir / f"{run_id}.etl"
            exports_dir = run_dir / "wpa_exports"
            log_info(
                f"Starting profile {run_sequence}/{total_runs}: "
                f"{sweep_name} / {case.name} / repeat {repeat_index} | "
                f"{build_case_option_summary(options)}"
            )

            start_trace = start_wpr_trace(wpr_path=wpr_path, profiles=profile_names, run_dir=run_dir)
            if start_trace["exit_code"] != 0:
                failure = {
                    "sweep_name": sweep_name,
                    "case_name": case.name,
                    "repeat_index": repeat_index,
                    "stage": "wpr-start",
                    "reason": f"wpr start failed with exit code {start_trace['exit_code']}",
                }
                break

            try:
                row = execute_download_run(
                    exe_path=context.exe_path,
                    url=args.url,
                    run_root=run_root,
                    sweep_name=sweep_name,
                    case=case,
                    repeat_index=repeat_index,
                    options=options,
                    head_metadata=head_metadata,
                    label=label,
                    scenario="loopback_external_url",
                    run_sequence=run_sequence,
                    keep_downloads=args.keep_downloads,
                    run_dir=run_dir,
                )
            except Exception as exc:
                stop_trace = stop_wpr_trace(wpr_path=wpr_path, trace_path=trace_path, run_dir=run_dir)
                failure = {
                    "sweep_name": sweep_name,
                    "case_name": case.name,
                    "repeat_index": repeat_index,
                    "stage": "download-run",
                    "reason": str(exc),
                }
                profile_info = {
                    "trace_status": "failed",
                    "wpr_profiles": ",".join(profile_names),
                    "wpr_start_exit_code": start_trace["exit_code"],
                    "wpr_stop_exit_code": stop_trace["exit_code"],
                    "trace_path": str(trace_path.relative_to(run_root)),
                    "trace_size_bytes": trace_path.stat().st_size if trace_path.exists() else 0,
                    "wpa_export_exit_code": -1,
                    "wpa_profile_path": "" if wpa_profile_path is None else str(wpa_profile_path),
                    "exported_files_count": 0,
                    "exports_dir": str(exports_dir.relative_to(run_root)),
                }
                write_json(run_dir / f"{run_id}.profile.json", profile_info)
                break

            stop_trace = stop_wpr_trace(wpr_path=wpr_path, trace_path=trace_path, run_dir=run_dir)
            export_result: dict[str, Any] | None = None
            exported_files_count = 0
            if stop_trace["exit_code"] == 0 and wpaexporter_path is not None and wpa_profile_path is not None:
                export_result = export_trace(
                    wpaexporter_path=wpaexporter_path,
                    wpa_profile_path=wpa_profile_path,
                    symbol_path=symbol_path,
                    trace_path=trace_path,
                    exports_dir=exports_dir,
                    run_id=run_id,
                    run_dir=run_dir,
                )
                export_indexes[run_id] = summarize_exports(exports_dir)
                exported_files_count = len(export_indexes[run_id])
            else:
                export_indexes[run_id] = []

            row["trace_status"] = "success"
            row["wpr_profiles"] = ",".join(profile_names)
            row["wpr_start_exit_code"] = start_trace["exit_code"]
            row["wpr_stop_exit_code"] = stop_trace["exit_code"]
            row["trace_path"] = str(trace_path.relative_to(run_root))
            row["trace_size_bytes"] = trace_path.stat().st_size if trace_path.exists() else 0
            row["wpa_export_exit_code"] = -1 if export_result is None else export_result["exit_code"]
            row["wpa_profile_path"] = "" if wpa_profile_path is None else str(wpa_profile_path)
            row["exported_files_count"] = exported_files_count
            row["exports_dir"] = str(exports_dir.relative_to(run_root))
            raw_rows.append(row)

            write_json(
                run_dir / f"{run_id}.profile.json",
                {
                    "run_id": run_id,
                    "wpr_profiles": profile_names,
                    "trace_path": str(trace_path),
                    "trace_size_bytes": row["trace_size_bytes"],
                    "wpr_start": {
                        "command": start_trace["command"],
                        "exit_code": start_trace["exit_code"],
                        "stdout_path": str(start_trace["stdout_path"]),
                        "stderr_path": str(start_trace["stderr_path"]),
                    },
                    "wpr_stop": {
                        "command": stop_trace["command"],
                        "exit_code": stop_trace["exit_code"],
                        "stdout_path": str(stop_trace["stdout_path"]),
                        "stderr_path": str(stop_trace["stderr_path"]),
                    },
                    "wpa_export": None if export_result is None else {
                        "command": export_result["command"],
                        "exit_code": export_result["exit_code"],
                        "stdout_path": str(export_result["stdout_path"]),
                        "stderr_path": str(export_result["stderr_path"]),
                        "exported_files": export_result["exported_files"],
                    },
                    "symbol_path": str(symbol_path),
                },
            )

            log_info(
                "Finished profile "
                f"{row['run_sequence']}/{total_runs}: "
                f"status={row['status']} "
                f"trace_stop={row['wpr_stop_exit_code']} "
                f"avg_net={row['avg_network_speed_mb_s']:.2f}MB/s "
                f"avg_disk={row['avg_disk_speed_mb_s']:.2f}MB/s "
                f"pauses={int(total_pause_count(row))} "
                f"exports={row['exported_files_count']}"
            )

            run_sequence += 1

            if row["exit_code"] != 0 or row["status"] != "success":
                failure = {
                    "sweep_name": sweep_name,
                    "case_name": case.name,
                    "repeat_index": repeat_index,
                    "stage": "download-status",
                    "reason": row.get("error") or "CLI returned failure status",
                }
                break
            if stop_trace["exit_code"] != 0:
                failure = {
                    "sweep_name": sweep_name,
                    "case_name": case.name,
                    "repeat_index": repeat_index,
                    "stage": "wpr-stop",
                    "reason": f"wpr stop failed with exit code {stop_trace['exit_code']}",
                }
                break
            if export_result is not None and export_result["exit_code"] != 0:
                failure = {
                    "sweep_name": sweep_name,
                    "case_name": case.name,
                    "repeat_index": repeat_index,
                    "stage": "wpaexporter",
                    "reason": f"wpaexporter failed with exit code {export_result['exit_code']}",
                }
                break
        if failure is not None:
            break

    write_csv(raw_csv_path, raw_rows, PROFILE_RAW_FIELD_ORDER)
    metadata.update(
        {
            "completed_runs": len(raw_rows),
            "failure": failure,
            "metadata_path": str(metadata_path.relative_to(run_root)),
            "raw_csv_path": str(raw_csv_path.relative_to(run_root)),
            "report_path": str(report_path.relative_to(run_root)),
        }
    )
    write_json(metadata_path, metadata)
    render_profile_report(report_path, metadata, raw_rows, export_indexes, failure)

    if failure is not None:
        log_info(f"Profiler stopped early. Results dir: {run_root}")
        print(
            "Profiler stopped on failure: "
            f"{failure['sweep_name']} / {failure['case_name']} / repeat {failure['repeat_index']} / {failure['stage']}",
            file=sys.stderr,
        )
        print(f"Results written to: {run_root}", file=sys.stderr)
        return 1

    log_info(f"Profiler completed successfully. Results dir: {run_root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

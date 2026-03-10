#!/usr/bin/env python3

from __future__ import annotations

import argparse
import math
import shutil
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Any

from performance_common import (
    DEFAULT_BENCHMARK_EXE,
    DEFAULT_BENCHMARK_OUTPUT_ROOT,
    DEFAULT_OPTIONS,
    DEFAULT_REPEATS,
    RAW_FIELD_ORDER,
    SUMMARY_NUMERIC_FIELDS,
    aggregate_numeric_values,
    build_benchmark_suites,
    build_case_option_summary,
    build_execution_plan,
    build_sweeps,
    default_metadata,
    execute_download_run,
    format_float,
    format_pct,
    log_info,
    make_run_root,
    parse_case_list,
    perform_head_request,
    safe_pct_change,
    total_pause_count,
    validate_execution_inputs,
    write_csv,
    write_json,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", required=True)
    parser.add_argument("--output-root", default=str(DEFAULT_BENCHMARK_OUTPUT_ROOT))
    parser.add_argument("--exe", default=str(DEFAULT_BENCHMARK_EXE))
    parser.add_argument("--repeats", type=int, default=DEFAULT_REPEATS)
    parser.add_argument("--inter-run-delay-ms", type=int, default=100)
    parser.add_argument("--failure-retries", type=int, default=1)
    parser.add_argument("--only-sweep")
    parser.add_argument("--benchmark-suite")
    parser.add_argument("--case-list")
    parser.add_argument("--label", default="")
    parser.add_argument("--keep-downloads", action="store_true")
    parser.add_argument("--overwrite-existing", choices=["true", "false"])
    return parser.parse_args()


def aggregate_runs(
    successful_runs: list[dict[str, Any]],
    baseline_key: tuple[str, str] | None,
    expected_repeats: int,
) -> list[dict[str, Any]]:
    grouped: dict[tuple[str, str], list[dict[str, Any]]] = {}
    for run in successful_runs:
        key = (str(run["sweep_name"]), str(run["case_name"]))
        grouped.setdefault(key, []).append(run)
    aggregated_rows: list[dict[str, Any]] = []
    baseline_speed = None
    for key, runs in grouped.items():
        if len(runs) != expected_repeats:
            continue
        first = runs[0]
        row: dict[str, Any] = {
            "scenario": first["scenario"],
            "label": first["label"],
            "sweep_name": first["sweep_name"],
            "case_name": first["case_name"],
            "case_order": first["case_order"],
            "display_value": first["display_value"],
            "case_purpose": first["case_purpose"],
            "repeats_completed": len(runs),
            "content_length": first["content_length"],
            "max_connections": first["max_connections"],
            "queue_capacity_packets": first["queue_capacity_packets"],
            "scheduler_window_bytes": first["scheduler_window_bytes"],
            "backpressure_high_bytes": first["backpressure_high_bytes"],
            "backpressure_low_bytes": first["backpressure_low_bytes"],
            "block_size": first["block_size"],
            "io_alignment": first["io_alignment"],
            "max_gap_bytes": first["max_gap_bytes"],
            "flush_threshold_bytes": first["flush_threshold_bytes"],
            "flush_interval_ms": first["flush_interval_ms"],
            "overwrite_existing": first["overwrite_existing"],
        }
        for field in SUMMARY_NUMERIC_FIELDS + ["wall_clock_duration_ms"]:
            values = [float(run[field]) for run in runs]
            median_value, min_value, max_value, stdev_value = aggregate_numeric_values(values)
            row[f"{field}_median"] = median_value
            row[f"{field}_min"] = min_value
            row[f"{field}_max"] = max_value
            row[f"{field}_stdev"] = stdev_value
        resumed_values = [bool(run["resumed"]) for run in runs]
        row["all_resumed"] = all(resumed_values)
        row["any_resumed"] = any(resumed_values)
        row["total_pause_count_median"] = total_pause_count(row, "_median")
        aggregated_rows.append(row)
        if baseline_key is not None and key == baseline_key:
            baseline_speed = row["avg_network_speed_mb_s_median"]
    aggregated_rows.sort(key=lambda item: (str(item["sweep_name"]), int(item["case_order"])))
    for row in aggregated_rows:
        if baseline_speed is None:
            row["avg_network_speed_gain_vs_baseline_pct"] = math.nan
        else:
            row["avg_network_speed_gain_vs_baseline_pct"] = safe_pct_change(
                float(baseline_speed),
                float(row["avg_network_speed_mb_s_median"]),
            )
    return aggregated_rows


def better_case(candidate: dict[str, Any], current_best: dict[str, Any]) -> bool:
    candidate_speed = float(candidate["avg_network_speed_mb_s_median"])
    current_speed = float(current_best["avg_network_speed_mb_s_median"])
    if candidate_speed > current_speed * 1.05:
        return True
    if current_speed > candidate_speed * 1.05:
        return False
    candidate_memory = float(candidate["max_memory_bytes_median"])
    current_memory = float(current_best["max_memory_bytes_median"])
    if candidate_memory != current_memory:
        return candidate_memory < current_memory
    candidate_pauses = float(candidate["total_pause_count_median"])
    current_pauses = float(current_best["total_pause_count_median"])
    if candidate_pauses != current_pauses:
        return candidate_pauses < current_pauses
    candidate_inflight = float(candidate["max_inflight_bytes_median"])
    current_inflight = float(current_best["max_inflight_bytes_median"])
    if candidate_inflight != current_inflight:
        return candidate_inflight < current_inflight
    return int(candidate["case_order"]) < int(current_best["case_order"])


def find_best_cases(aggregated_rows: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    best: dict[str, dict[str, Any]] = {}
    for row in aggregated_rows:
        sweep_name = str(row["sweep_name"])
        if sweep_name == "baseline":
            continue
        if sweep_name not in best or better_case(row, best[sweep_name]):
            best[sweep_name] = row
    return best


def detect_inflections(aggregated_rows: list[dict[str, Any]]) -> dict[str, dict[str, Any] | None]:
    grouped: dict[str, list[dict[str, Any]]] = {}
    for row in aggregated_rows:
        sweep_name = str(row["sweep_name"])
        if sweep_name == "baseline":
            continue
        grouped.setdefault(sweep_name, []).append(row)
    detected_rows: dict[str, dict[str, Any] | None] = {}
    for sweep_name, rows in grouped.items():
        rows.sort(key=lambda item: int(item["case_order"]))
        detected: dict[str, Any] | None = None
        for previous, current in zip(rows, rows[1:]):
            throughput_improvement = safe_pct_change(
                float(previous["avg_network_speed_mb_s_median"]),
                float(current["avg_network_speed_mb_s_median"]),
            )
            memory_growth = safe_pct_change(
                float(previous["max_memory_bytes_median"]),
                float(current["max_memory_bytes_median"]),
            )
            pause_growth = safe_pct_change(
                float(previous["total_pause_count_median"]),
                float(current["total_pause_count_median"]),
            )
            inflight_growth = safe_pct_change(
                float(previous["max_inflight_bytes_median"]),
                float(current["max_inflight_bytes_median"]),
            )
            if throughput_improvement < 5.0 and (
                memory_growth > 20.0 or
                pause_growth > 50.0 or
                inflight_growth > 25.0
            ):
                detected = {
                    "row": current,
                    "throughput_improvement_pct": throughput_improvement,
                    "memory_growth_pct": memory_growth,
                    "pause_growth_pct": pause_growth,
                    "inflight_growth_pct": inflight_growth,
                }
                break
        detected_rows[sweep_name] = detected
    return detected_rows


def render_markdown_report(
    report_path: Path,
    metadata: dict[str, Any],
    aggregated_rows: list[dict[str, Any]],
    raw_rows: list[dict[str, Any]],
    failure: dict[str, Any] | None,
) -> None:
    baseline_row = next((row for row in aggregated_rows if row["sweep_name"] == "baseline"), None)
    best_cases = find_best_cases(aggregated_rows)
    inflections = detect_inflections(aggregated_rows)
    execution_mode = str(metadata.get("execution_mode", "all_sweeps"))
    benchmark_suite_name = str(metadata.get("benchmark_suite_name") or "")
    grouped_rows: dict[str, list[dict[str, Any]]] = {}
    for row in aggregated_rows:
        grouped_rows.setdefault(str(row["sweep_name"]), []).append(row)

    lines: list[str] = []
    lines.append(f"# Benchmark Report: {metadata['label']}")
    lines.append("")
    lines.append("## Overview")
    lines.append("")
    lines.append("| Item | Value |")
    lines.append("| --- | --- |")
    lines.append(f"| Generated At | {metadata['generated_at']} |")
    lines.append(f"| Scenario | {metadata['scenario']} |")
    lines.append(f"| URL | {metadata['head']['requested_url']} |")
    lines.append(f"| Resolved URL | {metadata['head']['resolved_url']} |")
    lines.append(f"| Content Length | {metadata['head']['content_length']} |")
    lines.append(f"| Accept-Ranges | {metadata['head']['accept_ranges']} |")
    lines.append(f"| ETag | {metadata['head']['etag'] or '(empty)'} |")
    lines.append(f"| Last-Modified | {metadata['head']['last_modified'] or '(empty)'} |")
    lines.append(f"| CLI | {metadata['exe_path']} |")
    lines.append(f"| Repeats | {metadata['repeats']} |")
    lines.append(f"| Planned Runs | {metadata['planned_runs']} |")
    lines.append(f"| Attempted Invocations | {metadata['attempted_runs']} |")
    lines.append(f"| Inter-run Delay Ms | {metadata['inter_run_delay_ms']} |")
    lines.append(f"| Failure Retries | {metadata['failure_retries']} |")
    lines.append(f"| Execution Mode | {execution_mode} |")
    lines.append(f"| Benchmark Suite | {benchmark_suite_name if benchmark_suite_name else '(none)'} |")
    lines.append(f"| Completed Runs | {len(raw_rows)} |")
    lines.append(f"| Host | {metadata['host']} |")
    lines.append(f"| Python | {metadata['python_version']} |")
    lines.append("")

    if failure is not None:
        lines.append("## Failure")
        lines.append("")
        lines.append("| Item | Value |")
        lines.append("| --- | --- |")
        lines.append(f"| Sweep | {failure['sweep_name']} |")
        lines.append(f"| Case | {failure['case_name']} |")
        lines.append(f"| Repeat | {failure['repeat_index']} |")
        lines.append(f"| Attempt | {failure['attempt_index']} |")
        lines.append(f"| Reason | {failure['reason']} |")
        lines.append(f"| Exit Code | {failure['exit_code']} |")
        lines.append(f"| Summary | {failure['summary_path']} |")
        lines.append(f"| Stdout | {failure['stdout_path']} |")
        lines.append(f"| Stderr | {failure['stderr_path']} |")
        lines.append("")

    if baseline_row is not None:
        lines.append("## Baseline")
        lines.append("")
        lines.append("| Metric | Value |")
        lines.append("| --- | --- |")
        lines.append(f"| Avg Network MB/s | {format_float(float(baseline_row['avg_network_speed_mb_s_median']))} |")
        lines.append(f"| Avg Disk MB/s | {format_float(float(baseline_row['avg_disk_speed_mb_s_median']))} |")
        lines.append(f"| Peak Network MB/s | {format_float(float(baseline_row['peak_network_speed_mb_s_median']))} |")
        lines.append(f"| Max Memory Bytes | {int(round(float(baseline_row['max_memory_bytes_median'])))} |")
        lines.append(f"| Max Inflight Bytes | {int(round(float(baseline_row['max_inflight_bytes_median'])))} |")
        lines.append(f"| Total Pause Count | {format_float(float(baseline_row['total_pause_count_median']))} |")
        lines.append("")

    if execution_mode == "benchmark_suite":
        lines.append("## Benchmark Suite Cases")
        lines.append("")
        lines.append("| Case | Value | Purpose | Avg Network MB/s | Avg Disk MB/s | Gain vs Baseline | Max Memory Bytes | Total Pauses | Max Inflight Bytes |")
        lines.append("| --- | --- | --- | --- | --- | --- | --- | --- | --- |")
        for row in sorted(grouped_rows.get(benchmark_suite_name, []), key=lambda item: int(item["case_order"])):
            lines.append(
                f"| {row['case_name']} | {row['display_value']} | {row['case_purpose']} | "
                f"{format_float(float(row['avg_network_speed_mb_s_median']))} | "
                f"{format_float(float(row['avg_disk_speed_mb_s_median']))} | "
                f"{format_pct(float(row['avg_network_speed_gain_vs_baseline_pct']))} | "
                f"{int(round(float(row['max_memory_bytes_median'])))} | "
                f"{format_float(float(row['total_pause_count_median']))} | "
                f"{int(round(float(row['max_inflight_bytes_median'])))} |"
            )
        lines.append("")
    else:
        lines.append("## Best Cases")
        lines.append("")
        lines.append("| Sweep | Case | Value | Avg Network MB/s | Max Memory Bytes | Total Pause Count | Max Inflight Bytes |")
        lines.append("| --- | --- | --- | --- | --- | --- | --- |")
        for sweep_name in metadata["executed_sweeps"]:
            if sweep_name == "baseline":
                continue
            row = best_cases.get(sweep_name)
            if row is None:
                continue
            lines.append(
                f"| {sweep_name} | {row['case_name']} | {row['display_value']} | "
                f"{format_float(float(row['avg_network_speed_mb_s_median']))} | "
                f"{int(round(float(row['max_memory_bytes_median'])))} | "
                f"{format_float(float(row['total_pause_count_median']))} | "
                f"{int(round(float(row['max_inflight_bytes_median'])))} |"
            )
        lines.append("")

        lines.append("## Inflection Points")
        lines.append("")
        lines.append("| Sweep | Inflection | Throughput Delta | Memory Delta | Pause Delta | Inflight Delta |")
        lines.append("| --- | --- | --- | --- | --- | --- |")
        for sweep_name in metadata["executed_sweeps"]:
            if sweep_name == "baseline":
                continue
            detected = inflections.get(sweep_name)
            if not detected:
                lines.append(f"| {sweep_name} | none | n/a | n/a | n/a | n/a |")
                continue
            row = detected["row"]
            lines.append(
                f"| {sweep_name} | {row['case_name']} ({row['display_value']}) | "
                f"{format_pct(float(detected['throughput_improvement_pct']))} | "
                f"{format_pct(float(detected['memory_growth_pct']))} | "
                f"{format_pct(float(detected['pause_growth_pct']))} | "
                f"{format_pct(float(detected['inflight_growth_pct']))} |"
            )
        lines.append("")

    lines.append("## Case Tables")
    lines.append("")
    for sweep_name in metadata["executed_sweeps"]:
        rows = grouped_rows.get(sweep_name, [])
        if not rows:
            continue
        lines.append(f"### {sweep_name}")
        lines.append("")
        lines.append("| Case | Value | Avg Network MB/s | Avg Disk MB/s | Gain vs Baseline | Max Memory Bytes | Total Pauses | Max Inflight Bytes |")
        lines.append("| --- | --- | --- | --- | --- | --- | --- | --- |")
        for row in sorted(rows, key=lambda item: int(item["case_order"])):
            lines.append(
                f"| {row['case_name']} | {row['display_value']} | "
                f"{format_float(float(row['avg_network_speed_mb_s_median']))} | "
                f"{format_float(float(row['avg_disk_speed_mb_s_median']))} | "
                f"{format_pct(float(row['avg_network_speed_gain_vs_baseline_pct']))} | "
                f"{int(round(float(row['max_memory_bytes_median'])))} | "
                f"{format_float(float(row['total_pause_count_median']))} | "
                f"{int(round(float(row['max_inflight_bytes_median'])))} |"
            )
        lines.append("")

    lines.append("## Files")
    lines.append("")
    lines.append("| File | Path |")
    lines.append("| --- | --- |")
    lines.append(f"| Metadata | {metadata['metadata_path']} |")
    lines.append(f"| Raw Runs CSV | {metadata['raw_csv_path']} |")
    lines.append(f"| Aggregated CSV | {metadata['aggregated_csv_path']} |")
    lines.append(f"| Report | {metadata['report_path']} |")

    report_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()
    sweeps = build_sweeps()
    benchmark_suites = build_benchmark_suites()
    sweep_names = {sweep.name for sweep in sweeps}
    benchmark_suite_names = set(benchmark_suites)
    try:
        if args.inter_run_delay_ms < 0:
            raise RuntimeError("--inter-run-delay-ms must be zero or greater")
        if args.failure_retries < 0:
            raise RuntimeError("--failure-retries must be zero or greater")
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
        execution_mode, benchmark_suite, execution_plan, executed_sweeps = build_execution_plan(
            sweeps=sweeps,
            benchmark_suites=benchmark_suites,
            only_sweep=args.only_sweep,
            benchmark_suite_name=args.benchmark_suite,
            requested_case_names=requested_case_names,
        )
    except RuntimeError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    log_info(f"CLI: {context.exe_path}")
    log_info(
        "HEAD ok: "
        f"status={head_metadata['http_status']} "
        f"size={head_metadata['content_length']} "
        f"accept_ranges={head_metadata['accept_ranges']}"
    )

    label = args.label or "benchmark"
    run_root = make_run_root(context.output_root, label)
    benchmark_metadata_path = run_root / "benchmark_metadata.json"
    raw_csv_path = run_root / "raw_runs.csv"
    aggregated_csv_path = run_root / "aggregated_cases.csv"
    report_path = run_root / "report.md"

    raw_rows: list[dict[str, Any]] = []
    failure: dict[str, Any] | None = None
    run_sequence = 1
    attempted_invocations = 0

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
    total_runs = len(execution_plan) * args.repeats
    metadata["inter_run_delay_ms"] = args.inter_run_delay_ms
    metadata["failure_retries"] = args.failure_retries
    metadata["planned_runs"] = total_runs
    metadata["attempted_runs"] = 0
    write_json(benchmark_metadata_path, metadata)
    log_info(f"Results dir: {run_root}")
    log_info(f"Execution plan: {len(execution_plan)} cases x {args.repeats} repeats = {total_runs} runs")
    log_info(
        f"Failure policy: retry each failed repeat up to {args.failure_retries} extra time(s)"
    )
    if args.inter_run_delay_ms > 0:
        log_info(f"Inter-run delay: {args.inter_run_delay_ms} ms")
    if requested_case_names:
        log_info(f"Selected cases: {', '.join(requested_case_names)}")

    for sweep_name, case, overrides in execution_plan:
        for repeat_index in range(1, args.repeats + 1):
            options = dict(DEFAULT_OPTIONS)
            options["overwrite_existing"] = context.overwrite_existing
            options.update(overrides)
            log_info(
                f"Starting run {run_sequence}/{total_runs}: "
                f"{sweep_name} / {case.name} / repeat {repeat_index} | "
                f"{build_case_option_summary(options)}"
            )
            repeat_succeeded = False
            attempt_failure: dict[str, Any] | None = None
            final_row: dict[str, Any] | None = None
            max_attempts = args.failure_retries + 1
            run_id = f"{sweep_name}__{case.name}__r{repeat_index:02d}"
            run_dir = run_root / "runs" / run_id
            for attempt_index in range(1, max_attempts + 1):
                if attempt_index > 1 and args.inter_run_delay_ms > 0:
                    log_info(
                        f"Waiting {args.inter_run_delay_ms} ms before retry "
                        f"{attempt_index}/{max_attempts}: {sweep_name} / {case.name} / repeat {repeat_index}"
                    )
                    time.sleep(args.inter_run_delay_ms / 1000.0)
                if run_dir.exists():
                    shutil.rmtree(run_dir)
                run_dir.mkdir(parents=True, exist_ok=False)
                attempted_invocations += 1
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
                        attempt_index=attempt_index,
                        keep_downloads=args.keep_downloads,
                        extra_run_metadata={"max_attempts": max_attempts},
                        run_dir=run_dir,
                    )
                    log_info(
                        "Finished run "
                        f"{row['run_sequence']}/{total_runs} "
                        f"(attempt {attempt_index}/{max_attempts}): "
                        f"status={row['status']} "
                        f"exit={row['exit_code']} "
                        f"duration_ms={row['wall_clock_duration_ms']} "
                        f"avg_net={row['avg_network_speed_mb_s']:.2f}MB/s "
                        f"avg_disk={row['avg_disk_speed_mb_s']:.2f}MB/s "
                        f"memory={row['max_memory_bytes']} "
                        f"pauses={int(total_pause_count(row))}"
                    )
                    final_row = row
                    if row["exit_code"] == 0 and row["status"] == "success":
                        repeat_succeeded = True
                        attempt_failure = None
                        run_sequence += 1
                        break
                    attempt_failure = {
                        "sweep_name": sweep_name,
                        "case_name": case.name,
                        "repeat_index": repeat_index,
                        "attempt_index": attempt_index,
                        "reason": row.get("error") or "CLI returned failure status",
                        "exit_code": row["exit_code"],
                        "summary_path": row["summary_path"],
                        "stdout_path": row["stdout_path"],
                        "stderr_path": row["stderr_path"],
                    }
                except Exception as exc:
                    log_info(
                        "Run failed before completion: "
                        f"{sweep_name} / {case.name} / repeat {repeat_index} / "
                        f"attempt {attempt_index}/{max_attempts} | {exc}"
                    )
                    attempt_failure = {
                        "sweep_name": sweep_name,
                        "case_name": case.name,
                        "repeat_index": repeat_index,
                        "attempt_index": attempt_index,
                        "reason": str(exc),
                        "exit_code": -1,
                        "summary_path": str((run_dir / f"{run_id}.summary.txt").relative_to(run_root)),
                        "stdout_path": str((run_dir / f"{run_id}.stdout.txt").relative_to(run_root)),
                        "stderr_path": str((run_dir / f"{run_id}.stderr.txt").relative_to(run_root)),
                    }
                if attempt_index < max_attempts:
                    log_info(
                        "Retrying failed run: "
                        f"{sweep_name} / {case.name} / repeat {repeat_index} / "
                        f"next attempt {attempt_index + 1}/{max_attempts}"
                    )
            if final_row is not None:
                raw_rows.append(final_row)
            if not repeat_succeeded:
                failure = attempt_failure
                break
            if args.inter_run_delay_ms > 0 and run_sequence <= total_runs:
                time.sleep(args.inter_run_delay_ms / 1000.0)
        if failure is not None:
            break

    successful_rows = [row for row in raw_rows if row["exit_code"] == 0 and row["status"] == "success"]
    aggregated_rows = aggregate_runs(successful_rows, baseline_key=("baseline", "baseline_default"), expected_repeats=args.repeats)

    aggregated_field_order = [
        "scenario",
        "label",
        "sweep_name",
        "case_name",
        "case_order",
        "display_value",
        "case_purpose",
        "repeats_completed",
        "content_length",
        "max_connections",
        "queue_capacity_packets",
        "scheduler_window_bytes",
        "backpressure_high_bytes",
        "backpressure_low_bytes",
        "block_size",
        "io_alignment",
        "max_gap_bytes",
        "flush_threshold_bytes",
        "flush_interval_ms",
        "overwrite_existing",
        "all_resumed",
        "any_resumed",
        "total_pause_count_median",
        "avg_network_speed_gain_vs_baseline_pct",
    ]
    for field in SUMMARY_NUMERIC_FIELDS + ["wall_clock_duration_ms"]:
        aggregated_field_order.extend(
            [
                f"{field}_median",
                f"{field}_min",
                f"{field}_max",
                f"{field}_stdev",
            ]
        )

    write_csv(raw_csv_path, raw_rows, RAW_FIELD_ORDER)
    write_csv(aggregated_csv_path, aggregated_rows, aggregated_field_order)

    metadata.update(
        {
            "completed_runs": len(raw_rows),
            "attempted_runs": attempted_invocations,
            "successful_runs": len(successful_rows),
            "failure": failure,
            "metadata_path": str(benchmark_metadata_path.relative_to(run_root)),
            "raw_csv_path": str(raw_csv_path.relative_to(run_root)),
            "aggregated_csv_path": str(aggregated_csv_path.relative_to(run_root)),
            "report_path": str(report_path.relative_to(run_root)),
        }
    )
    write_json(benchmark_metadata_path, metadata)
    render_markdown_report(report_path, metadata, aggregated_rows, raw_rows, failure)

    if failure is not None:
        log_info(f"Stopped early. Results dir: {run_root}")
        print(
            "Benchmark stopped on failure: "
            f"{failure['sweep_name']} / {failure['case_name']} / repeat {failure['repeat_index']}",
            file=sys.stderr,
        )
        print(f"Results written to: {run_root}", file=sys.stderr)
        return 1

    log_info(f"Benchmark completed successfully. Results dir: {run_root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

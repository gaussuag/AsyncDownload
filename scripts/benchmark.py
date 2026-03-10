#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import json
import math
import platform
import re
import shutil
import statistics
import subprocess
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EXE = Path("build/src/Debug/AsyncDownload.exe")
DEFAULT_OUTPUT_ROOT = Path("build/benchmarks")
DEFAULT_REPEATS = 3
MIB = 1024 * 1024
KIB = 1024

DEFAULT_OPTIONS: dict[str, Any] = {
    "max_connections": 4,
    "queue_capacity_packets": 4096,
    "scheduler_window_bytes": 4 * MIB,
    "backpressure_high_bytes": 256 * MIB,
    "backpressure_low_bytes": 128 * MIB,
    "block_size": 64 * KIB,
    "io_alignment": 4 * KIB,
    "max_gap_bytes": 32 * MIB,
    "flush_threshold_bytes": 16 * MIB,
    "flush_interval_ms": 2000,
    "overwrite_existing": True,
}

SUMMARY_SPECS: list[tuple[str, str, str]] = [
    ("status", "status", "str"),
    ("total_bytes", "total_bytes", "int"),
    ("downloaded_bytes", "downloaded_bytes", "int"),
    ("persisted_bytes", "persisted_bytes", "int"),
    ("duration_ms", "duration_ms", "int"),
    ("avg_network_speed", "avg_network_speed_mb_s", "speed"),
    ("avg_disk_speed", "avg_disk_speed_mb_s", "speed"),
    ("peak_network_speed", "peak_network_speed_mb_s", "speed"),
    ("peak_disk_speed", "peak_disk_speed_mb_s", "speed"),
    ("time_to_first_byte_ms", "time_to_first_byte_ms", "int"),
    ("time_to_first_persist_ms", "time_to_first_persist_ms", "int"),
    ("resumed", "resumed", "bool"),
    ("resume_reused_bytes", "resume_reused_bytes", "int"),
    ("max_memory_bytes", "max_memory_bytes", "int"),
    ("max_inflight_bytes", "max_inflight_bytes", "int"),
    ("max_queued_packets", "max_queued_packets", "int"),
    ("max_active_requests", "max_active_requests", "int"),
    ("memory_pause_count", "memory_pause_count", "int"),
    ("queue_full_pause_count", "queue_full_pause_count", "int"),
    ("window_boundary_pause_count", "window_boundary_pause_count", "int"),
    ("gap_pause_count", "gap_pause_count", "int"),
    ("windows_total", "windows_total", "int"),
    ("ranges_total", "ranges_total", "int"),
    ("ranges_stolen", "ranges_stolen", "int"),
    ("write_callback_calls", "write_callback_calls", "int"),
    ("packets_enqueued_total", "packets_enqueued_total", "int"),
    ("avg_packet_size_bytes", "avg_packet_size_bytes", "float"),
    ("max_packet_size_bytes", "max_packet_size_bytes", "int"),
    ("flush_count", "flush_count", "int"),
    ("flush_time_ms_total", "flush_time_ms_total", "int"),
    ("metadata_save_count", "metadata_save_count", "int"),
    ("metadata_save_time_ms_total", "metadata_save_time_ms_total", "int"),
]

SUMMARY_REQUIRED_KEYS = {source_key for source_key, _, _ in SUMMARY_SPECS}
SUMMARY_NUMERIC_FIELDS = [
    target_key for _, target_key, field_type in SUMMARY_SPECS
    if field_type in {"int", "float", "speed"}
]
RAW_FIELD_ORDER = [
    "run_id",
    "run_sequence",
    "scenario",
    "label",
    "sweep_name",
    "case_name",
    "case_order",
    "repeat_index",
    "url",
    "resolved_url",
    "content_length",
    "accept_ranges",
    "etag",
    "last_modified",
    "http_status",
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
    "status",
    "exit_code",
    "wall_clock_duration_ms",
    "stdout_path",
    "stderr_path",
    "summary_path",
    "config_path",
    "run_metadata_path",
    "output_path",
    "temporary_path",
    "metadata_path",
    "error",
] + SUMMARY_NUMERIC_FIELDS + ["resumed"]


@dataclass(frozen=True)
class CaseDefinition:
    name: str
    overrides: dict[str, Any]
    display_value: str
    order: int


@dataclass(frozen=True)
class SweepDefinition:
    name: str
    cases: list[CaseDefinition]


def build_sweeps() -> list[SweepDefinition]:
    return [
        SweepDefinition(
            name="max_connections",
            cases=[
                CaseDefinition("conn_1", {"max_connections": 1}, "1", 1),
                CaseDefinition("conn_2", {"max_connections": 2}, "2", 2),
                CaseDefinition("conn_4", {"max_connections": 4}, "4", 3),
                CaseDefinition("conn_8", {"max_connections": 8}, "8", 4),
                CaseDefinition("conn_16", {"max_connections": 16}, "16", 5),
            ],
        ),
        SweepDefinition(
            name="scheduler_window_bytes",
            cases=[
                CaseDefinition("window_1MiB", {"scheduler_window_bytes": 1 * MIB}, "1 MiB", 1),
                CaseDefinition("window_4MiB", {"scheduler_window_bytes": 4 * MIB}, "4 MiB", 2),
                CaseDefinition("window_16MiB", {"scheduler_window_bytes": 16 * MIB}, "16 MiB", 3),
                CaseDefinition("window_64MiB", {"scheduler_window_bytes": 64 * MIB}, "64 MiB", 4),
            ],
        ),
        SweepDefinition(
            name="queue_capacity_packets",
            cases=[
                CaseDefinition("queue_256", {"queue_capacity_packets": 256}, "256", 1),
                CaseDefinition("queue_1024", {"queue_capacity_packets": 1024}, "1024", 2),
                CaseDefinition("queue_4096", {"queue_capacity_packets": 4096}, "4096", 3),
                CaseDefinition("queue_16384", {"queue_capacity_packets": 16384}, "16384", 4),
            ],
        ),
        SweepDefinition(
            name="backpressure_pair",
            cases=[
                CaseDefinition(
                    "bp_64MiB_32MiB",
                    {"backpressure_high_bytes": 64 * MIB, "backpressure_low_bytes": 32 * MIB},
                    "64/32 MiB",
                    1,
                ),
                CaseDefinition(
                    "bp_128MiB_64MiB",
                    {"backpressure_high_bytes": 128 * MIB, "backpressure_low_bytes": 64 * MIB},
                    "128/64 MiB",
                    2,
                ),
                CaseDefinition(
                    "bp_256MiB_128MiB",
                    {"backpressure_high_bytes": 256 * MIB, "backpressure_low_bytes": 128 * MIB},
                    "256/128 MiB",
                    3,
                ),
                CaseDefinition(
                    "bp_512MiB_256MiB",
                    {"backpressure_high_bytes": 512 * MIB, "backpressure_low_bytes": 256 * MIB},
                    "512/256 MiB",
                    4,
                ),
            ],
        ),
    ]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", required=True)
    parser.add_argument("--output-root", default=str(DEFAULT_OUTPUT_ROOT))
    parser.add_argument("--exe", default=str(DEFAULT_EXE))
    parser.add_argument("--repeats", type=int, default=DEFAULT_REPEATS)
    parser.add_argument("--only-sweep")
    parser.add_argument("--label", default="")
    parser.add_argument("--keep-downloads", action="store_true")
    parser.add_argument("--overwrite-existing", choices=["true", "false"])
    return parser.parse_args()


def resolve_repo_path(path_text: str) -> Path:
    path = Path(path_text)
    if path.is_absolute():
        return path
    return REPO_ROOT / path


def log_info(message: str) -> None:
    timestamp = datetime.now().strftime("%H:%M:%S")
    print(f"[{timestamp}] {message}", flush=True)


def sanitize_label(text: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9._-]+", "_", text.strip())
    cleaned = cleaned.strip("._-")
    return cleaned or "benchmark"


def make_run_root(output_root: Path, label: str) -> Path:
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    base = output_root / f"{timestamp}_{sanitize_label(label)}"
    candidate = base
    index = 2
    while candidate.exists():
        candidate = output_root / f"{base.name}_{index}"
        index += 1
    candidate.mkdir(parents=True, exist_ok=False)
    return candidate


def perform_head_request(url: str) -> dict[str, Any]:
    request = urllib.request.Request(
        url,
        method="HEAD",
        headers={"User-Agent": "AsyncDownloadBenchmark/1.0"},
    )
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            headers = response.headers
            content_length_text = headers.get("Content-Length")
            if content_length_text is None:
                raise RuntimeError("HEAD response missing Content-Length")
            try:
                content_length = int(content_length_text)
            except ValueError as exc:
                raise RuntimeError("HEAD response contains invalid Content-Length") from exc
            accept_ranges = headers.get("Accept-Ranges", "")
            if "bytes" not in accept_ranges.lower():
                raise RuntimeError("HEAD response does not advertise Accept-Ranges: bytes")
            return {
                "requested_url": url,
                "resolved_url": response.geturl(),
                "http_status": getattr(response, "status", 200),
                "content_length": content_length,
                "accept_ranges": accept_ranges,
                "etag": headers.get("ETag", ""),
                "last_modified": headers.get("Last-Modified", ""),
                "content_type": headers.get("Content-Type", ""),
            }
    except urllib.error.HTTPError as exc:
        raise RuntimeError(f"HEAD request failed with HTTP {exc.code}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"HEAD request failed: {exc.reason}") from exc


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True), encoding="utf-8")


def write_config(path: Path, options: dict[str, Any]) -> None:
    write_json(path, {"download_options": options})


def parse_bool(text: str) -> bool:
    lowered = text.strip().lower()
    if lowered == "true":
        return True
    if lowered == "false":
        return False
    raise ValueError(f"invalid boolean value: {text}")


def parse_speed(text: str) -> float:
    normalized = text.strip()
    if normalized.endswith("MB/s"):
        normalized = normalized[:-4].strip()
    return float(normalized)


def parse_summary(summary_path: Path) -> dict[str, Any]:
    if not summary_path.exists():
        raise RuntimeError("summary file was not created")
    raw_text = summary_path.read_text(encoding="utf-8", errors="replace")
    parsed: dict[str, str] = {}
    for line in raw_text.splitlines():
        stripped = line.strip()
        if not stripped or stripped == "Summary" or "=" not in stripped:
            continue
        key, value = stripped.split("=", 1)
        parsed[key.strip()] = value.strip()
    missing = sorted(SUMMARY_REQUIRED_KEYS - parsed.keys())
    if missing:
        raise RuntimeError(f"summary file missing required fields: {', '.join(missing)}")
    result: dict[str, Any] = {}
    for source_key, target_key, field_type in SUMMARY_SPECS:
        raw_value = parsed[source_key]
        if field_type == "str":
            result[target_key] = raw_value
        elif field_type == "int":
            result[target_key] = int(raw_value)
        elif field_type == "float":
            result[target_key] = float(raw_value)
        elif field_type == "speed":
            result[target_key] = parse_speed(raw_value)
        elif field_type == "bool":
            result[target_key] = parse_bool(raw_value)
        else:
            raise RuntimeError(f"unsupported summary field type: {field_type}")
    result["error"] = parsed.get("error", "")
    return result


def format_relative(path: Path, root: Path) -> str:
    try:
        return str(path.relative_to(root))
    except ValueError:
        return str(path)


def cleanup_download_artifacts(output_path: Path) -> None:
    for artifact in (
        output_path,
        Path(str(output_path) + ".part"),
        Path(str(output_path) + ".config.json"),
    ):
        if artifact.exists():
            if artifact.is_dir():
                shutil.rmtree(artifact)
            else:
                artifact.unlink()


def total_pause_count(row: dict[str, Any], suffix: str = "") -> float:
    return (
        float(row[f"memory_pause_count{suffix}"]) +
        float(row[f"queue_full_pause_count{suffix}"]) +
        float(row[f"window_boundary_pause_count{suffix}"]) +
        float(row[f"gap_pause_count{suffix}"])
    )


def safe_pct_change(previous: float, current: float) -> float:
    if previous == 0.0:
        if current == 0.0:
            return 0.0
        return math.inf
    return ((current - previous) / previous) * 100.0


def write_csv(path: Path, rows: list[dict[str, Any]], field_order: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=field_order, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def aggregate_numeric_values(values: list[float]) -> tuple[float, float, float, float]:
    median_value = statistics.median(values)
    min_value = min(values)
    max_value = max(values)
    stdev_value = statistics.stdev(values) if len(values) >= 2 else 0.0
    return float(median_value), float(min_value), float(max_value), float(stdev_value)


def build_case_option_summary(options: dict[str, Any]) -> str:
    return (
        "conn={max_connections}, "
        "queue={queue_capacity_packets}, "
        "window={scheduler_window_bytes}, "
        "bp={backpressure_high_bytes}/{backpressure_low_bytes}, "
        "flush={flush_threshold_bytes}@{flush_interval_ms}ms, "
        "gap={max_gap_bytes}"
    ).format(**options)


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


def format_pct(value: float) -> str:
    if math.isinf(value):
        return "inf"
    if math.isnan(value):
        return "n/a"
    return f"{value:.2f}%"


def format_float(value: float, digits: int = 2) -> str:
    if math.isnan(value):
        return "n/a"
    return f"{value:.{digits}f}"


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


def execute_run(
    *,
    exe_path: Path,
    url: str,
    run_root: Path,
    sweep_name: str,
    case: CaseDefinition,
    repeat_index: int,
    options: dict[str, Any],
    head_metadata: dict[str, Any],
    label: str,
    scenario: str,
    run_sequence: int,
    keep_downloads: bool,
) -> dict[str, Any]:
    run_id = f"{sweep_name}__{case.name}__r{repeat_index:02d}"
    run_dir = run_root / "runs" / run_id
    run_dir.mkdir(parents=True, exist_ok=False)

    output_path = run_dir / f"{run_id}.bin"
    summary_path = run_dir / f"{run_id}.summary.txt"
    stdout_path = run_dir / f"{run_id}.stdout.txt"
    stderr_path = run_dir / f"{run_id}.stderr.txt"
    config_path = run_dir / f"{run_id}.config.json"
    run_metadata_path = run_dir / f"{run_id}.run.json"

    write_config(config_path, options)

    command = [
        str(exe_path),
        url,
        str(output_path),
        "--config",
        str(config_path),
        "--summary-file",
        str(summary_path),
    ]

    started_at = datetime.now()
    started_perf = time.perf_counter()
    completed = subprocess.run(
        command,
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    ended_perf = time.perf_counter()
    ended_at = datetime.now()

    stdout_path.write_text(completed.stdout, encoding="utf-8")
    stderr_path.write_text(completed.stderr, encoding="utf-8")

    summary_data = parse_summary(summary_path)
    wall_clock_duration_ms = int(round((ended_perf - started_perf) * 1000.0))
    row: dict[str, Any] = {
        "run_id": run_id,
        "run_sequence": run_sequence,
        "scenario": scenario,
        "label": label,
        "sweep_name": sweep_name,
        "case_name": case.name,
        "case_order": case.order,
        "display_value": case.display_value,
        "repeat_index": repeat_index,
        "url": head_metadata["requested_url"],
        "resolved_url": head_metadata["resolved_url"],
        "content_length": head_metadata["content_length"],
        "accept_ranges": head_metadata["accept_ranges"],
        "etag": head_metadata["etag"],
        "last_modified": head_metadata["last_modified"],
        "http_status": head_metadata["http_status"],
        "max_connections": options["max_connections"],
        "queue_capacity_packets": options["queue_capacity_packets"],
        "scheduler_window_bytes": options["scheduler_window_bytes"],
        "backpressure_high_bytes": options["backpressure_high_bytes"],
        "backpressure_low_bytes": options["backpressure_low_bytes"],
        "block_size": options["block_size"],
        "io_alignment": options["io_alignment"],
        "max_gap_bytes": options["max_gap_bytes"],
        "flush_threshold_bytes": options["flush_threshold_bytes"],
        "flush_interval_ms": options["flush_interval_ms"],
        "overwrite_existing": options["overwrite_existing"],
        "exit_code": completed.returncode,
        "wall_clock_duration_ms": wall_clock_duration_ms,
        "stdout_path": format_relative(stdout_path, run_root),
        "stderr_path": format_relative(stderr_path, run_root),
        "summary_path": format_relative(summary_path, run_root),
        "config_path": format_relative(config_path, run_root),
        "run_metadata_path": format_relative(run_metadata_path, run_root),
        "output_path": format_relative(output_path, run_root),
        "temporary_path": format_relative(Path(str(output_path) + ".part"), run_root),
        "metadata_path": format_relative(Path(str(output_path) + ".config.json"), run_root),
    }
    row.update(summary_data)

    write_json(
        run_metadata_path,
        {
            "run_id": run_id,
            "run_sequence": run_sequence,
            "scenario": scenario,
            "label": label,
            "command": command,
            "started_at": started_at.isoformat(timespec="seconds"),
            "ended_at": ended_at.isoformat(timespec="seconds"),
            "wall_clock_duration_ms": wall_clock_duration_ms,
            "exit_code": completed.returncode,
            "options": options,
            "summary": summary_data,
            "head": head_metadata,
            "files": {
                "config": str(config_path),
                "summary": str(summary_path),
                "stdout": str(stdout_path),
                "stderr": str(stderr_path),
                "output": str(output_path),
                "temporary": str(Path(str(output_path) + ".part")),
                "metadata": str(Path(str(output_path) + ".config.json")),
            },
        },
    )

    if not keep_downloads:
        cleanup_download_artifacts(output_path)

    return row


def validate_inputs(
    args: argparse.Namespace,
    sweep_names: set[str],
) -> tuple[Path, Path, bool]:
    exe_path = resolve_repo_path(args.exe)
    output_root = resolve_repo_path(args.output_root)
    if not exe_path.exists():
        raise RuntimeError(f"CLI executable not found: {exe_path}")
    if not exe_path.is_file():
        raise RuntimeError(f"CLI executable path is not a file: {exe_path}")
    if args.repeats <= 0:
        raise RuntimeError("--repeats must be greater than zero")
    if args.only_sweep and args.only_sweep not in sweep_names:
        allowed = ", ".join(sorted(sweep_names))
        raise RuntimeError(f"--only-sweep must be one of: {allowed}")
    overwrite_existing = bool(DEFAULT_OPTIONS["overwrite_existing"])
    if args.overwrite_existing == "true":
        overwrite_existing = True
    elif args.overwrite_existing == "false":
        overwrite_existing = False
    return exe_path, output_root, overwrite_existing


def main() -> int:
    args = parse_args()
    sweeps = build_sweeps()
    sweeps.extend(
        [
            SweepDefinition(
                name="flush_threshold_bytes",
                cases=[
                    CaseDefinition("flush_4MiB", {"flush_threshold_bytes": 4 * MIB}, "4 MiB", 1),
                    CaseDefinition("flush_16MiB", {"flush_threshold_bytes": 16 * MIB}, "16 MiB", 2),
                    CaseDefinition("flush_64MiB", {"flush_threshold_bytes": 64 * MIB}, "64 MiB", 3),
                ],
            ),
            SweepDefinition(
                name="flush_interval_ms",
                cases=[
                    CaseDefinition("interval_500ms", {"flush_interval_ms": 500}, "500 ms", 1),
                    CaseDefinition("interval_2000ms", {"flush_interval_ms": 2000}, "2000 ms", 2),
                    CaseDefinition("interval_5000ms", {"flush_interval_ms": 5000}, "5000 ms", 3),
                ],
            ),
            SweepDefinition(
                name="max_gap_bytes",
                cases=[
                    CaseDefinition("gap_4MiB", {"max_gap_bytes": 4 * MIB}, "4 MiB", 1),
                    CaseDefinition("gap_16MiB", {"max_gap_bytes": 16 * MIB}, "16 MiB", 2),
                    CaseDefinition("gap_32MiB", {"max_gap_bytes": 32 * MIB}, "32 MiB", 3),
                    CaseDefinition("gap_64MiB", {"max_gap_bytes": 64 * MIB}, "64 MiB", 4),
                ],
            ),
        ]
    )
    sweep_names = {sweep.name for sweep in sweeps}
    try:
        exe_path, output_root, overwrite_existing = validate_inputs(args, sweep_names)
        head_metadata = perform_head_request(args.url)
    except RuntimeError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    log_info(f"CLI: {exe_path}")
    log_info(
        "HEAD ok: "
        f"status={head_metadata['http_status']} "
        f"size={head_metadata['content_length']} "
        f"accept_ranges={head_metadata['accept_ranges']}"
    )

    label = args.label or "benchmark"
    run_root = make_run_root(output_root, label)
    selected_sweeps = [
        sweep for sweep in sweeps
        if args.only_sweep is None or sweep.name == args.only_sweep
    ]

    baseline_case = CaseDefinition("baseline_default", {}, "default", 1)
    executed_sweeps = ["baseline"] + [sweep.name for sweep in selected_sweeps]
    benchmark_metadata_path = run_root / "benchmark_metadata.json"
    raw_csv_path = run_root / "raw_runs.csv"
    aggregated_csv_path = run_root / "aggregated_cases.csv"
    report_path = run_root / "report.md"

    benchmark_metadata = {
        "generated_at": datetime.now().isoformat(timespec="seconds"),
        "label": label,
        "scenario": "loopback_external_url",
        "exe_path": str(exe_path),
        "output_root": str(run_root),
        "repeats": args.repeats,
        "keep_downloads": args.keep_downloads,
        "head": head_metadata,
        "host": platform.node(),
        "platform": platform.platform(),
        "python_version": sys.version.replace("\n", " "),
        "executed_sweeps": executed_sweeps,
        "baseline_options": {**DEFAULT_OPTIONS, "overwrite_existing": overwrite_existing},
    }
    write_json(benchmark_metadata_path, benchmark_metadata)

    raw_rows: list[dict[str, Any]] = []
    failure: dict[str, Any] | None = None
    run_sequence = 1

    execution_plan: list[tuple[str, CaseDefinition, dict[str, Any]]] = [
        ("baseline", baseline_case, {}),
    ]
    for sweep in selected_sweeps:
        for case in sweep.cases:
            execution_plan.append((sweep.name, case, case.overrides))

    total_runs = len(execution_plan) * args.repeats
    log_info(
        f"Results dir: {run_root}"
    )
    log_info(
        f"Execution plan: {len(execution_plan)} cases x {args.repeats} repeats = {total_runs} runs"
    )

    for sweep_name, case, overrides in execution_plan:
        for repeat_index in range(1, args.repeats + 1):
            options = dict(DEFAULT_OPTIONS)
            options["overwrite_existing"] = overwrite_existing
            options.update(overrides)
            log_info(
                f"Starting run {run_sequence}/{total_runs}: "
                f"{sweep_name} / {case.name} / repeat {repeat_index} | "
                f"{build_case_option_summary(options)}"
            )
            try:
                row = execute_run(
                    exe_path=exe_path,
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
                )
                raw_rows.append(row)
                log_info(
                    "Finished run "
                    f"{row['run_sequence']}/{total_runs}: "
                    f"status={row['status']} "
                    f"exit={row['exit_code']} "
                    f"duration_ms={row['wall_clock_duration_ms']} "
                    f"avg_net={row['avg_network_speed_mb_s']:.2f}MB/s "
                    f"avg_disk={row['avg_disk_speed_mb_s']:.2f}MB/s "
                    f"memory={row['max_memory_bytes']} "
                    f"pauses={int(total_pause_count(row))}"
                )
                run_sequence += 1
                if row["exit_code"] != 0 or row["status"] != "success":
                    failure = {
                        "sweep_name": sweep_name,
                        "case_name": case.name,
                        "repeat_index": repeat_index,
                        "reason": row.get("error") or "CLI returned failure status",
                        "exit_code": row["exit_code"],
                        "summary_path": row["summary_path"],
                        "stdout_path": row["stdout_path"],
                        "stderr_path": row["stderr_path"],
                    }
                    break
            except Exception as exc:
                log_info(
                    f"Run failed before completion: {sweep_name} / {case.name} / repeat {repeat_index} | {exc}"
                )
                run_id = f"{sweep_name}__{case.name}__r{repeat_index:02d}"
                run_dir = run_root / "runs" / run_id
                failure = {
                    "sweep_name": sweep_name,
                    "case_name": case.name,
                    "repeat_index": repeat_index,
                    "reason": str(exc),
                    "exit_code": -1,
                    "summary_path": format_relative(run_dir / f"{run_id}.summary.txt", run_root),
                    "stdout_path": format_relative(run_dir / f"{run_id}.stdout.txt", run_root),
                    "stderr_path": format_relative(run_dir / f"{run_id}.stderr.txt", run_root),
                }
                break
        if failure is not None:
            break

    successful_rows = [
        row for row in raw_rows
        if row["exit_code"] == 0 and row["status"] == "success"
    ]
    aggregated_rows = aggregate_runs(
        successful_rows,
        baseline_key=("baseline", "baseline_default"),
        expected_repeats=args.repeats,
    )

    aggregated_field_order = [
        "scenario",
        "label",
        "sweep_name",
        "case_name",
        "case_order",
        "display_value",
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

    benchmark_metadata.update(
        {
            "completed_runs": len(raw_rows),
            "successful_runs": len(successful_rows),
            "failure": failure,
            "metadata_path": format_relative(benchmark_metadata_path, run_root),
            "raw_csv_path": format_relative(raw_csv_path, run_root),
            "aggregated_csv_path": format_relative(aggregated_csv_path, run_root),
            "report_path": format_relative(report_path, run_root),
        }
    )
    write_json(benchmark_metadata_path, benchmark_metadata)
    render_markdown_report(report_path, benchmark_metadata, aggregated_rows, raw_rows, failure)

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

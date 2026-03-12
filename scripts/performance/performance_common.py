#!/usr/bin/env python3

from __future__ import annotations

import json
import platform
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BENCHMARK_EXE = Path("build/src/Release/AsyncDownload.exe")
DEFAULT_PROFILER_EXE = Path("build/src/Release/AsyncDownload.exe")
DEFAULT_BENCHMARK_OUTPUT_ROOT = Path("build/benchmarks")
DEFAULT_PROFILER_OUTPUT_ROOT = Path("build/profiles")
DEFAULT_REPEATS = 3
DEFAULT_PROFILE_REPEATS = 1
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
    ("handle_data_packet_sample_count", "handle_data_packet_sample_count", "int"),
    ("handle_data_packet_avg_us", "handle_data_packet_avg_us", "float"),
    ("handle_data_packet_max_us", "handle_data_packet_max_us", "float"),
    ("append_bytes_sample_count", "append_bytes_sample_count", "int"),
    ("append_bytes_avg_us", "append_bytes_avg_us", "float"),
    ("append_bytes_max_us", "append_bytes_max_us", "float"),
    ("file_write_sample_count", "file_write_sample_count", "int"),
    ("file_write_avg_us", "file_write_avg_us", "float"),
    ("file_write_max_us", "file_write_max_us", "float"),
    ("file_write_calls_total", "file_write_calls_total", "int"),
    ("staged_write_flush_count", "staged_write_flush_count", "int"),
    ("staged_write_bytes_total", "staged_write_bytes_total", "int"),
]

SUMMARY_REQUIRED_KEYS = {source_key for source_key, _, _ in SUMMARY_SPECS}
SUMMARY_NUMERIC_FIELDS = [
    target_key for _, target_key, field_type in SUMMARY_SPECS
    if field_type in {"int", "float", "speed"}
]
RAW_FIELD_ORDER = [
    "run_id",
    "run_sequence",
    "attempt_index",
    "scenario",
    "label",
    "sweep_name",
    "case_name",
    "display_value",
    "case_purpose",
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
    purpose: str = ""


@dataclass(frozen=True)
class SweepDefinition:
    name: str
    cases: list[CaseDefinition]


@dataclass(frozen=True)
class BenchmarkSuiteDefinition:
    name: str
    description: str
    cases: list[CaseDefinition]
    baseline_overrides: dict[str, Any]


@dataclass(frozen=True)
class ExecutionContext:
    exe_path: Path
    output_root: Path
    overwrite_existing: bool


def make_case(
    name: str,
    display_value: str,
    order: int,
    purpose: str = "",
    **overrides: Any,
) -> CaseDefinition:
    return CaseDefinition(name, dict(overrides), display_value, order, purpose)


def build_sweeps() -> list[SweepDefinition]:
    sweeps = [
        SweepDefinition(
            name="max_connections",
            cases=[
                make_case("conn_1", "1", 1, max_connections=1),
                make_case("conn_2", "2", 2, max_connections=2),
                make_case("conn_4", "4", 3, max_connections=4),
                make_case("conn_8", "8", 4, max_connections=8),
                make_case("conn_16", "16", 5, max_connections=16),
            ],
        ),
        SweepDefinition(
            name="scheduler_window_bytes",
            cases=[
                make_case("window_1MiB", "1 MiB", 1, scheduler_window_bytes=1 * MIB),
                make_case("window_4MiB", "4 MiB", 2, scheduler_window_bytes=4 * MIB),
                make_case("window_16MiB", "16 MiB", 3, scheduler_window_bytes=16 * MIB),
                make_case("window_64MiB", "64 MiB", 4, scheduler_window_bytes=64 * MIB),
            ],
        ),
        SweepDefinition(
            name="queue_capacity_packets",
            cases=[
                make_case("queue_256", "256", 1, queue_capacity_packets=256),
                make_case("queue_1024", "1024", 2, queue_capacity_packets=1024),
                make_case("queue_4096", "4096", 3, queue_capacity_packets=4096),
                make_case("queue_16384", "16384", 4, queue_capacity_packets=16384),
            ],
        ),
        SweepDefinition(
            name="backpressure_pair",
            cases=[
                make_case(
                    "bp_64MiB_32MiB",
                    "64/32 MiB",
                    1,
                    backpressure_high_bytes=64 * MIB,
                    backpressure_low_bytes=32 * MIB,
                ),
                make_case(
                    "bp_128MiB_64MiB",
                    "128/64 MiB",
                    2,
                    backpressure_high_bytes=128 * MIB,
                    backpressure_low_bytes=64 * MIB,
                ),
                make_case(
                    "bp_256MiB_128MiB",
                    "256/128 MiB",
                    3,
                    backpressure_high_bytes=256 * MIB,
                    backpressure_low_bytes=128 * MIB,
                ),
                make_case(
                    "bp_512MiB_256MiB",
                    "512/256 MiB",
                    4,
                    backpressure_high_bytes=512 * MIB,
                    backpressure_low_bytes=256 * MIB,
                ),
            ],
        ),
        SweepDefinition(
            name="flush_threshold_bytes",
            cases=[
                make_case("flush_4MiB", "4 MiB", 1, flush_threshold_bytes=4 * MIB),
                make_case("flush_16MiB", "16 MiB", 2, flush_threshold_bytes=16 * MIB),
                make_case("flush_64MiB", "64 MiB", 3, flush_threshold_bytes=64 * MIB),
            ],
        ),
        SweepDefinition(
            name="flush_interval_ms",
            cases=[
                make_case("interval_500ms", "500 ms", 1, flush_interval_ms=500),
                make_case("interval_2000ms", "2000 ms", 2, flush_interval_ms=2000),
                make_case("interval_5000ms", "5000 ms", 3, flush_interval_ms=5000),
            ],
        ),
        SweepDefinition(
            name="max_gap_bytes",
            cases=[
                make_case("gap_4MiB", "4 MiB", 1, max_gap_bytes=4 * MIB),
                make_case("gap_16MiB", "16 MiB", 2, max_gap_bytes=16 * MIB),
                make_case("gap_32MiB", "32 MiB", 3, max_gap_bytes=32 * MIB),
                make_case("gap_64MiB", "64 MiB", 4, max_gap_bytes=64 * MIB),
            ],
        ),
    ]
    sweeps.extend(
        [
            SweepDefinition(
                name="max_connections_x_scheduler_window_bytes",
                cases=[
                    make_case("conn_4__window_1MiB", "4 / 1 MiB", 1, max_connections=4, scheduler_window_bytes=1 * MIB),
                    make_case("conn_4__window_4MiB", "4 / 4 MiB", 2, max_connections=4, scheduler_window_bytes=4 * MIB),
                    make_case("conn_4__window_16MiB", "4 / 16 MiB", 3, max_connections=4, scheduler_window_bytes=16 * MIB),
                    make_case("conn_8__window_1MiB", "8 / 1 MiB", 4, max_connections=8, scheduler_window_bytes=1 * MIB),
                    make_case("conn_8__window_4MiB", "8 / 4 MiB", 5, max_connections=8, scheduler_window_bytes=4 * MIB),
                    make_case("conn_8__window_16MiB", "8 / 16 MiB", 6, max_connections=8, scheduler_window_bytes=16 * MIB),
                    make_case("conn_16__window_1MiB", "16 / 1 MiB", 7, max_connections=16, scheduler_window_bytes=1 * MIB),
                    make_case("conn_16__window_4MiB", "16 / 4 MiB", 8, max_connections=16, scheduler_window_bytes=4 * MIB),
                    make_case("conn_16__window_16MiB", "16 / 16 MiB", 9, max_connections=16, scheduler_window_bytes=16 * MIB),
                ],
            ),
            SweepDefinition(
                name="max_connections_x_queue_capacity_packets",
                cases=[
                    make_case("conn_4__queue_256", "4 / 256", 1, max_connections=4, queue_capacity_packets=256),
                    make_case("conn_4__queue_1024", "4 / 1024", 2, max_connections=4, queue_capacity_packets=1024),
                    make_case("conn_4__queue_16384", "4 / 16384", 3, max_connections=4, queue_capacity_packets=16384),
                    make_case("conn_8__queue_256", "8 / 256", 4, max_connections=8, queue_capacity_packets=256),
                    make_case("conn_8__queue_1024", "8 / 1024", 5, max_connections=8, queue_capacity_packets=1024),
                    make_case("conn_8__queue_16384", "8 / 16384", 6, max_connections=8, queue_capacity_packets=16384),
                    make_case("conn_16__queue_256", "16 / 256", 7, max_connections=16, queue_capacity_packets=256),
                    make_case("conn_16__queue_1024", "16 / 1024", 8, max_connections=16, queue_capacity_packets=1024),
                    make_case("conn_16__queue_16384", "16 / 16384", 9, max_connections=16, queue_capacity_packets=16384),
                ],
            ),
            SweepDefinition(
                name="scheduler_window_bytes_x_max_gap_bytes",
                cases=[
                    make_case("window_4MiB__gap_16MiB", "4 MiB / 16 MiB", 1, scheduler_window_bytes=4 * MIB, max_gap_bytes=16 * MIB),
                    make_case("window_4MiB__gap_32MiB", "4 MiB / 32 MiB", 2, scheduler_window_bytes=4 * MIB, max_gap_bytes=32 * MIB),
                    make_case("window_4MiB__gap_64MiB", "4 MiB / 64 MiB", 3, scheduler_window_bytes=4 * MIB, max_gap_bytes=64 * MIB),
                    make_case("window_16MiB__gap_16MiB", "16 MiB / 16 MiB", 4, scheduler_window_bytes=16 * MIB, max_gap_bytes=16 * MIB),
                    make_case("window_16MiB__gap_32MiB", "16 MiB / 32 MiB", 5, scheduler_window_bytes=16 * MIB, max_gap_bytes=32 * MIB),
                    make_case("window_16MiB__gap_64MiB", "16 MiB / 64 MiB", 6, scheduler_window_bytes=16 * MIB, max_gap_bytes=64 * MIB),
                ],
            ),
            SweepDefinition(
                name="queue_capacity_packets_x_backpressure_pair",
                cases=[
                    make_case("queue_1024__bp_64MiB_32MiB", "1024 / 64-32 MiB", 1, queue_capacity_packets=1024, backpressure_high_bytes=64 * MIB, backpressure_low_bytes=32 * MIB),
                    make_case("queue_1024__bp_256MiB_128MiB", "1024 / 256-128 MiB", 2, queue_capacity_packets=1024, backpressure_high_bytes=256 * MIB, backpressure_low_bytes=128 * MIB),
                    make_case("queue_4096__bp_64MiB_32MiB", "4096 / 64-32 MiB", 3, queue_capacity_packets=4096, backpressure_high_bytes=64 * MIB, backpressure_low_bytes=32 * MIB),
                    make_case("queue_4096__bp_256MiB_128MiB", "4096 / 256-128 MiB", 4, queue_capacity_packets=4096, backpressure_high_bytes=256 * MIB, backpressure_low_bytes=128 * MIB),
                    make_case("queue_16384__bp_64MiB_32MiB", "16384 / 64-32 MiB", 5, queue_capacity_packets=16384, backpressure_high_bytes=64 * MIB, backpressure_low_bytes=32 * MIB),
                    make_case("queue_16384__bp_256MiB_128MiB", "16384 / 256-128 MiB", 6, queue_capacity_packets=16384, backpressure_high_bytes=256 * MIB, backpressure_low_bytes=128 * MIB),
                ],
            ),
        ]
    )
    return sweeps


def build_benchmark_suites() -> dict[str, BenchmarkSuiteDefinition]:
    return {
        "regression": BenchmarkSuiteDefinition(
            name="regression",
            description=(
                "Fixed before/after regression suite covering the built-in baseline plus current "
                "throughput candidate, balanced profile, memory-constrained profile, and known "
                "sensitive stress paths."
            ),
            cases=[
                make_case(
                    "throughput_candidate",
                    "conn=16 window=4MiB",
                    1,
                    purpose="Current strongest throughput candidate from single-parameter retests.",
                    max_connections=16,
                    scheduler_window_bytes=4 * MIB,
                ),
                make_case(
                    "balanced_candidate",
                    "conn=16 window=4MiB queue=1024 gap=32MiB",
                    2,
                    purpose="Near-top throughput with tighter queue depth and moderate gap budget.",
                    max_connections=16,
                    scheduler_window_bytes=4 * MIB,
                    queue_capacity_packets=1024,
                    max_gap_bytes=32 * MIB,
                ),
                make_case(
                    "deep_buffer_candidate",
                    "conn=16 window=4MiB queue=16384 gap=64MiB",
                    3,
                    purpose="High-buffer profile to expose inflight growth and persistence pressure.",
                    max_connections=16,
                    scheduler_window_bytes=4 * MIB,
                    queue_capacity_packets=16384,
                    max_gap_bytes=64 * MIB,
                ),
                make_case(
                    "memory_guard",
                    "conn=4 queue=256 bp=64/32MiB",
                    4,
                    purpose="Constrained queue and backpressure profile for memory-sensitive behavior.",
                    max_connections=4,
                    queue_capacity_packets=256,
                    backpressure_high_bytes=64 * MIB,
                    backpressure_low_bytes=32 * MIB,
                ),
                make_case(
                    "scheduler_stress",
                    "conn=8 window=16MiB",
                    5,
                    purpose="Previously unstable/underperforming scheduler region used as a regression canary.",
                    max_connections=8,
                    scheduler_window_bytes=16 * MIB,
                ),
                make_case(
                    "queue_backpressure_stress",
                    "queue=16384 bp=64/32MiB",
                    6,
                    purpose="Checks deep queue behavior under tighter backpressure thresholds.",
                    queue_capacity_packets=16384,
                    backpressure_high_bytes=64 * MIB,
                    backpressure_low_bytes=32 * MIB,
                ),
                make_case(
                    "gap_tolerance_probe",
                    "conn=16 window=4MiB gap=64MiB",
                    7,
                    purpose="Tracks whether broader gap tolerance improves throughput or only increases backlog.",
                    max_connections=16,
                    scheduler_window_bytes=4 * MIB,
                    max_gap_bytes=64 * MIB,
                ),
            ],
            baseline_overrides={},
        ),
        "regression_v2": BenchmarkSuiteDefinition(
            name="regression_v2",
            description=(
                "Post-aggregation regression suite with queue depths re-normalized so the queue "
                "byte budget stays close to the original regression suite."
            ),
            cases=[
                make_case(
                    "throughput_candidate",
                    "conn=16 window=4MiB queue=1024",
                    1,
                    purpose="High-throughput candidate after aggregation, with queue depth scaled to the old byte budget.",
                    max_connections=16,
                    scheduler_window_bytes=4 * MIB,
                    queue_capacity_packets=1024,
                ),
                make_case(
                    "balanced_candidate",
                    "conn=16 window=4MiB queue=256 gap=32MiB",
                    2,
                    purpose="Balanced post-aggregation profile with tighter queue depth and moderate gap budget.",
                    max_connections=16,
                    scheduler_window_bytes=4 * MIB,
                    queue_capacity_packets=256,
                    max_gap_bytes=32 * MIB,
                ),
                make_case(
                    "deep_buffer_candidate",
                    "conn=16 window=4MiB queue=4096 gap=64MiB",
                    3,
                    purpose="Deep-buffer post-aggregation profile that preserves a large queue byte budget without inflating it by 4x.",
                    max_connections=16,
                    scheduler_window_bytes=4 * MIB,
                    queue_capacity_packets=4096,
                    max_gap_bytes=64 * MIB,
                ),
                make_case(
                    "memory_guard",
                    "conn=4 queue=64 bp=64/32MiB",
                    4,
                    purpose="Memory-sensitive post-aggregation profile with queue depth scaled down to preserve the old byte ceiling.",
                    max_connections=4,
                    queue_capacity_packets=64,
                    backpressure_high_bytes=64 * MIB,
                    backpressure_low_bytes=32 * MIB,
                ),
                make_case(
                    "scheduler_stress",
                    "conn=8 queue=1024 window=16MiB",
                    5,
                    purpose="Scheduler stress canary after queue-depth re-normalization.",
                    max_connections=8,
                    queue_capacity_packets=1024,
                    scheduler_window_bytes=16 * MIB,
                ),
                make_case(
                    "queue_backpressure_stress",
                    "queue=4096 bp=64/32MiB",
                    6,
                    purpose="Deep-queue and tight-backpressure stress case re-normalized for 64KiB packets.",
                    queue_capacity_packets=4096,
                    backpressure_high_bytes=64 * MIB,
                    backpressure_low_bytes=32 * MIB,
                ),
                make_case(
                    "gap_tolerance_probe",
                    "conn=16 queue=1024 window=4MiB gap=64MiB",
                    7,
                    purpose="Gap-tolerance probe after queue-depth re-normalization.",
                    max_connections=16,
                    queue_capacity_packets=1024,
                    scheduler_window_bytes=4 * MIB,
                    max_gap_bytes=64 * MIB,
                ),
            ],
            baseline_overrides={
                "queue_capacity_packets": 1024,
            },
        ),
    }


def default_baseline_case(overrides: dict[str, Any] | None = None) -> CaseDefinition:
    return CaseDefinition("baseline_default", {} if overrides is None else dict(overrides), "default", 1)


def resolve_repo_path(path_text: str) -> Path:
    path = Path(path_text)
    if path.is_absolute():
        return path
    return REPO_ROOT / path


def log_info(message: str) -> None:
    timestamp = datetime.now().strftime("%H:%M:%S")
    print(f"[{timestamp}] {message}", flush=True)


def parse_case_list(case_list_text: str | None) -> list[str]:
    if case_list_text is None:
        return []
    parsed = [item.strip() for item in case_list_text.split(",")]
    parsed = [item for item in parsed if item]
    if not parsed:
        raise RuntimeError("--case-list must contain at least one case name")
    unique_case_names: list[str] = []
    seen: set[str] = set()
    for case_name in parsed:
        if case_name in seen:
            continue
        seen.add(case_name)
        unique_case_names.append(case_name)
    return unique_case_names


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
        return float("inf")
    return ((current - previous) / previous) * 100.0


def write_csv(path: Path, rows: list[dict[str, Any]], field_order: list[str]) -> None:
    import csv

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=field_order, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def aggregate_numeric_values(values: list[float]) -> tuple[float, float, float, float]:
    import statistics

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


def filter_execution_plan(
    execution_plan: list[tuple[str, CaseDefinition, dict[str, Any]]],
    requested_case_names: list[str],
) -> list[tuple[str, CaseDefinition, dict[str, Any]]]:
    if not requested_case_names:
        return execution_plan
    available_case_names = [case.name for _, case, _ in execution_plan]
    duplicate_case_names = {
        case_name for case_name in available_case_names
        if available_case_names.count(case_name) > 1
    }
    if duplicate_case_names:
        duplicates = ", ".join(sorted(duplicate_case_names))
        raise RuntimeError(f"execution plan contains duplicate case names: {duplicates}")
    available_case_set = set(available_case_names)
    missing = [case_name for case_name in requested_case_names if case_name not in available_case_set]
    if missing:
        missing_text = ", ".join(missing)
        allowed = ", ".join(sorted(available_case_set))
        raise RuntimeError(f"--case-list contains unknown cases: {missing_text}. Allowed: {allowed}")
    requested_case_set = set(requested_case_names)
    return [
        entry for entry in execution_plan
        if entry[1].name in requested_case_set
    ]


def validate_execution_inputs(
    *,
    exe_text: str,
    output_root_text: str,
    repeats: int,
    only_sweep: str | None,
    benchmark_suite_name: str | None,
    overwrite_existing_text: str | None,
    sweep_names: set[str],
    benchmark_suite_names: set[str],
) -> ExecutionContext:
    exe_path = resolve_repo_path(exe_text)
    output_root = resolve_repo_path(output_root_text)
    if not exe_path.exists():
        raise RuntimeError(f"CLI executable not found: {exe_path}")
    if not exe_path.is_file():
        raise RuntimeError(f"CLI executable path is not a file: {exe_path}")
    if repeats <= 0:
        raise RuntimeError("--repeats must be greater than zero")
    if only_sweep and benchmark_suite_name:
        raise RuntimeError("--only-sweep and --benchmark-suite cannot be used together")
    if only_sweep and only_sweep not in sweep_names:
        allowed = ", ".join(sorted(sweep_names))
        raise RuntimeError(f"--only-sweep must be one of: {allowed}")
    if benchmark_suite_name and benchmark_suite_name not in benchmark_suite_names:
        allowed = ", ".join(sorted(benchmark_suite_names))
        raise RuntimeError(f"--benchmark-suite must be one of: {allowed}")
    overwrite_existing = bool(DEFAULT_OPTIONS["overwrite_existing"])
    if overwrite_existing_text == "true":
        overwrite_existing = True
    elif overwrite_existing_text == "false":
        overwrite_existing = False
    return ExecutionContext(exe_path=exe_path, output_root=output_root, overwrite_existing=overwrite_existing)


def build_execution_plan(
    *,
    sweeps: list[SweepDefinition],
    benchmark_suites: dict[str, BenchmarkSuiteDefinition],
    only_sweep: str | None,
    benchmark_suite_name: str | None,
    requested_case_names: list[str],
) -> tuple[str, BenchmarkSuiteDefinition | None, list[tuple[str, CaseDefinition, dict[str, Any]]], list[str]]:
    execution_mode = "all_sweeps"
    benchmark_suite = None
    selected_sweeps: list[SweepDefinition] = []
    if benchmark_suite_name is not None:
        execution_mode = "benchmark_suite"
        benchmark_suite = benchmark_suites[benchmark_suite_name]
    else:
        if only_sweep is not None:
            execution_mode = "single_sweep"
        selected_sweeps = [
            sweep for sweep in sweeps
            if only_sweep is None or sweep.name == only_sweep
        ]

    baseline_overrides = {} if benchmark_suite is None else benchmark_suite.baseline_overrides
    execution_plan: list[tuple[str, CaseDefinition, dict[str, Any]]] = [
        ("baseline", default_baseline_case(baseline_overrides), baseline_overrides),
    ]
    if benchmark_suite is not None:
        for case in benchmark_suite.cases:
            execution_plan.append((benchmark_suite.name, case, case.overrides))
    else:
        for sweep in selected_sweeps:
            for case in sweep.cases:
                execution_plan.append((sweep.name, case, case.overrides))

    execution_plan = filter_execution_plan(execution_plan, requested_case_names)
    if not execution_plan:
        raise RuntimeError("execution plan is empty after applying --case-list")

    executed_sweeps: list[str] = []
    for sweep_name, _, _ in execution_plan:
        if sweep_name not in executed_sweeps:
            executed_sweeps.append(sweep_name)
    return execution_mode, benchmark_suite, execution_plan, executed_sweeps


def default_metadata(
    *,
    label: str,
    scenario: str,
    exe_path: Path,
    run_root: Path,
    repeats: int,
    keep_downloads: bool,
    head_metadata: dict[str, Any],
    execution_mode: str,
    benchmark_suite: BenchmarkSuiteDefinition | None,
    requested_case_names: list[str],
    executed_sweeps: list[str],
    overwrite_existing: bool,
) -> dict[str, Any]:
    baseline_options = {**DEFAULT_OPTIONS, "overwrite_existing": overwrite_existing}
    if benchmark_suite is not None:
        baseline_options.update(benchmark_suite.baseline_overrides)
    return {
        "generated_at": datetime.now().isoformat(timespec="seconds"),
        "label": label,
        "scenario": scenario,
        "exe_path": str(exe_path),
        "output_root": str(run_root),
        "repeats": repeats,
        "keep_downloads": keep_downloads,
        "head": head_metadata,
        "host": platform.node(),
        "platform": platform.platform(),
        "python_version": sys.version.replace("\n", " "),
        "execution_mode": execution_mode,
        "benchmark_suite_name": benchmark_suite.name if benchmark_suite is not None else "",
        "benchmark_suite_description": benchmark_suite.description if benchmark_suite is not None else "",
        "selected_case_names": requested_case_names,
        "executed_sweeps": executed_sweeps,
        "baseline_options": baseline_options,
    }


def execute_download_run(
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
    attempt_index: int = 1,
    keep_downloads: bool,
    extra_run_metadata: dict[str, Any] | None = None,
    run_dir: Path | None = None,
) -> dict[str, Any]:
    run_id = f"{sweep_name}__{case.name}__r{repeat_index:02d}"
    if run_dir is None:
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
        "attempt_index": attempt_index,
        "scenario": scenario,
        "label": label,
        "sweep_name": sweep_name,
        "case_name": case.name,
        "display_value": case.display_value,
        "case_purpose": case.purpose,
        "case_order": case.order,
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

    metadata_payload = {
        "run_id": run_id,
        "run_sequence": run_sequence,
        "attempt_index": attempt_index,
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
    }
    if extra_run_metadata:
        metadata_payload["extra"] = extra_run_metadata
    write_json(run_metadata_path, metadata_payload)

    if not keep_downloads:
        cleanup_download_artifacts(output_path)

    return row


def format_pct(value: float) -> str:
    if value == float("inf"):
        return "inf"
    if value != value:
        return "n/a"
    return f"{value:.2f}%"


def format_float(value: float, digits: int = 2) -> str:
    if value != value:
        return "n/a"
    return f"{value:.{digits}f}"

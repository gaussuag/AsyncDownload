#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime
from pathlib import Path

VALID_TYPES = {"created", "modified", "deleted"}
VALID_STATUS = {"success", "partial", "failed"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Append one structured history record under .agents/history/{thread_key}/"
    )
    parser.add_argument("--thread-key", required=True)
    parser.add_argument("--purpose", required=True)
    parser.add_argument("--request-snapshot", required=True)
    parser.add_argument("--summary", required=True)
    parser.add_argument("--verification", required=True)
    parser.add_argument("--next-step", required=True)
    parser.add_argument("--status", required=True, choices=sorted(VALID_STATUS))
    parser.add_argument("--source", action="append", default=[])
    parser.add_argument("--change", action="append", default=[])
    parser.add_argument("--base-dir", default=".")
    return parser.parse_args()


def normalize_path(value: str, base_dir: Path) -> str:
    path = Path(value)
    if path.is_absolute():
        try:
            return path.relative_to(base_dir).as_posix()
        except ValueError:
            return path.as_posix()
    return path.as_posix()


def dedupe_preserve_order(values: list[str]) -> list[str]:
    result: list[str] = []
    seen: set[str] = set()
    for value in values:
        if value in seen:
            continue
        seen.add(value)
        result.append(value)
    return result


def parse_sources(raw_sources: list[str], base_dir: Path) -> list[str]:
    normalized = [normalize_path(item.strip(), base_dir) for item in raw_sources if item.strip()]
    return dedupe_preserve_order(normalized)


def parse_changes(raw_changes: list[str], base_dir: Path) -> list[dict[str, str]]:
    changes: list[dict[str, str]] = []
    seen: set[tuple[str, str]] = set()
    for raw in raw_changes:
        if ":" not in raw:
            raise ValueError(f"Invalid --change value '{raw}'. Expected path:type.")
        file_name, change_type = raw.rsplit(":", 1)
        file_name = file_name.strip()
        change_type = change_type.strip()
        if change_type not in VALID_TYPES:
            raise ValueError(
                f"Invalid change type '{change_type}'. Expected one of: "
                + ", ".join(sorted(VALID_TYPES))
            )
        if not file_name:
            raise ValueError("Change path cannot be empty.")
        normalized_file = normalize_path(file_name, base_dir)
        key = (normalized_file, change_type)
        if key in seen:
            continue
        seen.add(key)
        changes.append({"file": normalized_file, "type": change_type})
    return changes


def load_archive(index_path: Path, legacy_path: Path, thread_key: str) -> tuple[dict, bool]:
    source_path = index_path if index_path.exists() else legacy_path
    if not source_path.exists():
        return {"thread_key": thread_key, "records": {}}, False

    try:
        with source_path.open("r", encoding="utf-8-sig") as handle:
            payload = json.load(handle)
    except json.JSONDecodeError as exc:
        raise ValueError(f"Existing archive is not valid JSON: {exc}") from exc

    if not isinstance(payload, dict):
        raise ValueError("Existing archive root must be a JSON object.")

    migrated = False
    if "thread_key" in payload or "records" in payload:
        if payload.get("thread_key") not in (None, thread_key):
            raise ValueError("Existing archive thread_key does not match the requested thread_key.")
        records = payload.get("records")
        if not isinstance(records, dict):
            raise ValueError("Existing archive 'records' must be a JSON object.")
        return {"thread_key": thread_key, "records": records}, source_path == legacy_path

    for key, value in payload.items():
        if not isinstance(key, str) or not isinstance(value, list):
            raise ValueError("Legacy archive must map date keys to JSON arrays.")
    migrated = source_path == legacy_path
    return {"thread_key": thread_key, "records": payload}, migrated


def build_session_markdown(
    thread_key: str,
    timestamp: str,
    status: str,
    request_snapshot: str,
    sources: list[str],
    summary: str,
    changes: list[dict[str, str]],
    verification: str,
    next_step: str,
) -> str:
    lines = [
        "# Session Archive",
        "",
        f"thread_key: {thread_key}",
        f"time: {timestamp}",
        f"status: {status}",
        "",
        "## User Request",
        request_snapshot,
        "",
        "## Context Used",
    ]
    if sources:
        lines.extend([f"- {source}" for source in sources])
    else:
        lines.append("- None recorded")
    lines.extend(
        [
            "",
            "## Work Summary",
            summary,
            "",
            "## Files Changed",
        ]
    )
    if changes:
        lines.extend([f"- {item['file']} ({item['type']})" for item in changes])
    else:
        lines.append("- No file changes recorded")
    lines.extend(
        [
            "",
            "## Verification",
            verification,
            "",
            "## Next Step",
            next_step,
            "",
        ]
    )
    return "\n".join(lines)


def unique_session_path(sessions_dir: Path, stem: str) -> Path:
    candidate = sessions_dir / f"{stem}.md"
    if not candidate.exists():
        return candidate
    suffix = 1
    while True:
        candidate = sessions_dir / f"{stem}_{suffix:02d}.md"
        if not candidate.exists():
            return candidate
        suffix += 1


def main() -> int:
    args = parse_args()
    base_dir = Path(args.base_dir).resolve()
    history_root = base_dir / ".agents" / "history"
    thread_dir = history_root / args.thread_key
    index_path = thread_dir / "index.json"
    sessions_dir = thread_dir / "sessions"
    legacy_path = history_root / f"{args.thread_key}.json"

    try:
        sources = parse_sources(args.source, base_dir)
        changes = parse_changes(args.change, base_dir)
        now = datetime.now()
        date_key = now.strftime("%Y-%m-%d")
        time_value = now.strftime("%H:%M:%S")
        timestamp = now.strftime("%Y-%m-%d %H:%M:%S")
        session_stem = now.strftime("%Y-%m-%d_%H-%M-%S")
        payload, migrated_from_legacy = load_archive(index_path, legacy_path, args.thread_key)
        records = payload.get("records")
        if not isinstance(records, dict):
            raise ValueError("Archive 'records' must be a JSON object.")

        thread_dir.mkdir(parents=True, exist_ok=True)
        sessions_dir.mkdir(parents=True, exist_ok=True)
        session_path = unique_session_path(sessions_dir, session_stem)
        session_relative = normalize_path(str(session_path), base_dir)
        session_markdown = build_session_markdown(
            args.thread_key,
            timestamp,
            args.status,
            args.request_snapshot,
            sources,
            args.summary,
            changes,
            args.verification,
            args.next_step,
        )

        with session_path.open("w", encoding="utf-8") as handle:
            handle.write(session_markdown)

        day_records = records.setdefault(date_key, [])
        if not isinstance(day_records, list):
            raise ValueError(f"Archive date key '{date_key}' must contain a JSON array.")

        record = {
            "time": time_value,
            "purpose": args.purpose,
            "request_snapshot": args.request_snapshot,
            "summary": args.summary,
            "sources": sources,
            "changes": changes,
            "verification": args.verification,
            "next_step": args.next_step,
            "session_archive": session_relative,
            "status": args.status,
        }
        day_records.append(record)

        with index_path.open("w", encoding="utf-8") as handle:
            json.dump(payload, handle, ensure_ascii=False, indent=2)
            handle.write("\n")

        if migrated_from_legacy and legacy_path.exists():
            legacy_path.unlink()

        result = {
            "index_file": normalize_path(str(index_path), base_dir),
            "session_archive": session_relative,
            "date": date_key,
            "time": time_value,
            "purpose": args.purpose,
            "status": args.status,
            "changes": [item["file"] for item in changes],
        }
        json.dump(result, sys.stdout, ensure_ascii=False)
        sys.stdout.write("\n")
        return 0
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"Failed to write archive: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

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
        description="Append one structured history record to .agents/history/{thread_key}.json"
    )
    parser.add_argument("--thread-key", required=True)
    parser.add_argument("--purpose", required=True)
    parser.add_argument("--reasoning", required=True)
    parser.add_argument("--verification", required=True)
    parser.add_argument("--status", required=True, choices=sorted(VALID_STATUS))
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


def parse_changes(raw_changes: list[str], base_dir: Path) -> list[dict[str, str]]:
    changes: list[dict[str, str]] = []
    for raw in raw_changes:
        if ":" not in raw:
            raise ValueError(f"Invalid --change value '{raw}'. Expected path:type.")
        file_name, change_type = raw.rsplit(":", 1)
        change_type = change_type.strip()
        if change_type not in VALID_TYPES:
            raise ValueError(
                f"Invalid change type '{change_type}'. Expected one of: "
                + ", ".join(sorted(VALID_TYPES))
            )
        file_name = file_name.strip()
        if not file_name:
            raise ValueError("Change path cannot be empty.")
        changes.append(
            {
                "file": normalize_path(file_name, base_dir),
                "type": change_type,
            }
        )
    return changes


def load_archive(archive_path: Path) -> dict:
    if not archive_path.exists():
        return {}
    try:
        with archive_path.open("r", encoding="utf-8") as handle:
            payload = json.load(handle)
    except json.JSONDecodeError as exc:
        raise ValueError(f"Existing archive is not valid JSON: {exc}") from exc
    if not isinstance(payload, dict):
        raise ValueError("Existing archive root must be a JSON object.")
    return payload


def main() -> int:
    args = parse_args()
    base_dir = Path(args.base_dir).resolve()
    archive_dir = base_dir / ".agents" / "history"
    archive_path = archive_dir / f"{args.thread_key}.json"

    try:
        changes = parse_changes(args.change, base_dir)
        now = datetime.now()
        date_key = now.strftime("%Y-%m-%d")
        time_value = now.strftime("%H:%M:%S")
        payload = load_archive(archive_path)
        day_records = payload.setdefault(date_key, [])
        if not isinstance(day_records, list):
            raise ValueError(f"Archive date key '{date_key}' must contain a JSON array.")

        record = {
            "time": time_value,
            "purpose": args.purpose,
            "reasoning": args.reasoning,
            "changes": changes,
            "verification": args.verification,
            "status": args.status,
        }
        day_records.append(record)

        archive_dir.mkdir(parents=True, exist_ok=True)
        with archive_path.open("w", encoding="utf-8") as handle:
            json.dump(payload, handle, ensure_ascii=False, indent=2)
            handle.write("\n")

        result = {
            "archive_file": normalize_path(str(archive_path), base_dir),
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

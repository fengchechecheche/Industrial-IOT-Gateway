from __future__ import annotations

import datetime as dt
import hashlib
import json
import os
import pathlib
import platform
import tempfile
import time
from typing import Any

SCHEMA_VERSION = "1.0.0"


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def write_json_atomic(path: pathlib.Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    def normalized(item: Any) -> Any:
        if isinstance(item, float) and (item != item or item in {float("inf"), float("-inf")}):
            return None
        if isinstance(item, dict):
            return {key: normalized(child) for key, child in item.items()}
        if isinstance(item, list):
            return [normalized(child) for child in item]
        return item

    encoded = json.dumps(
        normalized(value), ensure_ascii=False, indent=2, sort_keys=True, allow_nan=False
    ) + "\n"
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        pathlib.Path(temporary).unlink(missing_ok=True)


class RotatingTextWriter:
    def __init__(self, directory: pathlib.Path, stem: str, suffix: str, maximum_bytes: int) -> None:
        self.directory = directory
        self.stem = stem
        self.suffix = suffix
        self.maximum_bytes = maximum_bytes
        self.index = 0
        self.size = 0
        self.stream = None
        self._open_next()

    def _open_next(self) -> None:
        if self.stream is not None:
            self.stream.close()
        self.index += 1
        self.size = 0
        path = self.directory / f"{self.stem}_{self.index:04d}.{self.suffix}"
        self.stream = path.open("w", encoding="utf-8", buffering=1)

    def write(self, text: str) -> None:
        encoded_size = len(text.encode("utf-8"))
        if self.size and self.size + encoded_size > self.maximum_bytes:
            self._open_next()
        assert self.stream is not None
        self.stream.write(text)
        self.size += encoded_size

    def close(self) -> None:
        if self.stream is not None:
            self.stream.close()
            self.stream = None


class SoakEvidence:
    def __init__(
        self,
        output_directory: pathlib.Path,
        *,
        run_id: str,
        profile: dict[str, Any],
        source_revision: str,
        environment_details: dict[str, Any] | None = None,
    ) -> None:
        self.directory = output_directory
        self.run_id = run_id
        self.profile = profile
        self.source_revision = source_revision
        self.started_utc = utc_now()
        self.started_monotonic = time.monotonic()
        self.event_sequence = 0
        self.directory.mkdir(parents=True, exist_ok=False)
        write_json_atomic(self.directory / "profile.json", profile)
        environment = {
            "schema_version": SCHEMA_VERSION,
            "platform": platform.platform(),
            "architecture": platform.machine(),
            "python": platform.python_version(),
            "pid": os.getpid(),
            "temperature": None,
            "temperature_state": "not_available_or_not_required",
        }
        if environment_details:
            environment.update(environment_details)
        write_json_atomic(
            self.directory / "environment.json",
            environment,
        )
        self.manifest = {
            "schema_version": SCHEMA_VERSION,
            "run_id": run_id,
            "profile_id": profile["profile_id"],
            "profile_kind": profile["profile_kind"],
            "source_revision": source_revision,
            "started_at_utc": self.started_utc,
            "ended_at_utc": None,
            "monotonic_duration_seconds": None,
            "status": "RUNNING",
            "worktree_state": "not_checked_by_policy",
        }
        write_json_atomic(self.directory / "manifest.json", self.manifest)
        for name in ("commands.jsonl", "events.jsonl", "resource_samples.jsonl"):
            (self.directory / name).write_text("", encoding="utf-8")
        write_json_atomic(self.directory / "summary.json", {"status": "RUNNING"})
        write_json_atomic(self.directory / "failures.json", [])

    def append_jsonl(self, name: str, value: dict[str, Any]) -> None:
        with (self.directory / name).open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(value, ensure_ascii=False, sort_keys=True) + "\n")

    def event(self, event_type: str, **details: Any) -> str:
        self.event_sequence += 1
        event_id = f"{self.run_id}:{self.event_sequence:06d}"
        value = {
            "schema_version": SCHEMA_VERSION,
            "event_id": event_id,
            "run_id": self.run_id,
            "event_type": event_type,
            "timestamp_utc": utc_now(),
            "monotonic_ms": int((time.monotonic() - self.started_monotonic) * 1000),
            "details": details,
        }
        self.append_jsonl("events.jsonl", value)
        return event_id

    def command(self, role: str, argv: list[str]) -> None:
        self.append_jsonl("commands.jsonl", {"role": role, "argv": argv})

    def finish(self, status: str) -> None:
        self.manifest["ended_at_utc"] = utc_now()
        self.manifest["monotonic_duration_seconds"] = time.monotonic() - self.started_monotonic
        self.manifest["status"] = status
        write_json_atomic(self.directory / "manifest.json", self.manifest)


def write_checksums(directory: pathlib.Path) -> None:
    lines = []
    for path in sorted(directory.rglob("*")):
        if not path.is_file() or path.name == "SHA256SUMS":
            continue
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        lines.append(f"{digest}  {path.relative_to(directory).as_posix()}")
    (directory / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="utf-8")

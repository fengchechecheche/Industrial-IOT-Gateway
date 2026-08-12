from __future__ import annotations

import datetime as dt
import hashlib
import json
import os
import pathlib
import platform
import time
from typing import Any

from .models import ScenarioStatus


SCHEMA_VERSION = "1.0.0"
REQUIRED_EVENT_FIELDS = {
    "schema_version",
    "event_id",
    "run_id",
    "scenario_id",
    "timestamp_utc",
    "monotonic_ms",
    "component",
    "event_type",
    "severity",
    "result",
    "queue_depths",
}


def _utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def _write_json(path: pathlib.Path, value: Any) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def validate_event(event: dict[str, Any]) -> None:
    missing = sorted(REQUIRED_EVENT_FIELDS.difference(event))
    if missing:
        raise ValueError("event is missing required fields: " + ", ".join(missing))
    if event["schema_version"] != SCHEMA_VERSION:
        raise ValueError("unsupported event schema_version")
    if not isinstance(event["monotonic_ms"], int) or event["monotonic_ms"] < 0:
        raise ValueError("monotonic_ms must be a non-negative integer")
    if not isinstance(event["queue_depths"], dict):
        raise ValueError("queue_depths must be an object")


class EvidenceWriter:
    def __init__(
        self,
        output_directory: pathlib.Path,
        *,
        run_id: str,
        source_revision: str,
        exploratory: bool,
        stage: str = "S4",
        task: str = "P3-S4-T03",
    ) -> None:
        self.output_directory = output_directory
        self.run_id = run_id
        self.source_revision = source_revision
        self.exploratory = exploratory
        self.stage = stage
        self.task = task
        self.started_utc = _utc_now()
        self.started_monotonic = time.monotonic()
        self._event_sequence = 0
        self._event_ids: set[str] = set()
        self._scenario_summaries: list[dict[str, Any]] = []
        self._failures: list[dict[str, Any]] = []

    def begin_run(self, config: dict[str, Any]) -> None:
        self.output_directory.mkdir(parents=True, exist_ok=False)
        (self.output_directory / "scenarios").mkdir()
        _write_json(
            self.output_directory / "manifest.json",
            {
                "schema_version": SCHEMA_VERSION,
                "run_id": self.run_id,
                "gate": "G3",
                "stage": self.stage,
                "task": self.task,
                "started_at_utc": self.started_utc,
                "ended_at_utc": None,
                "source_revision": self.source_revision,
                "worktree_state": "not_checked_by_policy",
                "exploratory": self.exploratory,
                "status": "RUNNING",
            },
        )
        _write_json(self.output_directory / "config.yaml", config)
        _write_json(
            self.output_directory / "environment.json",
            {
                "schema_version": SCHEMA_VERSION,
                "platform": platform.platform(),
                "architecture": platform.machine(),
                "python": platform.python_version(),
                "pid": os.getpid(),
            },
        )
        for name in ("commands.txt", "events.jsonl", "run.log"):
            (self.output_directory / name).write_text("", encoding="utf-8")
        _write_json(self.output_directory / "summary.json", {"status": "RUNNING"})
        _write_json(self.output_directory / "failures.json", [])

    def record_command(self, scenario_id: str, command: list[str]) -> None:
        with (self.output_directory / "commands.txt").open("a", encoding="utf-8") as stream:
            stream.write(json.dumps({"scenario_id": scenario_id, "argv": command}, ensure_ascii=False) + "\n")

    def append_log(self, scenario_id: str, stream_name: str, text: str) -> None:
        if not text:
            return
        with (self.output_directory / "run.log").open("a", encoding="utf-8") as stream:
            for line in text.splitlines():
                stream.write(f"[{scenario_id}][{stream_name}] {line}\n")

    def emit_event(
        self,
        scenario_id: str,
        event_type: str,
        *,
        component: str = "runner",
        severity: str = "info",
        result: str = "",
        details: dict[str, Any] | None = None,
    ) -> str:
        self._event_sequence += 1
        event_id = f"{self.run_id}:{scenario_id}:{self._event_sequence:06d}"
        event: dict[str, Any] = {
            "schema_version": SCHEMA_VERSION,
            "event_id": event_id,
            "run_id": self.run_id,
            "scenario_id": scenario_id,
            "timestamp_utc": _utc_now(),
            "monotonic_ms": int((time.monotonic() - self.started_monotonic) * 1000),
            "component": component,
            "event_type": event_type,
            "severity": severity,
            "result": result,
            "queue_depths": {},
        }
        if details:
            event["details"] = details
        validate_event(event)
        if event_id in self._event_ids:
            raise ValueError("duplicate event_id")
        self._event_ids.add(event_id)
        encoded = json.dumps(event, ensure_ascii=False, sort_keys=True) + "\n"
        with (self.output_directory / "events.jsonl").open("a", encoding="utf-8") as stream:
            stream.write(encoded)
        scenario_directory = self.output_directory / "scenarios" / scenario_id
        scenario_directory.mkdir(parents=True, exist_ok=True)
        with (scenario_directory / "events.jsonl").open("a", encoding="utf-8") as stream:
            stream.write(encoded)
        return event_id

    def write_scenario(
        self,
        scenario_id: str,
        config: dict[str, Any],
        oracles: list[dict[str, Any]],
        status: ScenarioStatus,
        *,
        recovery_time_ms: int | None,
        actual_result: str | None = None,
        process: dict[str, Any] | None = None,
        evidence_event_ids: list[str] | None = None,
    ) -> None:
        scenario_directory = self.output_directory / "scenarios" / scenario_id
        scenario_directory.mkdir(parents=True, exist_ok=True)
        if not (scenario_directory / "events.jsonl").exists():
            started = self.emit_event(scenario_id, "scenario_started", result="running")
            completed = self.emit_event(scenario_id, "scenario_completed", result=status.value)
            evidence_event_ids = [started, completed]
        evidence_event_ids = list(evidence_event_ids or [])
        _write_json(scenario_directory / "config.yaml", config)
        summary = {
            "schema_version": SCHEMA_VERSION,
            "run_id": self.run_id,
            "scenario_id": scenario_id,
            "status": status.value,
            "expected_result": config.get("expected"),
            "actual_result": actual_result or status.value,
            "recovery_time_ms": recovery_time_ms,
            "oracles": oracles,
            "evidence_event_ids": evidence_event_ids,
            "process": process,
        }
        _write_json(scenario_directory / "summary.json", summary)
        failures = []
        for index, oracle in enumerate(oracles, start=1):
            if not oracle.get("passed", False):
                failures.append(
                    {
                        "failure_id": f"{scenario_id}-{index:03d}",
                        "scenario_id": scenario_id,
                        "severity": "P1" if status == ScenarioStatus.TIMEOUT else "P2",
                        "oracle_id": oracle.get("oracle_id", "unknown"),
                        "expected": oracle.get("expected"),
                        "actual": oracle.get("actual"),
                        "evidence_event_ids": evidence_event_ids,
                        "status": "OPEN",
                    }
                )
        _write_json(scenario_directory / "failures.json", failures)
        self._scenario_summaries.append(summary)
        self._failures.extend(failures)

    def finish_run(self, status: ScenarioStatus) -> None:
        ended = _utc_now()
        duration_ms = int((time.monotonic() - self.started_monotonic) * 1000)
        _write_json(
            self.output_directory / "summary.json",
            {
                "schema_version": SCHEMA_VERSION,
                "run_id": self.run_id,
                "status": status.value,
                "started_at_utc": self.started_utc,
                "ended_at_utc": ended,
                "monotonic_duration_ms": duration_ms,
                "scenario_count": len(self._scenario_summaries),
                "scenarios": self._scenario_summaries,
                "unclosed_failures": len(self._failures),
            },
        )
        _write_json(self.output_directory / "failures.json", self._failures)
        manifest = json.loads((self.output_directory / "manifest.json").read_text(encoding="utf-8"))
        manifest["ended_at_utc"] = ended
        manifest["status"] = status.value
        _write_json(self.output_directory / "manifest.json", manifest)
        self._write_checksums()

    def _write_checksums(self) -> None:
        lines = []
        for path in sorted(self.output_directory.rglob("*")):
            if not path.is_file() or path.name == "SHA256SUMS":
                continue
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            lines.append(f"{digest}  {path.relative_to(self.output_directory).as_posix()}")
        (self.output_directory / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="utf-8")

    def verify_checksums(self) -> bool:
        checksum_path = self.output_directory / "SHA256SUMS"
        if not checksum_path.is_file():
            return False
        for line in checksum_path.read_text(encoding="utf-8").splitlines():
            digest, relative = line.split("  ", 1)
            path = self.output_directory / relative
            if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                return False
        return True

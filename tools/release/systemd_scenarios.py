from __future__ import annotations

import dataclasses
import hashlib
import json
import pathlib
import re
from datetime import UTC, datetime
from typing import Any

from tools.release.systemd_runner import (
    REQUIRED_G5_SCENARIOS,
    SystemdContractError,
    evaluate_g5,
    validate_unit_contract,
)


FIXED_UNIT = "industrial_iot_gateway.service"
MAX_STOP_MS = 5_000
MAX_MISSING_SERIAL_JOURNAL_BYTES = 65_536
MAX_MISSING_SERIAL_CPU_MS = 1_000


class G5ContractError(ValueError):
    """Raised when a native G5 input or observation violates its frozen contract."""


def validate_native_inputs(unit_name: str, source_revision: str) -> None:
    if unit_name != FIXED_UNIT:
        raise G5ContractError(f"only {FIXED_UNIT!r} may be operated on")
    if re.fullmatch(r"[0-9a-f]{40}", source_revision) is None:
        raise G5ContractError("source revision must be a full lowercase 40-character Git id")


def journal_after_cursor_arguments(unit_name: str, cursor: str) -> list[str]:
    validate_native_inputs(unit_name, "0" * 40)
    if not cursor or "\n" in cursor or "\r" in cursor:
        raise G5ContractError("journal cursor must be a non-empty single line")
    return [
        "journalctl",
        "--no-pager",
        "--output=cat",
        "--unit",
        unit_name,
        "--after-cursor",
        cursor,
    ]


def _is_int(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _equals(observation: dict[str, Any], key: str, expected: object) -> str | None:
    actual = observation.get(key)
    if actual != expected:
        return f"{key}: expected {expected!r}, got {actual!r}"
    return None


def _positive(observation: dict[str, Any], key: str) -> str | None:
    value = observation.get(key)
    if not _is_int(value) or value <= 0:
        return f"{key}: expected positive integer, got {value!r}"
    return None


def _bounded_integer(
    observation: dict[str, Any], key: str, minimum: int, maximum: int
) -> str | None:
    value = observation.get(key)
    if not _is_int(value) or value < minimum or value > maximum:
        return f"{key}: expected integer in [{minimum}, {maximum}], got {value!r}"
    return None


def _evaluate_unit(observation: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    failure = _equals(observation, "verify_rc", 0)
    if failure:
        failures.append(failure)
    properties = observation.get("properties")
    if not isinstance(properties, dict):
        failures.append("properties: expected mapping")
    else:
        try:
            validate_unit_contract({str(key): str(value) for key, value in properties.items()})
        except SystemdContractError as error:
            failures.append(str(error))
    return failures


def _evaluate_normal_start(observation: dict[str, Any]) -> list[str]:
    checks = (
        _equals(observation, "active_state", "active"),
        _positive(observation, "main_pid"),
        _equals(observation, "gateway_ready", True),
        _positive(observation, "requests_succeeded_delta"),
        _positive(observation, "mqtt_publish_successes_delta"),
    )
    return [value for value in checks if value]


def _evaluate_sigterm(observation: dict[str, Any]) -> list[str]:
    checks = (
        _equals(observation, "stop_rc", 0),
        _bounded_integer(observation, "stop_ms", 0, MAX_STOP_MS),
        _equals(observation, "active_state", "inactive"),
        _equals(observation, "main_pid", 0),
        _equals(observation, "stopped", True),
    )
    return [value for value in checks if value]


def _evaluate_sigint(observation: dict[str, Any]) -> list[str]:
    checks = (
        _equals(observation, "exit_code", 0),
        _bounded_integer(observation, "stop_ms", 0, MAX_STOP_MS),
        _equals(observation, "stopped", True),
    )
    return [value for value in checks if value]


def _evaluate_restart(observation: dict[str, Any]) -> list[str]:
    checks = [
        _equals(observation, "active_state", "active"),
        _positive(observation, "old_pid"),
        _positive(observation, "new_pid"),
        _equals(observation, "restart_count_delta", 1),
        _bounded_integer(observation, "restart_ms", 1_500, 5_000),
    ]
    if observation.get("old_pid") == observation.get("new_pid"):
        checks.append("new_pid: restarted process must have a different PID")
    return [value for value in checks if value]


def _evaluate_invalid_config(observation: dict[str, Any]) -> list[str]:
    checks = (
        _equals(observation, "exit_code", 4),
        _equals(observation, "restart_count_delta", 0),
        _equals(observation, "active_state", "failed"),
    )
    return [value for value in checks if value]


def _evaluate_missing_serial(observation: dict[str, Any]) -> list[str]:
    checks = (
        _equals(observation, "active_state", "active"),
        _bounded_integer(observation, "serial_errors_delta", 1, 100),
        _bounded_integer(
            observation, "journal_bytes", 0, MAX_MISSING_SERIAL_JOURNAL_BYTES
        ),
        _bounded_integer(observation, "cpu_time_delta_ms", 0, MAX_MISSING_SERIAL_CPU_MS),
    )
    return [value for value in checks if value]


def _evaluate_broker_unavailable(observation: dict[str, Any]) -> list[str]:
    checks = (
        _equals(observation, "active_state", "active"),
        _positive(observation, "serial_successes_delta"),
        _bounded_integer(observation, "mqtt_failures_delta", 1, 30),
        _equals(observation, "mqtt_reconnected", True),
        _equals(observation, "mqtt_publish_success_after_recovery", True),
    )
    return [value for value in checks if value]


def _evaluate_journal(observation: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    if observation.get("cursor_scoped") is not True:
        failures.append("cursor_scoped: journal evidence must use this run's cursor")
    events = observation.get("events")
    required = {"runtime_started", "gateway_ready", "stop_requested", "gateway_summary"}
    if not isinstance(events, list):
        failures.append("events: expected list")
    else:
        missing = sorted(required - {str(value) for value in events})
        if missing:
            failures.append(f"events: missing {missing}")
    return failures


def _evaluate_cycles(observation: dict[str, Any]) -> list[str]:
    checks = (
        _bounded_integer(observation, "cycles", 10, 10_000),
        _equals(observation, "failed_cycles", 0),
        _equals(observation, "residual_pids", 0),
        _bounded_integer(observation, "fd_drift", 0, 2),
        _bounded_integer(observation, "thread_drift", 0, 2),
    )
    return [value for value in checks if value]


_ORACLES = {
    "unit_static_verify": _evaluate_unit,
    "normal_start": _evaluate_normal_start,
    "sigterm_stop": _evaluate_sigterm,
    "sigint_direct": _evaluate_sigint,
    "abnormal_restart": _evaluate_restart,
    "invalid_config": _evaluate_invalid_config,
    "missing_serial": _evaluate_missing_serial,
    "broker_unavailable": _evaluate_broker_unavailable,
    "journal_observability": _evaluate_journal,
    "repeated_cycles": _evaluate_cycles,
}


def evaluate_scenario(name: str, observation: dict[str, Any]) -> dict[str, Any]:
    oracle = _ORACLES.get(name)
    if oracle is None:
        raise G5ContractError(f"unknown G5 scenario: {name}")
    failures = oracle(observation)
    return {
        "schema_version": "p3-s7-g5-scenario-v1",
        "scenario": name,
        "status": "PASS" if not failures else "FAIL",
        "failures": failures,
        "observation": observation,
    }


def _write_json(path: pathlib.Path, value: object) -> None:
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


@dataclasses.dataclass
class EvidenceStore:
    run_dir: pathlib.Path
    source_revision: str
    records: dict[str, dict[str, Any]] = dataclasses.field(default_factory=dict)

    @classmethod
    def create(
        cls,
        artifact_root: pathlib.Path,
        source_revision: str,
        *,
        now: datetime | None = None,
    ) -> "EvidenceStore":
        validate_native_inputs(FIXED_UNIT, source_revision)
        artifact_root.mkdir(parents=True, exist_ok=True)
        timestamp = (now or datetime.now(tz=UTC)).strftime("%Y%m%dT%H%M%SZ")
        prefix = f"{timestamp}_systemd_{source_revision[:8]}"
        for sequence in range(1, 1_000):
            run_dir = artifact_root / f"{prefix}_{sequence:03d}"
            try:
                run_dir.mkdir()
                break
            except FileExistsError:
                continue
        else:
            raise G5ContractError("cannot allocate unique G5 evidence directory")
        (run_dir / "IN_PROGRESS").write_text("G5 native run is in progress\n", encoding="utf-8")
        (run_dir / "events.jsonl").write_text("", encoding="utf-8")
        return cls(run_dir=run_dir, source_revision=source_revision)

    def event(self, event: str, **fields: object) -> None:
        record = {
            "timestamp_utc": datetime.now(tz=UTC).isoformat(),
            "event": event,
            **fields,
        }
        with (self.run_dir / "events.jsonl").open("a", encoding="utf-8") as output:
            output.write(json.dumps(record, ensure_ascii=False, sort_keys=True) + "\n")

    def record(self, record: dict[str, Any]) -> None:
        name = record.get("scenario")
        if name not in REQUIRED_G5_SCENARIOS:
            raise G5ContractError(f"invalid scenario record: {name!r}")
        if name in self.records:
            raise G5ContractError(f"scenario already recorded: {name}")
        self.records[str(name)] = record
        self.event("scenario_finished", scenario=name, status=record.get("status"))

    def finalize(self, *, environment_class: str, cleanup_ok: bool) -> int:
        summary = evaluate_g5(self.records, environment_class=environment_class)
        failures = [
            {
                "scenario": name,
                "failures": record.get("failures", []),
            }
            for name, record in self.records.items()
            if record.get("status") != "PASS"
        ]
        failures.extend(
            {
                "scenario": name,
                "failures": ["required scenario record is missing"],
            }
            for name in summary["missing_scenarios"]
        )
        if not cleanup_ok:
            failures.append({"scenario": "cleanup", "failures": ["cleanup did not complete"]})
        if failures and summary["status"] == "PASS":
            summary["status"] = "FAIL"
        summary.update(
            {
                "source_revision": self.source_revision,
                "cleanup_ok": cleanup_ok,
                "g4_inheritance_status": "REQUIRES_SEPARATE_REVIEW",
            }
        )
        _write_json(self.run_dir / "records.json", self.records)
        _write_json(self.run_dir / "failures.json", failures)
        _write_json(self.run_dir / "summary.json", summary)
        excluded = {"IN_PROGRESS", "PASS", "SHA256SUMS"}
        hash_paths = sorted(
            path
            for path in self.run_dir.rglob("*")
            if path.is_file() and path.name not in excluded
        )
        lines: list[str] = []
        for path in hash_paths:
            relative = path.relative_to(self.run_dir).as_posix()
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            lines.append(f"{digest}  {relative}")
        (self.run_dir / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="utf-8")
        (self.run_dir / "IN_PROGRESS").unlink(missing_ok=True)
        if summary["status"] == "PASS" and cleanup_ok and not failures:
            (self.run_dir / "PASS").write_text("G5 PASS\n", encoding="utf-8")
            return 0
        return 4

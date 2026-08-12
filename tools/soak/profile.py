from __future__ import annotations

import copy
import json
import pathlib
import re
from typing import Any

SCHEMA_VERSION = "1.0.0"
_REVISION = re.compile(r"^[0-9a-fA-F]{7,40}$")
_PROFILE_DURATIONS = {"release": 28800, "preflight": 3600}
_FAULT_KINDS = {
    "silent",
    "delayed_response",
    "bad_crc",
    "truncated_response",
    "exception_response",
    "broker_stop",
    "pty_disconnect",
}
_THRESHOLD_FIELDS = {
    "lifecycle": {
        "shutdown_timeout_seconds",
        "unexpected_exits_max",
        "critical_events_max",
        "parse_errors_max",
        "freshness_violations_max",
    },
    "requests": {
        "success_rate_min",
        "throughput_min_per_second",
        "latency_p95_ms_max",
        "latency_p99_ms_max",
        "latency_max_ms_exclusive",
        "non_target_starvation_seconds_max",
        "unclassified_errors_max",
    },
    "queues": {
        "request_rejections_max",
        "measurement_enqueue_failures_max",
        "critical_enqueue_failures_max",
        "drain_expired_max",
        "unconfirmed_on_close_max",
        "dropped_must_equal_expired_fresh",
    },
    "resources": {
        "rss_peak_mib_max",
        "rss_slope_mib_per_hour_max",
        "rss_stable_delta_mib_max",
        "cpu_average_percent_max",
        "cpu_p95_percent_max",
        "cpu_high_percent",
        "cpu_high_duration_seconds_max",
        "fd_peak_max",
        "fd_stable_drift_max",
        "thread_peak_max",
        "thread_stable_drift_max",
    },
    "disk": {
        "start_free_gib_min",
        "runtime_free_gib_min",
        "evidence_bytes_max",
        "log_segment_bytes_max",
    },
}


def _inside(path: pathlib.Path, root: pathlib.Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def _deep_merge(base: dict[str, Any], override: dict[str, Any]) -> dict[str, Any]:
    merged = copy.deepcopy(base)
    for key, value in override.items():
        if key in merged and isinstance(merged[key], dict) and isinstance(value, dict):
            merged[key] = _deep_merge(merged[key], value)
        else:
            merged[key] = copy.deepcopy(value)
    return merged


def _load_with_inheritance(
    path: pathlib.Path, repository_root: pathlib.Path, chain: tuple[pathlib.Path, ...] = ()
) -> dict[str, Any]:
    resolved = path.resolve()
    if not _inside(resolved, repository_root):
        raise ValueError("profile path must remain inside the repository")
    if resolved in chain:
        raise ValueError("profile inheritance cycle")
    try:
        value = json.loads(resolved.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot load profile: {error}") from error
    if not isinstance(value, dict):
        raise ValueError("profile root must be an object")
    parent = value.get("extends")
    if parent is None:
        return value
    if not isinstance(parent, str) or not parent:
        raise ValueError("extends must be a non-empty relative path")
    parent_path = (resolved.parent / parent).resolve()
    base = _load_with_inheritance(parent_path, repository_root, chain + (resolved,))
    override = dict(value)
    override.pop("extends", None)
    return _deep_merge(base, override)


def _require_number(value: Any, name: str, *, minimum: float = 0) -> None:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or value < minimum:
        raise ValueError(f"{name} must be a number >= {minimum}")


def _validate_repo_file(value: Any, name: str, repository_root: pathlib.Path) -> None:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{name} must be a repository-relative path")
    path = (repository_root / value).resolve()
    if not _inside(path, repository_root):
        raise ValueError(f"{name} escapes the repository")
    if not path.is_file():
        raise ValueError(f"{name} does not exist: {value}")


def validate_profile(profile: dict[str, Any], repository_root: pathlib.Path) -> None:
    required = {
        "schema_version",
        "profile_id",
        "profile_kind",
        "duration_seconds",
        "warmup_seconds",
        "sample_interval_seconds",
        "heartbeat_interval_seconds",
        "heartbeat_timeout_seconds",
        "fault_cycle_seconds",
        "fault_cycle_count",
        "load",
        "faults",
        "thresholds",
        "evidence",
    }
    missing = sorted(required.difference(profile))
    if missing:
        raise ValueError("profile is missing required fields: " + ", ".join(missing))
    if profile["schema_version"] != SCHEMA_VERSION:
        raise ValueError("unsupported profile schema_version")
    if not isinstance(profile["profile_id"], str) or not profile["profile_id"]:
        raise ValueError("profile_id must be non-empty")
    kind = profile["profile_kind"]
    if kind not in {"smoke", "preflight", "release"}:
        raise ValueError("profile_kind must be smoke, preflight, or release")
    if kind in _PROFILE_DURATIONS and profile["duration_seconds"] != _PROFILE_DURATIONS[kind]:
        raise ValueError(f"{kind} duration_seconds must be {_PROFILE_DURATIONS[kind]}")
    if kind == "smoke" and not 60 <= profile["duration_seconds"] <= 120:
        raise ValueError("smoke duration_seconds must be between 60 and 120")
    for field in (
        "duration_seconds",
        "sample_interval_seconds",
        "heartbeat_interval_seconds",
        "heartbeat_timeout_seconds",
        "fault_cycle_seconds",
        "fault_cycle_count",
    ):
        _require_number(profile[field], field, minimum=1)
    _require_number(profile["warmup_seconds"], "warmup_seconds")
    if profile["warmup_seconds"] >= profile["duration_seconds"]:
        raise ValueError("warmup_seconds must be shorter than duration_seconds")
    if profile["heartbeat_timeout_seconds"] < profile["heartbeat_interval_seconds"] * 2:
        raise ValueError("heartbeat_timeout_seconds must allow at least two heartbeat intervals")

    load = profile["load"]
    if not isinstance(load, dict):
        raise ValueError("load must be an object")
    serial = load.get("serial", {})
    if serial != {"baud": 19200, "data_bits": 8, "parity": "even", "stop_bits": 1}:
        raise ValueError("serial load must be frozen to 19200 8E1")
    if load.get("slave_count") != 3 or load.get("poll_job_count") != 16:
        raise ValueError("load must contain three slaves and sixteen poll jobs")
    if load.get("theoretical_poll_rate_per_second") != 26.2:
        raise ValueError("theoretical poll rate must be 26.2 requests/s")
    _validate_repo_file(load.get("register_map"), "load.register_map", repository_root)
    _validate_repo_file(load.get("scenario_map"), "load.scenario_map", repository_root)
    mqtt = load.get("mqtt", {})
    if mqtt.get("broker_host") not in {"127.0.0.1", "localhost"}:
        raise ValueError("MQTT broker must use a loopback address")
    if mqtt.get("protocol") != "3.1.1" or mqtt.get("qos") != 1:
        raise ValueError("MQTT load must use MQTT 3.1.1 and QoS 1")
    if mqtt.get("anonymous") is not True:
        raise ValueError("local soak broker must be anonymous")
    _require_number(mqtt.get("broker_port"), "load.mqtt.broker_port", minimum=1)

    thresholds = profile["thresholds"]
    if not isinstance(thresholds, dict):
        raise ValueError("thresholds must be an object")
    for section, names in _THRESHOLD_FIELDS.items():
        value = thresholds.get(section)
        if not isinstance(value, dict):
            raise ValueError(f"thresholds.{section} is required")
        missing_thresholds = sorted(names.difference(value))
        if missing_thresholds:
            raise ValueError(
                f"thresholds.{section} is missing: " + ", ".join(missing_thresholds)
            )

    faults = profile["faults"]
    if not isinstance(faults, list) or not faults:
        raise ValueError("faults must be a non-empty array")
    identifiers: set[str] = set()
    windows: list[tuple[float, float, str]] = []
    for fault in faults:
        if not isinstance(fault, dict):
            raise ValueError("each fault must be an object")
        fault_id = fault.get("fault_id")
        if not isinstance(fault_id, str) or not fault_id:
            raise ValueError("fault_id must be non-empty")
        if fault_id in identifiers:
            raise ValueError(f"duplicate fault_id: {fault_id}")
        identifiers.add(fault_id)
        if fault.get("kind") not in _FAULT_KINDS:
            raise ValueError(f"unsupported fault kind: {fault.get('kind')}")
        expected_actor = "runner" if fault["kind"] == "broker_stop" else "driver"
        if fault.get("actor") != expected_actor:
            raise ValueError(f"fault {fault_id} must use actor={expected_actor}")
        if expected_actor == "driver" and fault["kind"] != "pty_disconnect":
            if fault.get("target_slave") != 3:
                raise ValueError(f"fault {fault_id} must target slave 3")
        for field in ("offset_seconds", "duration_seconds", "recovery_timeout_seconds"):
            _require_number(fault.get(field), f"fault {fault_id}.{field}", minimum=0)
        start = float(fault["offset_seconds"])
        end = start + float(fault["duration_seconds"]) + float(fault["recovery_timeout_seconds"])
        if end > float(profile["fault_cycle_seconds"]):
            raise ValueError(f"fault {fault_id} recovery window exceeds the fault cycle")
        windows.append((start, end, fault_id))
    for previous, current in zip(sorted(windows), sorted(windows)[1:]):
        if current[0] < previous[1]:
            raise ValueError(f"fault windows overlap: {previous[2]} and {current[2]}")

    evidence = profile["evidence"]
    if not isinstance(evidence, dict) or evidence.get("raw_output_prefix") != "artifacts/soak/":
        raise ValueError("raw evidence must use artifacts/soak/")


def load_and_validate_profile(path: pathlib.Path, repository_root: pathlib.Path) -> dict[str, Any]:
    profile = _load_with_inheritance(path, repository_root)
    validate_profile(profile, repository_root)
    return profile


def validate_profile_pair(release: dict[str, Any], preflight: dict[str, Any]) -> None:
    if release.get("profile_kind") != "release" or preflight.get("profile_kind") != "preflight":
        raise ValueError("profile pair must be release and preflight")
    for field in ("load", "faults", "thresholds", "sample_interval_seconds", "heartbeat_timeout_seconds"):
        if release.get(field) != preflight.get(field):
            raise ValueError(f"release/preflight semantic drift in {field}")


def validate_execution_request(
    profile: dict[str, Any],
    source_revision: str,
    output_root: pathlib.Path,
    repository_root: pathlib.Path,
) -> None:
    if profile.get("profile_kind") in {"release", "preflight"} and not _REVISION.fullmatch(
        source_revision
    ):
        raise ValueError("formal execution requires a 7-40 hex source_revision")
    resolved_output = output_root.resolve()
    allowed = (repository_root / "artifacts/soak").resolve()
    if not _inside(resolved_output, allowed):
        raise ValueError("output root must remain under artifacts/soak/")

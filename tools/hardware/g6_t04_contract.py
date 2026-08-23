from __future__ import annotations

import copy
import json
import math
import pathlib
import re
import statistics
from collections import Counter, defaultdict
from collections.abc import Iterable, Mapping, Sequence
from typing import Any


PROFILE_SCHEMA = "p3-s7-g6-t04-profile-v1"
EXPECTED_PROFILE_IDS = {
    "preflight": "g6_t04_hardware_preflight_v1",
    "release": "g6_t04_hardware_release_v1",
}
EXPECTED_DURATIONS = {"preflight": 3600, "release": 28800}
EXPECTED_WARMUPS = {"preflight": 300, "release": 600}
FIXED_UNIT = "industrial_iot_gateway.service"
FIXED_MOSQUITTO_UNIT = "mosquitto.service"
FIXED_SERIAL_PATH = "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"
EXPECTED_DEVICES = {
    "1": {"device_name": "tas_env_01", "expected_task_count": 2},
    "2": {"device_name": "tas_env_02", "expected_task_count": 2},
    "4": {"device_name": "stm32_condition_node", "expected_task_count": 17},
}
EXPECTED_TASK_ADDRESSES = {
    "1": [0, 1],
    "2": [0, 1],
    "4": [0, 8, 12, 18, 20, 22, 24, 26, 28, 30, 50, 56, 66, 76, 86, 117, 120],
}
EXPECTED_THRESHOLDS = {
    "normal_success_rate_min": 0.999,
    "maximum_success_gap_ms": 10_000,
    "minimum_request_throughput_per_second": 3.0,
    "shutdown_timeout_ms": 5_000,
    "scheduler_feedback_queue_capacity": 256,
    "mqtt_queue_capacity": 1024,
    "rss_peak_max_mib": 256.0,
    "rss_slope_max_mib_per_hour": 2.0,
    "rss_stable_delta_max_mib": 16.0,
    "cpu_average_max_percent": 50.0,
    "cpu_p95_max_percent": 80.0,
    "cpu_above_90_max_seconds": 300,
    "fd_max": 64,
    "fd_drift_max": 4,
    "thread_max": 32,
    "thread_drift_max": 2,
    "temperature_max_c": 80.0,
    "minimum_disk_free_bytes": 15 * 1024**3,
    "evidence_warning_bytes": 8 * 1024**3,
    "evidence_risk_bytes": 9 * 1024**3,
    "evidence_max_bytes": 10 * 1024**3,
    "segment_max_bytes": 64 * 1024**2,
}
EXPECTED_EVIDENCE = {
    "raw_output_prefix": "artifacts/hardware/g6/t04/",
    "retain_all_gateway_events": True,
    "retain_all_mqtt_messages": True,
    "resource_sample_interval_seconds": 5,
    "allow_dynamic_downsampling": False,
    "overwrite_segments": False,
    "write_indices": True,
    "write_derived_csv_json": True,
}


class ContractError(ValueError):
    """Raised when a G6-T04 input violates the frozen hardware-soak contract."""


def validate_source_revision(revision: str) -> None:
    if re.fullmatch(r"[0-9a-f]{40}", revision) is None:
        raise ContractError("source revision must be a full lowercase 40-character Git id")


def _mapping(value: object, name: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise ContractError(f"{name} must be an object")
    return value


def validate_profile(profile: Mapping[str, Any]) -> None:
    if profile.get("schema_version") != PROFILE_SCHEMA:
        raise ContractError("unsupported G6-T04 profile schema")
    if profile.get("task_id") != "P3-S7-G6-T04":
        raise ContractError("profile task_id must be P3-S7-G6-T04")
    kind = profile.get("profile_kind")
    if kind not in EXPECTED_PROFILE_IDS:
        raise ContractError("profile_kind must be preflight or release")
    if profile.get("profile_id") != EXPECTED_PROFILE_IDS[str(kind)]:
        raise ContractError("profile_id does not match profile_kind")
    if profile.get("duration_seconds") != EXPECTED_DURATIONS[str(kind)]:
        raise ContractError("duration_seconds does not match the frozen profile")
    if profile.get("warmup_seconds") != EXPECTED_WARMUPS[str(kind)]:
        raise ContractError("warmup_seconds does not match the frozen profile")
    fixed = {
        "sample_interval_seconds": 5,
        "heartbeat_interval_seconds": 5,
        "heartbeat_timeout_seconds": 30,
        "systemd_unit": FIXED_UNIT,
        "mosquitto_unit": FIXED_MOSQUITTO_UNIT,
        "serial_path": FIXED_SERIAL_PATH,
        "register_map": "config/hardware/tas_dual_stm32_address4.yaml",
        "mqtt_topic_filter": "industrial_iot_gateway/#",
    }
    if any(profile.get(key) != value for key, value in fixed.items()):
        raise ContractError("profile changed a frozen runtime identity or sampling field")
    if dict(_mapping(profile.get("devices"), "devices")) != EXPECTED_DEVICES:
        raise ContractError("devices must match the frozen three-slave topology")
    addresses = _mapping(profile.get("task_addresses"), "task_addresses")
    if {str(key): list(value) for key, value in addresses.items()} != EXPECTED_TASK_ADDRESSES:
        raise ContractError("task_addresses must match all 21 frozen polling tasks")
    if dict(_mapping(profile.get("thresholds"), "thresholds")) != EXPECTED_THRESHOLDS:
        raise ContractError("thresholds must exactly match the frozen G6-T04 contract")
    if dict(_mapping(profile.get("evidence"), "evidence")) != EXPECTED_EVIDENCE:
        raise ContractError("evidence policy must retain complete bounded raw data")


def load_profile(path: pathlib.Path, repository_root: pathlib.Path) -> dict[str, Any]:
    resolved = path.resolve()
    allowed = (repository_root / "config/hardware").resolve()
    if resolved.parent != allowed or resolved.name not in {
        "g6_t04_preflight_profile.json",
        "g6_t04_release_profile.json",
    }:
        raise ContractError("profile must be a frozen file directly under config/hardware")
    value = json.loads(resolved.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ContractError("profile root must be an object")
    validate_profile(value)
    return value


def _number(value: object) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    result = float(value)
    return result if math.isfinite(result) else None


def _percentile(values: Sequence[float], percentile: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(percentile * len(ordered)) - 1))
    return ordered[index]


def _slope_per_hour(samples: Sequence[Mapping[str, Any]], field: str) -> float:
    points = []
    for sample in samples:
        elapsed = _number(sample.get("elapsed_seconds"))
        value = _number(sample.get(field))
        if elapsed is not None and value is not None:
            points.append((elapsed, value))
    if len(points) < 2:
        return 0.0
    mean_x = statistics.fmean(point[0] for point in points)
    mean_y = statistics.fmean(point[1] for point in points)
    denominator = sum((x - mean_x) ** 2 for x, _ in points)
    if denominator == 0:
        return 0.0
    per_second = sum((x - mean_x) * (y - mean_y) for x, y in points) / denominator
    return per_second * 3600.0


def _maximum_gap(values: Sequence[int], start_ms: int, end_ms: int) -> int:
    if not values:
        return end_ms - start_ms
    ordered = sorted(values)
    boundaries = [start_ms, *ordered, end_ms]
    return max(right - left for left, right in zip(boundaries, boundaries[1:]))


def _oracle(oracle_id: str, passed: bool, expected: object, actual: object) -> dict[str, Any]:
    return {
        "oracle_id": oracle_id,
        "enforced": True,
        "expected": expected,
        "actual": actual,
        "passed": bool(passed),
    }


def _logical_terminal_requests(rows: Sequence[Mapping[str, Any]]) -> tuple[list[Mapping[str, Any]], int]:
    grouped: dict[object, list[Mapping[str, Any]]] = defaultdict(list)
    anonymous = 0
    for row in rows:
        request_id = row.get("request_id")
        if isinstance(request_id, int) and not isinstance(request_id, bool):
            grouped[request_id].append(row)
        else:
            grouped[("anonymous", anonymous)].append(row)
            anonymous += 1
    terminal: list[Mapping[str, Any]] = []
    retries = 0
    for attempts in grouped.values():
        ordered = sorted(
            attempts,
            key=lambda row: (
                int(row.get("attempt", 0)) if isinstance(row.get("attempt"), int) else 0,
                int(row.get("monotonic_ms", 0)) if isinstance(row.get("monotonic_ms"), int) else 0,
            ),
        )
        retries += max(0, len(ordered) - 1)
        terminal.append(ordered[-1])
    return terminal, retries


def build_indices(
    gateway_events: Iterable[Mapping[str, Any]],
    mqtt_messages: Iterable[Mapping[str, Any]],
    resource_samples: Iterable[Mapping[str, Any]],
) -> dict[str, list[dict[str, Any]]]:
    requests: list[dict[str, Any]] = []
    mqtt: list[dict[str, Any]] = []
    timeline: list[dict[str, Any]] = []
    for event in gateway_events:
        if event.get("event") != "request_completed":
            continue
        row = {
            key: event.get(key)
            for key in (
                "source_file", "source_line", "event_id", "monotonic_ms", "request_id",
                "attempt", "slave_id", "function", "address", "result", "duration_ms",
            )
        }
        requests.append(row)
        timeline.append(
            {
                "kind": "request",
                "monotonic_ms": event.get("monotonic_ms"),
                "source_file": event.get("source_file"),
                "source_line": event.get("source_line"),
                "event_id": event.get("event_id"),
            }
        )
    for message in mqtt_messages:
        payload = message.get("payload") if isinstance(message.get("payload"), Mapping) else {}
        row = {
            "source_file": message.get("source_file"),
            "source_line": message.get("source_line"),
            "monotonic_ms": message.get("received_monotonic_ms"),
            "topic": message.get("topic"),
            "sequence": payload.get("sequence"),
            "quality": payload.get("quality"),
            "slave_id": payload.get("slave_id"),
        }
        mqtt.append(row)
        timeline.append(
            {
                "kind": "mqtt",
                "monotonic_ms": message.get("received_monotonic_ms"),
                "source_file": message.get("source_file"),
                "source_line": message.get("source_line"),
                "event_id": payload.get("event_id"),
            }
        )
    for sample in resource_samples:
        timeline.append(
            {
                "kind": "resource",
                "monotonic_ms": sample.get("monotonic_ms"),
                "source_file": sample.get("source_file"),
                "source_line": sample.get("source_line"),
                "event_id": sample.get("event_id"),
            }
        )
    timeline.sort(key=lambda row: (_number(row.get("monotonic_ms")) or 0, str(row["kind"])))
    return {"requests": requests, "mqtt": mqtt, "timeline": timeline}


def evaluate_run(profile: Mapping[str, Any], observation: Mapping[str, Any]) -> dict[str, Any]:
    validate_profile(profile)
    data = copy.deepcopy(dict(observation))
    thresholds = _mapping(profile["thresholds"], "thresholds")
    duration_seconds = int(profile["duration_seconds"])
    warmup_seconds = int(profile["warmup_seconds"])
    started_ms = int(data.get("started_monotonic_ms", 0))
    ended_ms = int(data.get("ended_monotonic_ms", started_ms))
    window_start = started_ms + warmup_seconds * 1000
    window_end = ended_ms
    actual_duration = max(0.0, (ended_ms - started_ms) / 1000.0)

    events = [row for row in data.get("gateway_events", []) if isinstance(row, Mapping)]
    normal_attempts = [
        row
        for row in events
        if row.get("event") == "request_completed"
        and isinstance(row.get("monotonic_ms"), int)
        and window_start <= int(row["monotonic_ms"]) <= window_end
        and row.get("result") != "shutdown_cancelled"
    ]
    normal, logical_retries = _logical_terminal_requests(normal_attempts)
    slaves: dict[str, dict[str, Any]] = {}
    oracles = [
        _oracle("duration.minimum", actual_duration >= duration_seconds, f">={duration_seconds}", actual_duration),
    ]
    for slave_id in (1, 2, 4):
        rows = [row for row in normal if row.get("slave_id") == slave_id]
        successes = [row for row in rows if row.get("result") == "success"]
        rate = len(successes) / len(rows) if rows else 0.0
        times = [int(row["monotonic_ms"]) for row in successes]
        gap = _maximum_gap(times, window_start, window_end)
        result_counts = dict(sorted(Counter(str(row.get("result")) for row in rows).items()))
        slave_attempt_count = sum(1 for row in normal_attempts if row.get("slave_id") == slave_id)
        slaves[str(slave_id)] = {
            "attempts": slave_attempt_count,
            "eligible_requests": len(rows),
            "successes": len(successes),
            "success_rate": rate,
            "maximum_success_gap_ms": gap,
            "logical_attempt_retries": max(0, slave_attempt_count - len(rows)),
            "result_counts": result_counts,
        }
        oracles.extend(
            [
                _oracle(
                    f"requests.slave_{slave_id}.success_rate",
                    rate >= float(thresholds["normal_success_rate_min"]),
                    f">={thresholds['normal_success_rate_min']}",
                    rate,
                ),
                _oracle(
                    f"requests.slave_{slave_id}.maximum_success_gap_ms",
                    gap <= int(thresholds["maximum_success_gap_ms"]),
                    f"<={thresholds['maximum_success_gap_ms']}",
                    gap,
                ),
            ]
        )

    successful = [row for row in normal if row.get("result") == "success"]
    window_seconds = max(1.0, (window_end - window_start) / 1000.0)
    throughput = len(successful) / window_seconds
    covered = {
        (str(row.get("slave_id")), int(row.get("address")))
        for row in successful
        if str(row.get("slave_id")) in EXPECTED_TASK_ADDRESSES
        and isinstance(row.get("address"), int)
    }
    expected = {
        (slave_id, address)
        for slave_id, addresses in EXPECTED_TASK_ADDRESSES.items()
        for address in addresses
    }
    task_coverage = {
        "expected": len(expected),
        "covered": len(covered & expected),
        "missing": [f"{slave}:{address}" for slave, address in sorted(expected - covered)],
    }
    oracles.extend(
        [
            _oracle(
                "requests.throughput_per_second",
                throughput >= float(thresholds["minimum_request_throughput_per_second"]),
                f">={thresholds['minimum_request_throughput_per_second']}",
                throughput,
            ),
            _oracle("requests.task_coverage", not task_coverage["missing"], 21, task_coverage["covered"]),
        ]
    )

    gateway_summary = data.get("gateway_summary") if isinstance(data.get("gateway_summary"), Mapping) else {}
    zero_fields = (
        "scheduler_feedback_delivery_failures",
        "scheduler_transition_errors",
        "mqtt_publish_failures",
        "mqtt_dropped",
        "mqtt_drain_expired",
    )
    for field in zero_fields:
        oracles.append(_oracle(f"gateway_summary.{field}", gateway_summary.get(field) == 0, 0, gateway_summary.get(field)))
    oracles.append(
        _oracle(
            "gateway_summary.serial_open_successes",
            gateway_summary.get("serial_open_successes") == 1,
            1,
            gateway_summary.get("serial_open_successes"),
        )
    )
    for field, threshold_name in (
        ("scheduler_feedback_queue_high_water", "scheduler_feedback_queue_capacity"),
        ("mqtt_queue_high_water", "mqtt_queue_capacity"),
    ):
        value = gateway_summary.get(field)
        capacity = int(thresholds[threshold_name])
        oracles.append(
            _oracle(
                f"gateway_summary.{field}",
                isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= capacity,
                f"0..{capacity}",
                value,
            )
        )
    oracles.append(_oracle("gateway_summary.stopped", gateway_summary.get("stopped") is True, True, gateway_summary.get("stopped")))
    oracles.append(
        _oracle(
            "shutdown.duration_ms",
            _number(data.get("shutdown_duration_ms")) is not None
            and float(data["shutdown_duration_ms"]) <= float(thresholds["shutdown_timeout_ms"]),
            f"<={thresholds['shutdown_timeout_ms']}",
            data.get("shutdown_duration_ms"),
        )
    )
    for field in ("serial_reopen_events_after_warmup", "unexpected_exits"):
        oracles.append(_oracle(f"runtime.{field}", data.get(field) == 0, 0, data.get(field)))

    mqtt = [row for row in data.get("mqtt_messages", []) if isinstance(row, Mapping)]
    mqtt_normal_all = [
        row for row in mqtt
        if isinstance(row.get("received_monotonic_ms"), int)
        and window_start <= int(row["received_monotonic_ms"]) <= window_end
    ]
    mqtt_run_id_mismatches = [
        row
        for row in mqtt_normal_all
        if not isinstance(row.get("payload"), Mapping)
        or row["payload"].get("run_id") != data.get("run_id")
    ]
    mqtt_normal = [row for row in mqtt_normal_all if row not in mqtt_run_id_mismatches]
    sequences = [
        int(payload["sequence"])
        for row in mqtt_normal
        for payload in [row.get("payload")]
        if isinstance(payload, Mapping) and isinstance(payload.get("sequence"), int)
    ]
    duplicate_sequences = len(sequences) - len(set(sequences))
    sequence_regressions = sum(1 for left, right in zip(sequences, sequences[1:]) if right <= left)
    fresh_count = sum(
        1 for row in mqtt_normal
        if isinstance(row.get("payload"), Mapping) and row["payload"].get("quality") == "fresh"
    )
    oracles.extend(
        [
            _oracle("mqtt.messages_present", bool(mqtt_normal), ">0", len(mqtt_normal)),
            _oracle("mqtt.fresh_present", fresh_count > 0, ">0", fresh_count),
            _oracle("mqtt.duplicate_sequences", duplicate_sequences == 0, 0, duplicate_sequences),
            _oracle("mqtt.sequence_regressions", sequence_regressions == 0, 0, sequence_regressions),
            _oracle("mqtt.run_id_mismatch", not mqtt_run_id_mismatches, 0, len(mqtt_run_id_mismatches)),
        ]
    )
    for slave_id, device in EXPECTED_DEVICES.items():
        topics = {
            str(row.get("topic"))
            for row in mqtt_normal
            if isinstance(row.get("payload"), Mapping)
            and str(row["payload"].get("slave_id")) == slave_id
            and row["payload"].get("message_type", "telemetry") == "telemetry"
            and "/registers/" in str(row.get("topic"))
        }
        expected_topics = int(device["expected_task_count"])
        oracles.append(
            _oracle(
                f"mqtt.slave_{slave_id}.telemetry_topic_coverage",
                len(topics) == expected_topics,
                expected_topics,
                len(topics),
            )
        )

    resources = [row for row in data.get("resource_samples", []) if isinstance(row, Mapping)]
    numeric = lambda field: [float(value) for row in resources if (value := _number(row.get(field))) is not None]
    rss = numeric("rss_mib")
    cpu = numeric("cpu_percent_single_core")
    fd = numeric("fd_count")
    threads = numeric("thread_count")
    temperature = numeric("soc_temperature_c")
    evidence = numeric("evidence_bytes")
    final_evidence = _number(data.get("final_evidence_bytes"))
    if final_evidence is not None:
        evidence.append(final_evidence)
    disk = numeric("disk_free_bytes")
    rss_slope = _slope_per_hour(resources, "rss_mib")
    rss_delta = (max(rss) - min(rss)) if rss else math.inf
    cpu_above_90_seconds = sum(int(profile["sample_interval_seconds"]) for value in cpu if value > 90.0)
    resource_checks = (
        ("resources.rss_peak_mib", bool(rss) and max(rss) <= thresholds["rss_peak_max_mib"], thresholds["rss_peak_max_mib"], max(rss) if rss else None),
        ("resources.rss_slope_mib_per_hour", bool(rss) and rss_slope <= thresholds["rss_slope_max_mib_per_hour"], thresholds["rss_slope_max_mib_per_hour"], rss_slope),
        ("resources.rss_stable_delta_mib", bool(rss) and rss_delta <= thresholds["rss_stable_delta_max_mib"], thresholds["rss_stable_delta_max_mib"], rss_delta),
        ("resources.cpu_average", bool(cpu) and statistics.fmean(cpu) <= thresholds["cpu_average_max_percent"], thresholds["cpu_average_max_percent"], statistics.fmean(cpu) if cpu else None),
        ("resources.cpu_p95", bool(cpu) and _percentile(cpu, 0.95) <= thresholds["cpu_p95_max_percent"], thresholds["cpu_p95_max_percent"], _percentile(cpu, 0.95) if cpu else None),
        ("resources.cpu_above_90_seconds", cpu_above_90_seconds <= thresholds["cpu_above_90_max_seconds"], thresholds["cpu_above_90_max_seconds"], cpu_above_90_seconds),
        ("resources.fd_max", bool(fd) and max(fd) <= thresholds["fd_max"], thresholds["fd_max"], max(fd) if fd else None),
        ("resources.fd_drift", bool(fd) and max(fd) - min(fd) <= thresholds["fd_drift_max"], thresholds["fd_drift_max"], max(fd) - min(fd) if fd else None),
        ("resources.thread_max", bool(threads) and max(threads) <= thresholds["thread_max"], thresholds["thread_max"], max(threads) if threads else None),
        ("resources.thread_drift", bool(threads) and max(threads) - min(threads) <= thresholds["thread_drift_max"], thresholds["thread_drift_max"], max(threads) - min(threads) if threads else None),
        ("arm64.temperature", bool(temperature) and max(temperature) < thresholds["temperature_max_c"], f"<{thresholds['temperature_max_c']}", max(temperature) if temperature else None),
        ("evidence.maximum_bytes", bool(evidence) and max(evidence) <= thresholds["evidence_max_bytes"], thresholds["evidence_max_bytes"], max(evidence) if evidence else None),
        ("host.minimum_disk_free", bool(disk) and min(disk) >= thresholds["minimum_disk_free_bytes"], thresholds["minimum_disk_free_bytes"], min(disk) if disk else None),
    )
    oracles.extend(_oracle(name, passed, expected_value, actual) for name, passed, expected_value, actual in resource_checks)
    for field in ("throttled_current_bits", "throttled_history_new_bits", "gateway_nrestarts", "mosquitto_nrestarts"):
        values = [row.get(field) for row in resources]
        oracles.append(_oracle(f"resources.{field}", bool(values) and all(value == 0 for value in values), "all 0", values))
    identities = {
        field: {row.get(field) for row in resources if row.get(field) not in (None, "")}
        for field in ("host_boot_id", "main_pid", "invocation_id")
    }
    for field, values in identities.items():
        oracles.append(_oracle(f"resources.{field}_stable", len(values) == 1, "one stable value", sorted(str(v) for v in values)))
    for field in ("gateway_active", "mosquitto_active"):
        values = [row.get(field) for row in resources]
        oracles.append(_oracle(f"resources.{field}", bool(values) and all(value is True for value in values), "all true", values))

    collector_failures = list(data.get("collector_failures", []))
    collection_gaps = list(data.get("collection_gaps", []))
    oracles.extend(
        [
            _oracle("evidence.collector_failures", not collector_failures, [], collector_failures),
            _oracle("evidence.collection_gaps", not collection_gaps, [], collection_gaps),
        ]
    )
    failures = [oracle for oracle in oracles if not oracle["passed"]]
    status = "PASS" if not failures else "FAIL"
    return {
        "schema_version": "p3-s7-g6-t04-summary-v1",
        "task_id": "P3-S7-G6-T04",
        "profile_id": profile["profile_id"],
        "profile_kind": profile["profile_kind"],
        "source_revision": data.get("source_revision"),
        "run_id": data.get("run_id"),
        "status": status,
        "hardware_long_soak_pass": status == "PASS" and profile["profile_kind"] == "release",
        "duration_seconds": actual_duration,
        "normal_window_seconds": window_seconds,
        "request_throughput_per_second": throughput,
        "slaves": slaves,
        "task_coverage": task_coverage,
        "mqtt": {
            "message_count": len(mqtt_normal),
            "fresh_count": fresh_count,
            "duplicate_sequences": duplicate_sequences,
            "sequence_regressions": sequence_regressions,
            "run_id_mismatches": len(mqtt_run_id_mismatches),
        },
        "logical_attempt_retries": logical_retries,
        "oracles": oracles,
        "unclosed_failures": len(failures),
    }

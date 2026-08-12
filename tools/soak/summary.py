from __future__ import annotations

import json
import math
import pathlib
import statistics
from typing import Any, Iterable

from .evidence import SCHEMA_VERSION, write_checksums, write_json_atomic


def _jsonl(
    paths: Iterable[pathlib.Path], *, tolerate_invalid: bool = False
) -> list[dict[str, Any]]:
    values: list[dict[str, Any]] = []
    for path in paths:
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            if not line.strip():
                continue
            try:
                value = json.loads(line)
            except json.JSONDecodeError as error:
                if tolerate_invalid:
                    values.append({"_parse_error": f"{path.name}:{number}: {error}"})
                    continue
                raise ValueError(f"invalid JSONL in {path.name}:{number}: {error}") from error
            if not isinstance(value, dict):
                raise ValueError(f"JSONL value in {path.name}:{number} is not an object")
            values.append(value)
    return values


def _percentile(values: list[float], percent: float) -> float:
    if not values:
        return math.nan
    ordered = sorted(values)
    rank = (len(ordered) - 1) * percent
    lower = math.floor(rank)
    upper = math.ceil(rank)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (rank - lower)


def _slope_per_hour(samples: list[dict[str, Any]], field: str, warmup: float) -> float:
    points = [(float(item["elapsed_seconds"]), float(item[field])) for item in samples if float(item["elapsed_seconds"]) >= warmup]
    if len(points) < 2:
        return 0.0
    x_mean = statistics.fmean(point[0] for point in points)
    y_mean = statistics.fmean(point[1] for point in points)
    denominator = sum((point[0] - x_mean) ** 2 for point in points)
    if denominator == 0:
        return 0.0
    per_second = sum((x - x_mean) * (y - y_mean) for x, y in points) / denominator
    return per_second * 3600.0


def _median_buckets(
    samples: list[dict[str, Any]], field: str, warmup: float, bucket_seconds: float = 300.0
) -> list[dict[str, Any]]:
    buckets: dict[int, list[float]] = {}
    for sample in samples:
        elapsed = float(sample["elapsed_seconds"])
        if elapsed < warmup:
            continue
        index = int((elapsed - warmup) // bucket_seconds)
        buckets.setdefault(index, []).append(float(sample[field]))
    return [
        {
            "elapsed_seconds": warmup + (index + 0.5) * bucket_seconds,
            field: statistics.median(values),
        }
        for index, values in sorted(buckets.items())
    ]


def _failure(failures: list[dict[str, Any]], oracle_id: str, expected: Any, actual: Any, severity: str = "P1") -> None:
    failures.append(
        {
            "failure_id": f"SOAK-{len(failures) + 1:03d}",
            "oracle_id": oracle_id,
            "severity": severity,
            "expected": expected,
            "actual": actual,
            "status": "OPEN",
        }
    )


def summarize(directory: pathlib.Path) -> dict[str, Any]:
    profile = json.loads((directory / "profile.json").read_text(encoding="utf-8"))
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    driver = _jsonl(sorted(directory.glob("driver_*.jsonl")))
    gateway = _jsonl(sorted(directory.glob("gateway_*.jsonl")), tolerate_invalid=True)
    resources = _jsonl([directory / "resource_samples.jsonl"])
    mqtt_messages = _jsonl(sorted(directory.glob("mqtt_messages_*.jsonl")))
    failures: list[dict[str, Any]] = []
    oracles: list[dict[str, Any]] = []

    def check(oracle_id: str, passed: bool, expected: Any, actual: Any, *, enforce: bool = True) -> None:
        oracles.append(
            {
                "oracle_id": oracle_id,
                "passed": passed,
                "enforced": enforce,
                "expected": expected,
                "actual": actual,
            }
        )
        if enforce and not passed:
            _failure(failures, oracle_id, expected, actual)

    started = next((item for item in driver if item.get("event") == "soak_driver_started"), None)
    stopped = next((item for item in reversed(driver) if item.get("event") == "soak_driver_stopped"), None)
    heartbeats = [item for item in driver if item.get("event") == "soak_heartbeat"]
    check("lifecycle.driver_started", started is not None, True, started is not None)
    check("lifecycle.driver_stopped", stopped is not None, True, stopped is not None)
    final_statistics = stopped.get("statistics", {}) if stopped else {}
    check("lifecycle.stopped_flag", final_statistics.get("stopped") is True, True, final_statistics.get("stopped"))
    shutdown_ms = stopped.get("shutdown_duration_ms") if stopped else None
    shutdown_limit = profile["thresholds"]["lifecycle"]["shutdown_timeout_seconds"] * 1000
    check("lifecycle.shutdown_budget", shutdown_ms is not None and shutdown_ms <= shutdown_limit, f"<= {shutdown_limit} ms", shutdown_ms)
    check("lifecycle.driver_internal_failure", not bool(stopped and stopped.get("internal_failure")), False, stopped.get("internal_failure") if stopped else None)
    gateway_parse_errors = sum(1 for item in gateway if "_parse_error" in item)
    check("evidence.gateway_json_parse_errors", gateway_parse_errors == 0, 0, gateway_parse_errors)

    heartbeat_gaps = []
    for previous, current in zip(heartbeats, heartbeats[1:]):
        heartbeat_gaps.append((current["monotonic_ms"] - previous["monotonic_ms"]) / 1000.0)
    maximum_heartbeat_gap = max(heartbeat_gaps, default=0.0)
    check("lifecycle.heartbeat_gap", maximum_heartbeat_gap <= profile["heartbeat_timeout_seconds"], f"<= {profile['heartbeat_timeout_seconds']} s", maximum_heartbeat_gap)

    runner_events = _jsonl([directory / "events.jsonl"])
    runner_types = [item.get("event_type") for item in runner_events]
    check("runner.provisional_status", manifest.get("status") == "PASS", "PASS", manifest.get("status"))
    check(
        "runner.no_internal_errors",
        not any(event_type in {"runner_exception", "json_parse_errors", "resource_stop", "heartbeat_timeout", "driver_duration_overrun", "driver_nonzero_exit"} for event_type in runner_types),
        "no runner failure event",
        runner_types,
    )
    driver_duration = (
        (float(stopped["monotonic_ms"]) - float(started["monotonic_ms"])) / 1000.0
        if started and stopped
        else 0.0
    )
    check(
        "lifecycle.profile_duration",
        driver_duration >= float(profile["duration_seconds"]) - 0.25,
        f">= {profile['duration_seconds'] - 0.25} s",
        driver_duration,
    )
    driver_fault_occurrences: dict[str, list[dict[str, Any]]] = {}
    for item in driver:
        fault_id = item.get("fault_id")
        if item.get("event") == "fault_started" and isinstance(fault_id, str):
            driver_fault_occurrences.setdefault(fault_id, []).append(
                {
                    "cycle": int(item.get("cycle", -1)),
                    "started_ms": float(item["monotonic_ms"]),
                    "cleared_ms": None,
                    "recovered": False,
                }
            )
        elif item.get("event") == "fault_cleared" and isinstance(fault_id, str):
            occurrences = driver_fault_occurrences.get(fault_id, [])
            if occurrences:
                occurrences[-1]["cleared_ms"] = float(item["monotonic_ms"])
        elif item.get("event") == "fault_recovered" and isinstance(fault_id, str):
            occurrences = driver_fault_occurrences.get(fault_id, [])
            if occurrences:
                occurrences[-1]["recovered"] = True

    runner_fault_occurrences: dict[str, dict[int, set[str]]] = {}
    for item in runner_events:
        details = item.get("details", {})
        fault_id = details.get("fault_id") if isinstance(details, dict) else None
        cycle = details.get("cycle") if isinstance(details, dict) else None
        if isinstance(fault_id, str) and isinstance(cycle, int):
            runner_fault_occurrences.setdefault(fault_id, {}).setdefault(cycle, set()).add(
                str(item.get("event_type"))
            )

    for fault in profile["faults"]:
        fault_id = fault["fault_id"]
        for cycle in range(int(profile["fault_cycle_count"])):
            oracle_prefix = f"fault.{fault_id}.cycle_{cycle}"
            if fault["actor"] == "driver":
                occurrence = next(
                    (
                        value
                        for value in driver_fault_occurrences.get(fault_id, [])
                        if value["cycle"] == cycle
                    ),
                    None,
                )
                check(f"{oracle_prefix}.triggered", occurrence is not None, True, occurrence is not None)
                check(
                    f"{oracle_prefix}.recovered",
                    bool(occurrence and occurrence["recovered"]),
                    True,
                    bool(occurrence and occurrence["recovered"]),
                )
            else:
                observed = runner_fault_occurrences.get(fault_id, {}).get(cycle, set())
                check(
                    f"{oracle_prefix}.triggered",
                    "broker_fault_started" in observed,
                    True,
                    "broker_fault_started" in observed,
                )
                check(
                    f"{oracle_prefix}.recovered",
                    "broker_fault_recovered" in observed,
                    True,
                    "broker_fault_recovered" in observed,
                )

    request_events = [item for item in gateway if item.get("event") == "request_completed"]
    driver_start_ms = float(started["monotonic_ms"]) if started else 0.0
    exclusion_windows: list[tuple[float, float]] = []
    for cycle in range(int(profile["fault_cycle_count"])):
        base = cycle * float(profile["fault_cycle_seconds"])
        for fault in profile["faults"]:
            exclusion_windows.append(
                (
                    base + float(fault["offset_seconds"]),
                    base + float(fault["offset_seconds"]) + float(fault["duration_seconds"]) + float(fault["recovery_timeout_seconds"]),
                )
            )
    def is_normal(item: dict[str, Any]) -> bool:
        elapsed = (float(item.get("monotonic_ms", 0)) - driver_start_ms) / 1000.0
        if elapsed < float(profile["warmup_seconds"]):
            return False
        return not any(start <= elapsed <= end for start, end in exclusion_windows)

    normal = [item for item in request_events if is_normal(item)]
    normal_success = [item for item in normal if item.get("result") == "success"]
    success_rate = len(normal_success) / len(normal) if normal else 0.0
    excluded = sum(max(0.0, min(float(profile["duration_seconds"]), end) - max(float(profile["warmup_seconds"]), start)) for start, end in exclusion_windows)
    normal_seconds = max(1.0, float(profile["duration_seconds"]) - float(profile["warmup_seconds"]) - excluded)
    throughput = len(normal_success) / normal_seconds
    latencies = [float(item["duration_ms"]) for item in normal_success if "duration_ms" in item]
    request_thresholds = profile["thresholds"]["requests"]
    strict_performance = profile["profile_kind"] != "smoke"
    check("requests.normal_success_rate", success_rate >= request_thresholds["success_rate_min"], f">= {request_thresholds['success_rate_min']}", success_rate, enforce=strict_performance)
    check("requests.normal_throughput", throughput >= request_thresholds["throughput_min_per_second"], f">= {request_thresholds['throughput_min_per_second']}", throughput, enforce=strict_performance)
    latency_p95 = _percentile(latencies, 0.95)
    latency_p99 = _percentile(latencies, 0.99)
    latency_max = max(latencies, default=math.nan)
    check("requests.latency_p95", bool(latencies) and latency_p95 <= request_thresholds["latency_p95_ms_max"], f"<= {request_thresholds['latency_p95_ms_max']} ms", latency_p95, enforce=strict_performance)
    check("requests.latency_p99", bool(latencies) and latency_p99 <= request_thresholds["latency_p99_ms_max"], f"<= {request_thresholds['latency_p99_ms_max']} ms", latency_p99, enforce=strict_performance)
    check("requests.latency_max", bool(latencies) and latency_max < request_thresholds["latency_max_ms_exclusive"], f"< {request_thresholds['latency_max_ms_exclusive']} ms", latency_max, enforce=strict_performance)
    known_results = {
        "success", "response_timeout", "crc_mismatch", "truncated_frame", "remote_exception",
        "serial_io_transient", "invalid_configuration", "broadcast_unsupported",
    }
    unclassified = sum(1 for item in request_events if item.get("result") not in known_results)
    check("requests.unclassified_errors", unclassified <= request_thresholds["unclassified_errors_max"], request_thresholds["unclassified_errors_max"], unclassified)

    for fault in profile["faults"]:
        if fault["actor"] != "driver":
            continue
        fault_id = fault["fault_id"]
        for cycle in range(int(profile["fault_cycle_count"])):
            occurrence = next(
                (
                    value
                    for value in driver_fault_occurrences.get(fault_id, [])
                    if value["cycle"] == cycle
                ),
                None,
            )
            matching = []
            if occurrence is not None and occurrence["cleared_ms"] is not None:
                matching = [
                    item
                    for item in request_events
                    if occurrence["started_ms"]
                    <= float(item.get("monotonic_ms", 0))
                    <= occurrence["cleared_ms"]
                    and item.get("result") == fault["expected_category"]
                ]
            check(
                f"fault.{fault_id}.cycle_{cycle}.category",
                bool(matching),
                fault["expected_category"],
                len(matching),
            )

    fairness_limit = float(request_thresholds["non_target_starvation_seconds_max"])
    fairness_gaps: list[float] = []
    for cycle in range(int(profile["fault_cycle_count"])):
        cycle_base = cycle * float(profile["fault_cycle_seconds"]) * 1000.0
        for fault in profile["faults"]:
            start = driver_start_ms + cycle_base + float(fault["offset_seconds"]) * 1000.0
            end = start + float(fault["duration_seconds"]) * 1000.0
            for slave_id in (1, 2):
                success_times = sorted(
                    float(item["monotonic_ms"])
                    for item in request_events
                    if item.get("result") == "success"
                    and item.get("slave_id") == slave_id
                    and start <= float(item.get("monotonic_ms", 0)) <= end
                )
                points = [start, *success_times, end]
                fairness_gaps.append(
                    max((right - left) / 1000.0 for left, right in zip(points, points[1:]))
                )
    maximum_fairness_gap = max(fairness_gaps, default=0.0)
    check("requests.non_target_fairness", maximum_fairness_gap <= fairness_limit, f"<= {fairness_limit} s", maximum_fairness_gap)

    queue_thresholds = profile["thresholds"]["queues"]
    for name in ("request_queue", "measurement_queue", "publish_queue"):
        queue = final_statistics.get(name, {})
        check(f"queues.{name}.bounded", queue.get("maximum_depth", 0) <= queue.get("capacity", -1), "maximum_depth <= capacity", queue)
    check("queues.measurement_enqueue_failures", final_statistics.get("measurement_enqueue_failures") == queue_thresholds["measurement_enqueue_failures_max"], queue_thresholds["measurement_enqueue_failures_max"], final_statistics.get("measurement_enqueue_failures"))
    request_queue_full = final_statistics.get("request_queue", {}).get("full")
    check("queues.request_rejections", request_queue_full == queue_thresholds["request_rejections_max"], queue_thresholds["request_rejections_max"], request_queue_full)
    publisher = final_statistics.get("publisher", {})
    for field in ("critical_enqueue_failures", "drain_expired", "unconfirmed_on_close"):
        check(f"mqtt.{field}", publisher.get(field) == queue_thresholds[f"{field}_max"], queue_thresholds[f"{field}_max"], publisher.get(field))
    check("mqtt.dropped_explained", publisher.get("dropped") == publisher.get("expired_fresh_dropped"), "dropped == expired_fresh_dropped", {"dropped": publisher.get("dropped"), "expired_fresh_dropped": publisher.get("expired_fresh_dropped")})
    if any(fault["kind"] == "broker_stop" for fault in profile["faults"]):
        check("mqtt.disconnect_observed", publisher.get("disconnected_events", 0) >= 1, ">= 1", publisher.get("disconnected_events"))
        check("mqtt.reconnect_observed", publisher.get("connected_events", 0) >= 2, ">= 2", publisher.get("connected_events"))

    statistics_snapshots = [
        item["statistics"]
        for item in heartbeats
        if isinstance(item.get("statistics"), dict)
    ]
    if final_statistics:
        statistics_snapshots.append(final_statistics)
    counter_paths: list[tuple[str, ...]] = [
        (field,)
        for field in (
            "requests_sent",
            "requests_succeeded",
            "requests_failed",
            "response_timeouts",
            "crc_errors",
            "truncated_frames",
            "remote_exceptions",
            "serial_errors",
            "serial_open_successes",
            "measurement_enqueue_failures",
        )
    ]
    for queue_name in ("request_queue", "measurement_queue", "publish_queue"):
        counter_paths.extend(
            (queue_name, field)
            for field in (
                "accepted",
                "popped",
                "full",
                "coalesced",
                "rejected_closed",
                "timed_out",
                "maximum_depth",
            )
        )
    counter_paths.extend(
        ("publisher", field)
        for field in (
            "connect_attempts",
            "connected_events",
            "disconnected_events",
            "publish_attempts",
            "publish_successes",
            "publish_failures",
            "coalesced",
            "dropped",
            "expired_fresh_dropped",
            "critical_enqueue_failures",
            "drain_expired",
            "unconfirmed_on_close",
        )
    )
    counter_regressions: list[str] = []
    for path in counter_paths:
        values: list[float] = []
        for snapshot in statistics_snapshots:
            value: Any = snapshot
            for component in path:
                if not isinstance(value, dict) or component not in value:
                    value = None
                    break
                value = value[component]
            if isinstance(value, (int, float)):
                values.append(float(value))
        if any(current < previous for previous, current in zip(values, values[1:])):
            counter_regressions.append(".".join(path))
    check("statistics.monotonic_counters", not counter_regressions, [], counter_regressions)

    payload_errors = 0
    for message in mqtt_messages:
        payload = message.get("payload")
        if not isinstance(payload, dict):
            payload_errors += 1
            continue
        quality = payload.get("quality")
        if quality == "invalid" and "value" in payload:
            payload_errors += 1
        if quality in {"stale", "offline"} and "value" in payload and payload.get("value_is_retained") is not True:
            payload_errors += 1
    check("mqtt.payload_semantics", payload_errors == 0, 0, payload_errors)
    critical_events = sum(1 for item in gateway if item.get("severity") == "critical")
    critical_limit = profile["thresholds"]["lifecycle"]["critical_events_max"]
    check("lifecycle.critical_events", critical_events <= critical_limit, critical_limit, critical_events)

    resource_thresholds = profile["thresholds"]["resources"]
    rss_peak = max((float(item["rss_mib"]) for item in resources), default=0.0)
    cpu_values = [float(item["cpu_percent_single_core"]) for item in resources]
    cpu_average = statistics.fmean(cpu_values) if cpu_values else 0.0
    cpu_p95 = _percentile(cpu_values, 0.95)
    fd_peak = max((int(item["fd_count"]) for item in resources), default=0)
    thread_peak = max((int(item["thread_count"]) for item in resources), default=0)
    rss_slope_samples = _median_buckets(
        resources, "rss_mib", float(profile["warmup_seconds"])
    )
    rss_slope = _slope_per_hour(rss_slope_samples, "rss_mib", 0.0)
    stable_samples = [item for item in resources if float(item["elapsed_seconds"]) >= float(profile["warmup_seconds"])]
    stable_span = 1800.0 if profile["profile_kind"] == "release" else 600.0
    first_window = [item for item in stable_samples if float(item["elapsed_seconds"]) <= float(profile["warmup_seconds"]) + stable_span]
    last_start = max(float(profile["warmup_seconds"]), float(profile["duration_seconds"]) - stable_span)
    last_window = [item for item in stable_samples if float(item["elapsed_seconds"]) >= last_start]
    def median_delta(field: str) -> float:
        if not first_window or not last_window:
            return 0.0
        return statistics.median(float(item[field]) for item in last_window) - statistics.median(float(item[field]) for item in first_window)
    rss_stable_delta = median_delta("rss_mib")
    fd_stable_drift = median_delta("fd_count")
    thread_stable_drift = median_delta("thread_count")
    high_cpu_limit = float(resource_thresholds["cpu_high_percent"])
    high_cpu_run = 0.0
    high_cpu_max = 0.0
    previous_elapsed: float | None = None
    for item in resources:
        elapsed_value = float(item["elapsed_seconds"])
        interval = 0.0 if previous_elapsed is None else elapsed_value - previous_elapsed
        previous_elapsed = elapsed_value
        if float(item["cpu_percent_single_core"]) > high_cpu_limit:
            high_cpu_run += interval
            high_cpu_max = max(high_cpu_max, high_cpu_run)
        else:
            high_cpu_run = 0.0
    resource_enforced = profile["profile_kind"] != "smoke"
    for oracle_id, passed, expected, actual in (
        ("resources.rss_peak", rss_peak <= resource_thresholds["rss_peak_mib_max"], resource_thresholds["rss_peak_mib_max"], rss_peak),
        ("resources.rss_slope", rss_slope <= resource_thresholds["rss_slope_mib_per_hour_max"], resource_thresholds["rss_slope_mib_per_hour_max"], rss_slope),
        ("resources.rss_stable_delta", rss_stable_delta <= resource_thresholds["rss_stable_delta_mib_max"], resource_thresholds["rss_stable_delta_mib_max"], rss_stable_delta),
        ("resources.cpu_average", cpu_average <= resource_thresholds["cpu_average_percent_max"], resource_thresholds["cpu_average_percent_max"], cpu_average),
        ("resources.cpu_p95", cpu_p95 <= resource_thresholds["cpu_p95_percent_max"], resource_thresholds["cpu_p95_percent_max"], cpu_p95),
        ("resources.fd_peak", fd_peak <= resource_thresholds["fd_peak_max"], resource_thresholds["fd_peak_max"], fd_peak),
        ("resources.thread_peak", thread_peak <= resource_thresholds["thread_peak_max"], resource_thresholds["thread_peak_max"], thread_peak),
        ("resources.fd_stable_drift", fd_stable_drift <= resource_thresholds["fd_stable_drift_max"], resource_thresholds["fd_stable_drift_max"], fd_stable_drift),
        ("resources.thread_stable_drift", thread_stable_drift <= resource_thresholds["thread_stable_drift_max"], resource_thresholds["thread_stable_drift_max"], thread_stable_drift),
        ("resources.sustained_high_cpu", high_cpu_max <= resource_thresholds["cpu_high_duration_seconds_max"], resource_thresholds["cpu_high_duration_seconds_max"], high_cpu_max),
    ):
        check(oracle_id, passed, expected, actual, enforce=resource_enforced)

    disk_thresholds = profile["thresholds"]["disk"]
    minimum_free = min((int(item["disk_free_bytes"]) for item in resources), default=0)
    evidence_peak = max((int(item["evidence_bytes"]) for item in resources), default=0)
    check("disk.runtime_free", minimum_free >= int(float(disk_thresholds["runtime_free_gib_min"]) * 1024**3), f">= {disk_thresholds['runtime_free_gib_min']} GiB", minimum_free)
    check("disk.evidence_size", evidence_peak <= int(disk_thresholds["evidence_bytes_max"]), f"<= {disk_thresholds['evidence_bytes_max']} bytes", evidence_peak)
    segment_limit = int(disk_thresholds["log_segment_bytes_max"])
    segment_patterns = (
        "driver_*.jsonl",
        "gateway_*.jsonl",
        "mqtt_messages_*.jsonl",
        "pty_*.log",
        "broker_*.log",
        "mqtt_subscriber_*.log",
    )
    oversized_segments = [
        {"name": path.name, "bytes": path.stat().st_size}
        for pattern in segment_patterns
        for path in sorted(directory.glob(pattern))
        if path.stat().st_size > segment_limit
    ]
    check(
        "disk.log_segment_size",
        not oversized_segments,
        f"each segment <= {segment_limit} bytes",
        oversized_segments,
    )

    metrics = {
        "request_events": len(request_events),
        "normal_request_events": len(normal),
        "normal_success_rate": success_rate,
        "normal_throughput_per_second": throughput,
        "latency_p95_ms": latency_p95 if math.isfinite(latency_p95) else None,
        "latency_p99_ms": latency_p99 if math.isfinite(latency_p99) else None,
        "latency_max_ms": latency_max if math.isfinite(latency_max) else None,
        "maximum_heartbeat_gap_seconds": maximum_heartbeat_gap,
        "maximum_non_target_gap_seconds": maximum_fairness_gap,
        "mqtt_message_count": len(mqtt_messages),
        "rss_peak_mib": rss_peak,
        "rss_slope_mib_per_hour": rss_slope,
        "rss_stable_delta_mib": rss_stable_delta,
        "fd_stable_drift": fd_stable_drift,
        "thread_stable_drift": thread_stable_drift,
        "maximum_sustained_high_cpu_seconds": high_cpu_max,
        "cpu_average_percent": cpu_average,
        "cpu_p95_percent": cpu_p95,
        "fd_peak": fd_peak,
        "thread_peak": thread_peak,
    }
    status = "PASS" if not failures and manifest.get("status") != "ABORTED_ENVIRONMENT" else "FAIL"
    summary = {
        "schema_version": SCHEMA_VERSION,
        "run_id": manifest["run_id"],
        "status": status,
        "profile_kind": profile["profile_kind"],
        "long_soak_pass": status == "PASS" and profile["profile_kind"] == "release",
        "metrics": metrics,
        "oracles": oracles,
        "unclosed_failures": len(failures),
    }
    write_json_atomic(directory / "summary.json", summary)
    write_json_atomic(directory / "failures.json", failures)
    write_checksums(directory)
    return summary

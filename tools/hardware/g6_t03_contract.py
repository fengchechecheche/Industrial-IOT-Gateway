from __future__ import annotations

import copy
import json
import pathlib
import re
from collections import Counter
from collections.abc import Iterable, Mapping, Sequence
from typing import Any


PROFILE_SCHEMA = "p3-s7-g6-t03-profile-v1"
SCENARIO_SCHEMA = "p3-s7-g6-t03-scenario-v1"
REBOOT_CHECKPOINT_SCHEMA = "p3-s7-g6-t03-reboot-checkpoint-v1"
FIXED_UNIT = "industrial_iot_gateway.service"
FIXED_SERIAL_PATH = "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"
EXPECTED_DEVICES = {
    1: "tas_env_01",
    2: "tas_env_02",
    4: "stm32_condition_node",
}
EXPECTED_SCENARIO_IDS = (
    "t03_f01_tas_a_cycle_2",
    "t03_f01_tas_a_cycle_3",
    "t03_f02_tas_b_cycle_2",
    "t03_f02_tas_b_cycle_3",
    "t03_f03_stm32_reset_1",
    "t03_f03_stm32_reset_2",
    "t03_f03_stm32_reset_3",
    "t03_f04_mosquitto_1",
    "t03_f04_mosquitto_2",
    "t03_f04_mosquitto_3",
    "t03_f05_usb_rs485_1",
    "t03_f05_usb_rs485_2",
    "t03_f05_usb_rs485_3",
    "t03_f06_sigterm_1",
    "t03_f06_sigterm_2",
    "t03_f06_sigterm_3",
    "t03_f07_reboot",
)
EXPECTED_THRESHOLDS = {
    "normal_success_rate_min": 0.999,
    "non_target_max_success_gap_ms": 10_000,
    "recovery_timeout_ms": 120_000,
    "ssh_recovery_timeout_ms": 300_000,
    "shutdown_timeout_ms": 5_000,
    "post_reboot_observation_ms": 600_000,
    "temperature_max_c": 80.0,
    "evidence_max_bytes": 5 * 1024 * 1024 * 1024,
    "minimum_disk_free_bytes": 15 * 1024 * 1024 * 1024,
}
ALLOWED_INSTALLED_PATHS = frozenset(
    {
        "/usr/local/bin/gateway_app",
        "/etc/systemd/system/industrial_iot_gateway.service",
        "/etc/industrial_iot_gateway/gateway.env",
        "/etc/industrial_iot_gateway/register_map.yaml",
    }
)


class ContractError(ValueError):
    """Raised when a G6-T03 input or observation violates the frozen contract."""


def validate_source_revision(revision: str) -> None:
    if re.fullmatch(r"[0-9a-f]{40}", revision) is None:
        raise ContractError("source revision must be a full lowercase 40-character Git id")


def _mapping(value: object, name: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise ContractError(f"{name} must be an object")
    return value


def validate_profile(profile: Mapping[str, Any]) -> None:
    if profile.get("schema_version") != PROFILE_SCHEMA:
        raise ContractError("unsupported G6-T03 profile schema")
    if profile.get("task_id") != "P3-S7-G6-T03":
        raise ContractError("profile task_id must be P3-S7-G6-T03")
    if profile.get("systemd_unit") != FIXED_UNIT:
        raise ContractError(f"only {FIXED_UNIT!r} may be operated on")

    serial = _mapping(profile.get("serial"), "serial")
    if serial.get("path") != FIXED_SERIAL_PATH:
        raise ContractError("profile must use the frozen stable serial path")
    expected_serial = {
        "baud": 19200,
        "data_bits": 8,
        "parity": "even",
        "stop_bits": 1,
        "minimum_request_interval_ms": 200,
    }
    if any(serial.get(key) != value for key, value in expected_serial.items()):
        raise ContractError("serial contract must remain 19200 8E1 with a 200 ms interval")

    devices = profile.get("devices")
    if not isinstance(devices, Sequence) or isinstance(devices, (str, bytes)):
        raise ContractError("devices must be a list")
    actual_devices: dict[int, str] = {}
    for value in devices:
        device = _mapping(value, "device")
        slave_id = device.get("slave_id")
        name = device.get("device_name")
        if isinstance(slave_id, bool) or not isinstance(slave_id, int) or not isinstance(name, str):
            raise ContractError("each device needs an integer slave_id and string device_name")
        actual_devices[slave_id] = name
    if actual_devices != EXPECTED_DEVICES:
        raise ContractError(f"devices must equal {EXPECTED_DEVICES!r}")

    thresholds = _mapping(profile.get("thresholds"), "thresholds")
    if dict(thresholds) != EXPECTED_THRESHOLDS:
        raise ContractError("thresholds must exactly match the frozen G6-T03 contract")
    if tuple(profile.get("scenario_ids", ())) != EXPECTED_SCENARIO_IDS:
        raise ContractError("scenario_ids must exactly match the frozen execution order")


def validate_action_transition(
    state: Mapping[str, Any], scenario_id: str, phase: str
) -> dict[str, Any]:
    if scenario_id not in EXPECTED_SCENARIO_IDS:
        raise ContractError(f"unknown scenario: {scenario_id}")
    if phase not in {"trigger", "restore", "complete"}:
        raise ContractError(f"unsupported action phase: {phase}")
    updated = copy.deepcopy(dict(state))
    completed = list(updated.get("completed", []))
    active = updated.get("active_scenario")
    if scenario_id in completed:
        raise ContractError(f"scenario already completed: {scenario_id}")
    if phase == "trigger":
        if updated.get("baseline_complete") is not True:
            raise ContractError("baseline must be complete before a fault trigger")
        if active is not None:
            raise ContractError(f"scenario already active: {active}")
        updated["active_scenario"] = scenario_id
        updated["active_phase"] = "triggered"
    elif phase == "restore":
        if active != scenario_id:
            raise ContractError(f"active scenario is {active!r}, not {scenario_id!r}")
        updated["active_phase"] = "restored"
    else:
        if active != scenario_id or updated.get("active_phase") != "restored":
            raise ContractError("scenario must be restored before completion")
        completed.append(scenario_id)
        updated["completed"] = completed
        updated["active_scenario"] = None
        updated["active_phase"] = None
        updated["baseline_complete"] = False
    return updated


def filter_mqtt_messages(
    lines: Iterable[str], *, run_id: str, slave_id: int
) -> dict[str, Any]:
    selected: list[dict[str, Any]] = []
    for line in lines:
        if " " not in line:
            continue
        _, payload_text = line.split(" ", 1)
        try:
            payload = json.loads(payload_text)
        except json.JSONDecodeError:
            continue
        if not isinstance(payload, dict):
            continue
        if payload.get("run_id") == run_id and payload.get("slave_id") == slave_id:
            selected.append(payload)
    states = [str(value["state"]) for value in selected if isinstance(value.get("state"), str)]
    qualities = Counter(
        str(value["quality"]) for value in selected if isinstance(value.get("quality"), str)
    )
    latest_fresh = next(
        (value for value in reversed(selected) if value.get("quality") == "fresh"), None
    )
    return {
        "message_count": len(selected),
        "states": states,
        "qualities": dict(sorted(qualities.items())),
        "latest_fresh": latest_fresh,
    }


def _maximum_gap(values: Sequence[int]) -> int:
    return max((right - left for left, right in zip(values, values[1:])), default=0)


def normal_window_statistics(
    events: Iterable[Mapping[str, Any]], *, start_ms: int, end_ms: int
) -> dict[str, Any]:
    grouped: dict[int, list[Mapping[str, Any]]] = {}
    for event in events:
        if event.get("event") != "request_completed":
            continue
        monotonic_ms = event.get("monotonic_ms")
        slave_id = event.get("slave_id")
        if (
            isinstance(monotonic_ms, int)
            and start_ms <= monotonic_ms <= end_ms
            and isinstance(slave_id, int)
        ):
            grouped.setdefault(slave_id, []).append(event)
    output: dict[str, Any] = {}
    for slave_id, rows in sorted(grouped.items()):
        eligible = [row for row in rows if row.get("result") != "shutdown_cancelled"]
        successes = [row for row in eligible if row.get("result") == "success"]
        success_times = [int(row["monotonic_ms"]) for row in successes]
        output[str(slave_id)] = {
            "eligible_requests": len(eligible),
            "successes": len(successes),
            "success_rate": len(successes) / len(eligible) if eligible else 0.0,
            "maximum_success_gap_ms": _maximum_gap(success_times),
            "result_counts": dict(sorted(Counter(str(row.get("result")) for row in rows).items())),
        }
    return output


def evaluate_baseline(
    observation: Mapping[str, Any], profile: Mapping[str, Any]
) -> dict[str, Any]:
    validate_profile(profile)
    thresholds = _mapping(profile["thresholds"], "thresholds")
    slaves = observation.get("slaves")
    values = slaves if isinstance(slaves, Mapping) else {}
    expected_coverage = {"1": 2, "2": 2, "4": 17}
    oracles: list[dict[str, Any]] = [
        _oracle(
            "baseline.main_pid",
            isinstance(observation.get("main_pid"), int)
            and not isinstance(observation.get("main_pid"), bool)
            and int(observation["main_pid"]) > 0,
            "> 0",
            observation.get("main_pid"),
        ),
        _oracle(
            "baseline.nrestarts",
            observation.get("nrestarts") == 0,
            0,
            observation.get("nrestarts"),
        ),
        _oracle(
            "baseline.mqtt_success",
            isinstance(observation.get("mqtt_publish_successes"), int)
            and int(observation["mqtt_publish_successes"]) > 0,
            "> 0",
            observation.get("mqtt_publish_successes"),
        ),
        _oracle(
            "baseline.mqtt_failures",
            observation.get("mqtt_publish_failures") == 0,
            0,
            observation.get("mqtt_publish_failures"),
        ),
    ]
    for slave_id, coverage in expected_coverage.items():
        raw = values.get(slave_id)
        slave = raw if isinstance(raw, Mapping) else {}
        success_rate = slave.get("success_rate")
        gap = slave.get("maximum_success_gap_ms")
        oracles.extend(
            [
                _oracle(
                    f"baseline.slave_{slave_id}.success_rate",
                    isinstance(success_rate, (int, float))
                    and not isinstance(success_rate, bool)
                    and float(success_rate) >= float(thresholds["normal_success_rate_min"]),
                    f">= {thresholds['normal_success_rate_min']}",
                    success_rate,
                ),
                _oracle(
                    f"baseline.slave_{slave_id}.gap",
                    isinstance(gap, int)
                    and not isinstance(gap, bool)
                    and gap <= int(thresholds["non_target_max_success_gap_ms"]),
                    f"<= {thresholds['non_target_max_success_gap_ms']}",
                    gap,
                ),
                _oracle(
                    f"baseline.slave_{slave_id}.coverage",
                    slave.get("register_coverage") == coverage,
                    coverage,
                    slave.get("register_coverage"),
                ),
            ]
        )
    failures = [
        f"{value['oracle_id']}: expected {value['expected']!r}, got {value['actual']!r}"
        for value in oracles
        if value["passed"] is not True
    ]
    return {
        "schema_version": "p3-s7-g6-t03-baseline-v1",
        "status": "PASS" if not failures else "FAIL",
        "oracles": oracles,
        "failures": failures,
    }


def _oracle(identifier: str, passed: bool, expected: object, actual: object) -> dict[str, Any]:
    return {
        "oracle_id": identifier,
        "passed": passed,
        "expected": expected,
        "actual": actual,
    }


def _ordered_states(actual: object) -> bool:
    if not isinstance(actual, Sequence) or isinstance(actual, (str, bytes)):
        return False
    values = list(actual)
    try:
        offline = values.index("offline")
        probing = values.index("probing", offline + 1)
        values.index("online", probing + 1)
        return True
    except ValueError:
        return False


def evaluate_scenario(
    scenario_kind: str, observation: Mapping[str, Any], profile: Mapping[str, Any]
) -> dict[str, Any]:
    validate_profile(profile)
    thresholds = _mapping(profile["thresholds"], "thresholds")
    gap_limit = int(thresholds["non_target_max_success_gap_ms"])
    recovery_limit = int(thresholds["recovery_timeout_ms"])
    oracles: list[dict[str, Any]] = []

    if scenario_kind == "tas_branch":
        gaps = observation.get("non_target_max_success_gap_ms")
        gap_values = dict(gaps) if isinstance(gaps, Mapping) else {}
        oracles.extend(
            [
                _oracle("target.timeouts", int(observation.get("target_timeout_count", 0)) > 0, "> 0", observation.get("target_timeout_count")),
                _oracle("non_target.gaps", bool(gap_values) and all(isinstance(value, int) and value <= gap_limit for value in gap_values.values()), f"<= {gap_limit}", gap_values),
                _oracle("process.pid_stable", observation.get("main_pid_unchanged") is True, True, observation.get("main_pid_unchanged")),
                _oracle("systemd.nrestarts", observation.get("nrestarts_delta") == 0, 0, observation.get("nrestarts_delta")),
                _oracle("mqtt.state_recovery", _ordered_states(observation.get("target_states")), "offline -> probing -> online", observation.get("target_states")),
                _oracle("mqtt.fresh_non_retained", observation.get("target_latest_fresh_retained") is False, False, observation.get("target_latest_fresh_retained")),
                _oracle("scheduler.feedback_failures", observation.get("scheduler_feedback_delivery_failures") == 0, 0, observation.get("scheduler_feedback_delivery_failures")),
                _oracle("scheduler.transition_errors", observation.get("scheduler_transition_errors") == 0, 0, observation.get("scheduler_transition_errors")),
                _oracle("target.recovery_time", isinstance(observation.get("recovery_time_ms"), int) and int(observation["recovery_time_ms"]) <= recovery_limit, f"<= {recovery_limit}", observation.get("recovery_time_ms")),
            ]
        )
    elif scenario_kind == "stm32_reset":
        gaps = observation.get("non_target_max_success_gap_ms")
        gap_values = dict(gaps) if isinstance(gaps, Mapping) else {}
        oracles.extend(
            [
                _oracle("reset.stimulus_observed", observation.get("stimulus_observed") is True, True, observation.get("stimulus_observed")),
                _oracle("non_target.gaps", bool(gap_values) and all(isinstance(value, int) and value <= gap_limit for value in gap_values.values()), f"<= {gap_limit}", gap_values),
                _oracle("target.register_coverage", observation.get("register_coverage") == 17, 17, observation.get("register_coverage")),
                _oracle("target.recovered_fresh", observation.get("target_fresh") is True, True, observation.get("target_fresh")),
                _oracle("process.pid_stable", observation.get("main_pid_unchanged") is True, True, observation.get("main_pid_unchanged")),
                _oracle("target.recovery_time", isinstance(observation.get("recovery_time_ms"), int) and int(observation["recovery_time_ms"]) <= recovery_limit, f"<= {recovery_limit}", observation.get("recovery_time_ms")),
            ]
        )
    elif scenario_kind == "mosquitto":
        rates = observation.get("normal_success_rates")
        gaps = observation.get("max_success_gap_ms")
        rate_values = dict(rates) if isinstance(rates, Mapping) else {}
        gap_values = dict(gaps) if isinstance(gaps, Mapping) else {}
        oracles.extend(
            [
                _oracle("serial.success_rates", bool(rate_values) and all(isinstance(value, (int, float)) and not isinstance(value, bool) and float(value) >= float(thresholds["normal_success_rate_min"]) for value in rate_values.values()), f">= {thresholds['normal_success_rate_min']}", rate_values),
                _oracle("serial.gaps", bool(gap_values) and all(isinstance(value, int) and value <= gap_limit for value in gap_values.values()), f"<= {gap_limit}", gap_values),
                _oracle("process.pid_stable", observation.get("main_pid_unchanged") is True, True, observation.get("main_pid_unchanged")),
                _oracle("mqtt.queue_bounded", observation.get("queue_bounded") is True, True, observation.get("queue_bounded")),
                _oracle("mqtt.reconnected", observation.get("reconnected") is True, True, observation.get("reconnected")),
                _oracle("mqtt.fresh_restored", observation.get("fresh_restored") is True, True, observation.get("fresh_restored")),
                _oracle("mqtt.drain_expired", observation.get("mqtt_drain_expired") == 0, 0, observation.get("mqtt_drain_expired")),
                _oracle("mqtt.recovery_time", isinstance(observation.get("recovery_time_ms"), int) and int(observation["recovery_time_ms"]) <= recovery_limit, f"<= {recovery_limit}", observation.get("recovery_time_ms")),
            ]
        )
    elif scenario_kind == "usb_rs485":
        successes = observation.get("slave_successes_after_recovery")
        success_values = dict(successes) if isinstance(successes, Mapping) else {}
        oracles.extend(
            [
                _oracle("serial.by_id_disappeared", observation.get("by_id_disappeared") is True, True, observation.get("by_id_disappeared")),
                _oracle("serial.by_id_restored", observation.get("by_id_restored") is True, True, observation.get("by_id_restored")),
                _oracle("serial.open_success_delta", isinstance(observation.get("serial_open_successes_delta"), int) and int(observation["serial_open_successes_delta"]) >= 1, ">= 1", observation.get("serial_open_successes_delta")),
                _oracle("serial.slaves_recovered", set(success_values) == {"1", "2", "4"} and all(isinstance(value, int) and value > 0 for value in success_values.values()), "slaves 1,2,4 > 0", success_values),
                _oracle("process.pid_stable", observation.get("main_pid_unchanged") is True, True, observation.get("main_pid_unchanged")),
                _oracle("systemd.nrestarts", observation.get("nrestarts_delta") == 0, 0, observation.get("nrestarts_delta")),
                _oracle("serial.recovery_time", isinstance(observation.get("recovery_time_ms"), int) and int(observation["recovery_time_ms"]) <= recovery_limit, f"<= {recovery_limit}", observation.get("recovery_time_ms")),
            ]
        )
    elif scenario_kind == "sigterm":
        oracles.extend(
            [
                _oracle("process.stop_rc", observation.get("stop_rc") == 0, 0, observation.get("stop_rc")),
                _oracle("process.stop_time", isinstance(observation.get("stop_ms"), int) and int(observation["stop_ms"]) <= int(thresholds["shutdown_timeout_ms"]), f"<= {thresholds['shutdown_timeout_ms']}", observation.get("stop_ms")),
                _oracle("process.inactive", observation.get("active_state") == "inactive" and observation.get("main_pid") == 0, "inactive/MainPID=0", {"active_state": observation.get("active_state"), "main_pid": observation.get("main_pid")}),
                _oracle("process.stopped_summary", observation.get("stopped") is True, True, observation.get("stopped")),
                _oracle("process.recovered", observation.get("all_slaves_and_mqtt_recovered") is True, True, observation.get("all_slaves_and_mqtt_recovered")),
                _oracle("process.residuals", observation.get("residual_pids") == 0, 0, observation.get("residual_pids")),
            ]
        )
    else:
        raise ContractError(f"unsupported scenario kind: {scenario_kind}")

    failures = [
        f"{value['oracle_id']}: expected {value['expected']!r}, got {value['actual']!r}"
        for value in oracles
        if value["passed"] is not True
    ]
    return {
        "schema_version": SCENARIO_SCHEMA,
        "scenario_kind": scenario_kind,
        "status": "PASS" if not failures else "FAIL",
        "oracles": oracles,
        "failures": failures,
    }


def validate_reboot_checkpoint(checkpoint: Mapping[str, Any], source_revision: str) -> None:
    validate_source_revision(source_revision)
    if checkpoint.get("schema_version") != REBOOT_CHECKPOINT_SCHEMA:
        raise ContractError("unsupported reboot checkpoint schema")
    if checkpoint.get("phase") != "PREPARED_FOR_REBOOT":
        raise ContractError("checkpoint phase is not PREPARED_FOR_REBOOT")
    if checkpoint.get("source_revision") != source_revision:
        raise ContractError("checkpoint source revision does not match candidate")
    digest = checkpoint.get("runner_sha256")
    if not isinstance(digest, str) or re.fullmatch(r"[0-9a-f]{64}", digest) is None:
        raise ContractError("runner SHA-256 is invalid")
    if not str(checkpoint.get("boot_id_before", "")).strip():
        raise ContractError("boot ID is missing")
    installed = checkpoint.get("installed_sha256")
    if not isinstance(installed, Mapping) or set(installed) != ALLOWED_INSTALLED_PATHS:
        raise ContractError("installed paths do not match the cleanup allowlist")
    for path, value in installed.items():
        if path not in ALLOWED_INSTALLED_PATHS:
            raise ContractError(f"installed path is outside the cleanup allowlist: {path}")
        if not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{64}", value) is None:
            raise ContractError(f"invalid installed SHA-256 for {path}")


def evaluate_cleanup_observation(observation: Mapping[str, Any]) -> dict[str, Any]:
    required_true = {
        "temporary_builds_absent": "temporary build directories must be absent",
        "temporary_helpers_absent": "temporary helper files must be absent",
        "pycache_absent": "pycache files must be absent",
        "gateway_active": "gateway service must remain active for T04 handoff",
        "mosquitto_active": "Mosquitto service must remain active for T04 handoff",
        "hardware_topology_restored": "three-slave hardware topology must be restored",
    }
    failures = [message for key, message in required_true.items() if observation.get(key) is not True]
    if observation.get("residual_test_processes") != 0:
        failures.append("residual test process count must be zero")
    disk_free = observation.get("disk_free_bytes")
    if (
        isinstance(disk_free, bool)
        or not isinstance(disk_free, int)
        or disk_free < EXPECTED_THRESHOLDS["minimum_disk_free_bytes"]
    ):
        failures.append("disk free space must remain at least 15 GiB")
    return {
        "schema_version": "p3-s7-g6-t03-cleanup-v1",
        "status": "PASS" if not failures else "FAIL",
        "failures": failures,
        "observation": dict(observation),
    }

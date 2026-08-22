from __future__ import annotations

from collections.abc import Mapping
from typing import Any


def _number(observation: Mapping[str, Any], key: str, default: float = 0.0) -> float:
    value = observation.get(key, default)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return default
    return float(value)


def evaluate_post_reboot(
    observation: Mapping[str, Any], checkpoint: Mapping[str, Any] | None = None
) -> list[str]:
    failures: list[str] = []
    before = str(observation.get("boot_id_before", ""))
    after = str(observation.get("boot_id_after", ""))
    if not before or not after or before == after:
        failures.append("boot ID did not change after controlled reboot")
    if checkpoint is not None:
        if before != checkpoint.get("boot_id_before"):
            failures.append("post-reboot boot ID baseline does not match checkpoint")
        for key, label in (
            ("source_revision", "source revision"),
            ("runner_sha256", "runner SHA-256"),
            ("installed_sha256", "installed file SHA-256 map"),
        ):
            if observation.get(key) != checkpoint.get(key):
                failures.append(f"post-reboot {label} does not match checkpoint")
    if observation.get("gateway_active") is not True:
        failures.append("gateway service is not active")
    if observation.get("mosquitto_active") is not True:
        failures.append("Mosquitto service is not active")
    if observation.get("gateway_enabled") is not True:
        failures.append("gateway service is not enabled")
    if observation.get("mosquitto_enabled") is not True:
        failures.append("Mosquitto service is not enabled")
    if observation.get("serial_by_id_ready") is not True:
        failures.append("stable USB-RS485 by-id path is not ready")
    if observation.get("serial_accessible") is not True:
        failures.append("gateway service user cannot access USB-RS485")
    if observation.get("journal_current_boot") is not True:
        failures.append("journal evidence is not scoped to the current boot")
    if observation.get("journal_required_events") is not True:
        failures.append("current-boot journal lacks required gateway events")
    if not 0 <= _number(observation, "ready_within_ms", float("inf")) <= 120_000:
        failures.append("gateway did not become ready within 120 seconds")
    successes = observation.get("slave_successes")
    values = successes if isinstance(successes, Mapping) else {}
    for slave_id in ("1", "2", "4"):
        value = values.get(slave_id, 0)
        if isinstance(value, bool) or not isinstance(value, int) or value < 1:
            failures.append(f"slave {slave_id} has no success after reboot")
    if _number(observation, "mqtt_publish_successes") < 1:
        failures.append("MQTT has no successful publish after reboot")
    if _number(observation, "nrestarts") != 0:
        failures.append("gateway NRestarts must remain 0")
    failed_units = observation.get("failed_units")
    if not isinstance(failed_units, list) or failed_units:
        failures.append("failed unit list is not empty")
    if observation.get("restart_loop") is not False:
        failures.append("restart loop was detected")
    if _number(observation, "temperature_peak_c", float("inf")) >= 80.0:
        failures.append("SoC temperature must remain below 80 C")
    if _number(observation, "throttled_current_bits") != 0:
        failures.append("current throttled bits must remain zero")
    if _number(observation, "throttled_history_new_bits") != 0:
        failures.append("historical throttled bits must not increase")
    if _number(observation, "observation_ms") < 600_000:
        failures.append("post-reboot observation must reach 600 seconds")
    return failures

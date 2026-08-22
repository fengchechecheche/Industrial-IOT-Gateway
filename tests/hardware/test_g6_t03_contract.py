from __future__ import annotations

import copy
import json
import pathlib
import tempfile
import unittest

from tools.hardware.g6_t03_contract import (
    ContractError,
    EXPECTED_SCENARIO_IDS,
    FIXED_SERIAL_PATH,
    evaluate_cleanup_observation,
    evaluate_scenario,
    filter_mqtt_messages,
    normal_window_statistics,
    validate_action_transition,
    validate_profile,
    validate_reboot_checkpoint,
    validate_source_revision,
)


REVISION = "b35a01e25c95fb427fbfa165c4e7f3b030f73b8d"


def valid_profile() -> dict[str, object]:
    return {
        "schema_version": "p3-s7-g6-t03-profile-v1",
        "task_id": "P3-S7-G6-T03",
        "systemd_unit": "industrial_iot_gateway.service",
        "serial": {
            "path": FIXED_SERIAL_PATH,
            "baud": 19200,
            "data_bits": 8,
            "parity": "even",
            "stop_bits": 1,
            "minimum_request_interval_ms": 200,
        },
        "devices": [
            {"slave_id": 1, "device_name": "tas_env_01"},
            {"slave_id": 2, "device_name": "tas_env_02"},
            {"slave_id": 4, "device_name": "stm32_condition_node"},
        ],
        "thresholds": {
            "normal_success_rate_min": 0.999,
            "non_target_max_success_gap_ms": 10000,
            "recovery_timeout_ms": 120000,
            "ssh_recovery_timeout_ms": 300000,
            "shutdown_timeout_ms": 5000,
            "post_reboot_observation_ms": 600000,
            "temperature_max_c": 80.0,
            "evidence_max_bytes": 5368709120,
            "minimum_disk_free_bytes": 16106127360,
        },
        "scenario_ids": list(EXPECTED_SCENARIO_IDS),
    }


class G6T03ContractTest(unittest.TestCase):
    def test_profile_freezes_real_hardware_contract(self) -> None:
        validate_profile(valid_profile())
        invalid = copy.deepcopy(valid_profile())
        invalid["serial"]["path"] = "/dev/ttyUSB0"  # type: ignore[index]
        with self.assertRaisesRegex(ContractError, "stable serial path"):
            validate_profile(invalid)
        invalid = copy.deepcopy(valid_profile())
        invalid["thresholds"]["non_target_max_success_gap_ms"] = 20000  # type: ignore[index]
        with self.assertRaisesRegex(ContractError, "thresholds"):
            validate_profile(invalid)

    def test_revision_must_be_a_full_lowercase_git_id(self) -> None:
        validate_source_revision(REVISION)
        for invalid in ("b35a01e", "B" * 40, "g" * 40):
            with self.subTest(invalid=invalid), self.assertRaises(ContractError):
                validate_source_revision(invalid)

    def test_action_state_machine_rejects_out_of_order_and_replayed_actions(self) -> None:
        state = {"baseline_complete": False, "active_scenario": None, "completed": []}
        with self.assertRaisesRegex(ContractError, "baseline"):
            validate_action_transition(state, "t03_f03_stm32_reset_1", "trigger")
        state["baseline_complete"] = True
        triggered = validate_action_transition(state, "t03_f03_stm32_reset_1", "trigger")
        with self.assertRaisesRegex(ContractError, "already active"):
            validate_action_transition(triggered, "t03_f03_stm32_reset_2", "trigger")
        restored = validate_action_transition(triggered, "t03_f03_stm32_reset_1", "restore")
        completed = validate_action_transition(restored, "t03_f03_stm32_reset_1", "complete")
        with self.assertRaisesRegex(ContractError, "already completed"):
            validate_action_transition(completed, "t03_f03_stm32_reset_1", "trigger")

    def test_mqtt_filter_uses_exact_run_id_and_target_slave(self) -> None:
        lines = [
            'topic {"run_id":"old","slave_id":1,"state":"offline"}',
            'topic {"run_id":"run-1","slave_id":2,"state":"offline"}',
            'topic {"run_id":"run-1","slave_id":1,"state":"offline"}',
            'topic {"run_id":"run-1","slave_id":1,"state":"online"}',
            'topic {"run_id":"run-1","slave_id":1,"quality":"fresh","value_is_retained":false}',
        ]
        filtered = filter_mqtt_messages(lines, run_id="run-1", slave_id=1)
        self.assertEqual(filtered["states"], ["offline", "online"])
        self.assertEqual(filtered["qualities"], {"fresh": 1})
        self.assertFalse(filtered["latest_fresh"]["value_is_retained"])

    def test_normal_window_excludes_shutdown_cancelled(self) -> None:
        events = [
            {"event": "request_completed", "slave_id": 1, "result": "success", "monotonic_ms": 100},
            {"event": "request_completed", "slave_id": 1, "result": "shutdown_cancelled", "monotonic_ms": 200},
            {"event": "request_completed", "slave_id": 1, "result": "success", "monotonic_ms": 300},
        ]
        statistics = normal_window_statistics(events, start_ms=0, end_ms=250)
        self.assertEqual(statistics["1"]["eligible_requests"], 1)
        self.assertEqual(statistics["1"]["success_rate"], 1.0)

    def test_branch_fault_oracle_accepts_isolated_recovery(self) -> None:
        observation = {
            "target_timeout_count": 4,
            "non_target_max_success_gap_ms": {"2": 5795, "4": 3503},
            "main_pid_unchanged": True,
            "nrestarts_delta": 0,
            "target_states": ["offline", "probing", "online"],
            "target_latest_fresh_retained": False,
            "scheduler_feedback_delivery_failures": 0,
            "scheduler_transition_errors": 0,
            "recovery_time_ms": 65000,
        }
        result = evaluate_scenario("tas_branch", observation, valid_profile())
        self.assertEqual(result["status"], "PASS")

    def test_every_failed_oracle_is_reported(self) -> None:
        observation = {
            "target_timeout_count": 0,
            "non_target_max_success_gap_ms": {"2": 10001, "4": 10002},
            "main_pid_unchanged": False,
            "nrestarts_delta": 1,
            "target_states": ["offline"],
            "target_latest_fresh_retained": True,
            "scheduler_feedback_delivery_failures": 1,
            "scheduler_transition_errors": 1,
            "recovery_time_ms": 120001,
        }
        result = evaluate_scenario("tas_branch", observation, valid_profile())
        self.assertEqual(result["status"], "FAIL")
        self.assertGreaterEqual(len(result["failures"]), 8)

    def test_reboot_checkpoint_rejects_wrong_identity_and_unsafe_paths(self) -> None:
        checkpoint = {
            "schema_version": "p3-s7-g6-t03-reboot-checkpoint-v1",
            "phase": "PREPARED_FOR_REBOOT",
            "source_revision": REVISION,
            "runner_sha256": "a" * 64,
            "boot_id_before": "boot-a",
            "installed_sha256": {
                "/usr/local/bin/gateway_app": "b" * 64,
                "/etc/systemd/system/industrial_iot_gateway.service": "c" * 64,
                "/etc/industrial_iot_gateway/gateway.env": "d" * 64,
                "/etc/industrial_iot_gateway/register_map.yaml": "e" * 64,
            },
        }
        validate_reboot_checkpoint(checkpoint, REVISION)
        with self.assertRaisesRegex(ContractError, "source revision"):
            validate_reboot_checkpoint(checkpoint, "0" * 40)
        unsafe = copy.deepcopy(checkpoint)
        unsafe["installed_sha256"]["/etc/passwd"] = "f" * 64  # type: ignore[index]
        with self.assertRaisesRegex(ContractError, "allowlist"):
            validate_reboot_checkpoint(unsafe, REVISION)

    def test_cleanup_requires_restored_services_topology_and_no_residuals(self) -> None:
        passing = {
            "temporary_builds_absent": True,
            "temporary_helpers_absent": True,
            "pycache_absent": True,
            "gateway_active": True,
            "mosquitto_active": True,
            "hardware_topology_restored": True,
            "residual_test_processes": 0,
            "disk_free_bytes": 20 * 1024**3,
        }
        self.assertEqual(evaluate_cleanup_observation(passing)["status"], "PASS")
        failed = dict(passing)
        failed["temporary_builds_absent"] = False
        failed["residual_test_processes"] = 1
        failed["disk_free_bytes"] = 14 * 1024**3
        result = evaluate_cleanup_observation(failed)
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(len(result["failures"]), 3)


if __name__ == "__main__":
    unittest.main()

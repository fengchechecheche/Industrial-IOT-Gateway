from __future__ import annotations

import json
import pathlib
import tempfile
import unittest

from tools.release.systemd_runner import REQUIRED_G5_SCENARIOS
from tools.release.systemd_scenarios import (
    EvidenceStore,
    G5ContractError,
    evaluate_scenario,
    journal_after_cursor_arguments,
    validate_native_inputs,
)


REVISION = "dd671cf3c8fda523ad5cc1e8539135fceda2b112"


def passing_observation(name: str) -> dict[str, object]:
    values: dict[str, dict[str, object]] = {
        "unit_static_verify": {
            "verify_rc": 0,
            "properties": {
                "Restart": "on-failure",
                "RestartUSec": "2s",
                "TimeoutStopUSec": "5s",
                "StartLimitBurst": "5",
                "KillSignal": "SIGTERM",
                "User": "iot-gw",
            },
        },
        "normal_start": {
            "active_state": "active",
            "main_pid": 1001,
            "gateway_ready": True,
            "requests_succeeded_delta": 3,
            "mqtt_publish_successes_delta": 1,
        },
        "sigterm_stop": {
            "stop_rc": 0,
            "stop_ms": 1200,
            "active_state": "inactive",
            "main_pid": 0,
            "stopped": True,
        },
        "sigint_direct": {"exit_code": 0, "stop_ms": 900, "stopped": True},
        "abnormal_restart": {
            "active_state": "active",
            "old_pid": 1001,
            "new_pid": 1002,
            "restart_count_delta": 1,
            "restart_ms": 2200,
        },
        "invalid_config": {
            "exit_code": 4,
            "restart_count_delta": 0,
            "active_state": "failed",
        },
        "missing_serial": {
            "active_state": "active",
            "serial_open_successes": 0,
            "journal_bytes": 2048,
            "cpu_time_delta_ms": 40,
        },
        "broker_unavailable": {
            "active_state": "active",
            "serial_successes_delta": 8,
            "mqtt_failures_delta": 2,
            "mqtt_reconnected": True,
            "mqtt_publish_success_after_recovery": True,
        },
        "journal_observability": {
            "cursor_scoped": True,
            "events": [
                "runtime_started",
                "gateway_ready",
                "stop_requested",
                "gateway_summary",
            ],
        },
        "repeated_cycles": {
            "cycles": 10,
            "failed_cycles": 0,
            "residual_pids": 0,
            "fd_drift": 0,
            "thread_drift": 0,
        },
    }
    return values[name]


class SystemdScenariosTest(unittest.TestCase):
    def test_all_frozen_scenario_oracles_accept_passing_observations(self) -> None:
        for name in REQUIRED_G5_SCENARIOS:
            with self.subTest(name=name):
                record = evaluate_scenario(name, passing_observation(name))
                self.assertEqual(record["status"], "PASS")
                self.assertEqual(record["scenario"], name)

    def test_boundary_failures_are_not_silently_accepted(self) -> None:
        failures = {
            "sigterm_stop": {"stop_ms": 5001},
            "abnormal_restart": {"restart_count_delta": 2},
            "missing_serial": {"journal_bytes": 65537},
            "repeated_cycles": {"residual_pids": 1},
        }
        for name, override in failures.items():
            observation = passing_observation(name)
            observation.update(override)
            with self.subTest(name=name):
                record = evaluate_scenario(name, observation)
                self.assertEqual(record["status"], "FAIL")
                self.assertTrue(record["failures"])

    def test_journal_query_is_cursor_scoped_and_argument_safe(self) -> None:
        arguments = journal_after_cursor_arguments(
            "industrial_iot_gateway.service", "s=0123456789abcdef;i=1;b=2;m=3;t=4;x=5"
        )
        self.assertEqual(arguments[0:2], ["journalctl", "--no-pager"])
        self.assertIn("--after-cursor", arguments)
        self.assertNotIn("--since", arguments)
        self.assertNotIn("bash", arguments)

    def test_only_fixed_unit_and_full_revision_are_accepted(self) -> None:
        validate_native_inputs("industrial_iot_gateway.service", REVISION)
        with self.assertRaises(G5ContractError):
            validate_native_inputs("ssh.service", REVISION)
        with self.assertRaises(G5ContractError):
            validate_native_inputs("industrial_iot_gateway.service", "dd671cf")

    def test_evidence_store_writes_traceable_failure_and_hashes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            store = EvidenceStore.create(pathlib.Path(directory), REVISION)
            for name in REQUIRED_G5_SCENARIOS:
                observation = passing_observation(name)
                if name == "sigterm_stop":
                    observation["stop_ms"] = 5001
                store.record(evaluate_scenario(name, observation))
            rc = store.finalize(environment_class="NATIVE_ELIGIBLE", cleanup_ok=True)
            self.assertNotEqual(rc, 0)
            summary = json.loads((store.run_dir / "summary.json").read_text(encoding="utf-8"))
            failures = json.loads((store.run_dir / "failures.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["status"], "FAIL")
            self.assertEqual(failures[0]["scenario"], "sigterm_stop")
            self.assertTrue((store.run_dir / "SHA256SUMS").is_file())
            self.assertFalse((store.run_dir / "PASS").exists())

    def test_non_native_environment_cannot_create_pass_marker(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            store = EvidenceStore.create(pathlib.Path(directory), REVISION)
            for name in REQUIRED_G5_SCENARIOS:
                store.record(evaluate_scenario(name, passing_observation(name)))
            rc = store.finalize(environment_class="DEVELOPMENT_ONLY", cleanup_ok=True)
            self.assertNotEqual(rc, 0)
            self.assertFalse((store.run_dir / "PASS").exists())


if __name__ == "__main__":
    unittest.main()

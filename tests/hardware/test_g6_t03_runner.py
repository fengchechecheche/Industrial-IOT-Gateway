from __future__ import annotations

import contextlib
import io
import json
import pathlib
import tempfile
import unittest

from tools.hardware.g6_t03_contract import EXPECTED_SCENARIO_IDS
from tools.hardware.g6_t03_runner import EvidenceStore, main


REVISION = "b35a01e25c95fb427fbfa165c4e7f3b030f73b8d"


def passing_record(scenario_id: str) -> dict[str, object]:
    return {
        "schema_version": "p3-s7-g6-t03-scenario-v1",
        "scenario_id": scenario_id,
        "scenario_kind": "test_fixture",
        "status": "PASS",
        "failures": [],
        "oracles": [{"oracle_id": "fixture.pass", "passed": True}],
        "evidence_event_ids": [f"run:{scenario_id}:000001"],
    }


def profile() -> dict[str, object]:
    return {
        "schema_version": "p3-s7-g6-t03-profile-v1",
        "task_id": "P3-S7-G6-T03",
        "systemd_unit": "industrial_iot_gateway.service",
        "serial": {
            "path": "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0",
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


def baseline_observation() -> dict[str, object]:
    return {
        "main_pid": 1234,
        "nrestarts": 0,
        "mqtt_publish_successes": 20,
        "mqtt_publish_failures": 0,
        "slaves": {
            "1": {"success_rate": 1.0, "maximum_success_gap_ms": 4800, "register_coverage": 2},
            "2": {"success_rate": 1.0, "maximum_success_gap_ms": 4900, "register_coverage": 2},
            "4": {"success_rate": 1.0, "maximum_success_gap_ms": 2800, "register_coverage": 17},
        },
    }


def reboot_checkpoint() -> dict[str, object]:
    return {
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


def passing_post_reboot_observation() -> dict[str, object]:
    return {
        "boot_id_before": "boot-a",
        "boot_id_after": "boot-b",
        "source_revision": REVISION,
        "runner_sha256": "a" * 64,
        "installed_sha256": {
            "/usr/local/bin/gateway_app": "b" * 64,
            "/etc/systemd/system/industrial_iot_gateway.service": "c" * 64,
            "/etc/industrial_iot_gateway/gateway.env": "d" * 64,
            "/etc/industrial_iot_gateway/register_map.yaml": "e" * 64,
        },
        "gateway_active": True,
        "mosquitto_active": True,
        "gateway_enabled": True,
        "mosquitto_enabled": True,
        "serial_by_id_ready": True,
        "serial_accessible": True,
        "journal_current_boot": True,
        "journal_required_events": True,
        "ready_within_ms": 30_000,
        "slave_successes": {"1": 3, "2": 3, "4": 5},
        "mqtt_publish_successes": 10,
        "nrestarts": 0,
        "failed_units": [],
        "restart_loop": False,
        "temperature_peak_c": 55.0,
        "throttled_current_bits": 0,
        "throttled_history_new_bits": 0,
        "observation_ms": 600_000,
        "evidence_event_ids": ["run:reboot:000001"],
    }


class G6T03RunnerTest(unittest.TestCase):
    def test_evidence_store_requires_all_scenarios_and_writes_hashes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            store = EvidenceStore.create(pathlib.Path(directory), REVISION, "run-1")
            for scenario_id in EXPECTED_SCENARIO_IDS:
                store.record_scenario(passing_record(scenario_id))
            rc = store.finalize(cleanup_ok=True)
            self.assertEqual(rc, 0)
            summary = json.loads((store.run_dir / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["status"], "PASS")
            self.assertEqual(summary["unclosed_failures"], 0)
            self.assertTrue((store.run_dir / "PASS").is_file())
            self.assertTrue(store.verify_checksums())

    def test_missing_scenario_or_cleanup_failure_cannot_create_pass(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            store = EvidenceStore.create(pathlib.Path(directory), REVISION, "run-2")
            store.record_scenario(passing_record(EXPECTED_SCENARIO_IDS[0]))
            rc = store.finalize(cleanup_ok=False)
            self.assertEqual(rc, 4)
            self.assertFalse((store.run_dir / "PASS").exists())
            self.assertTrue((store.run_dir / "FAIL").exists())

    def test_record_action_cli_rejects_restore_without_trigger(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            profile_path = root / "profile.json"
            profile_path.write_text(json.dumps(profile()), encoding="utf-8")
            self.assertEqual(
                main(
                    [
                        "initialize",
                        "--artifact-root",
                        str(root),
                        "--source-revision",
                        REVISION,
                        "--run-id",
                        "run-3",
                        "--profile",
                        str(profile_path),
                    ]
                ),
                0,
            )
            run_dir = root / "run-3"
            with self.assertRaisesRegex(ValueError, "active scenario"):
                main(
                    [
                        "record-action",
                        "--run-dir",
                        str(run_dir),
                        "--scenario-id",
                        "t03_f03_stm32_reset_1",
                        "--phase",
                        "restore",
                        "--user-confirmation",
                        "restored",
                    ]
                )

    def test_initialize_requires_profile_and_baseline_requires_passing_observation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                main(
                    [
                        "initialize",
                        "--artifact-root",
                        str(root),
                        "--source-revision",
                        REVISION,
                        "--run-id",
                        "missing-profile",
                    ]
                )
            profile_path = root / "profile.json"
            profile_path.write_text(json.dumps(profile()), encoding="utf-8")
            main(
                [
                    "initialize",
                    "--artifact-root",
                    str(root),
                    "--source-revision",
                    REVISION,
                    "--run-id",
                    "baseline",
                    "--profile",
                    str(profile_path),
                ]
            )
            observation_path = root / "baseline.json"
            failed = baseline_observation()
            failed["slaves"]["2"]["maximum_success_gap_ms"] = 10001  # type: ignore[index]
            observation_path.write_text(json.dumps(failed), encoding="utf-8")
            self.assertEqual(
                main(
                    [
                        "mark-baseline",
                        "--run-dir",
                        str(root / "baseline"),
                        "--observation",
                        str(observation_path),
                    ]
                ),
                4,
            )
            state = json.loads((root / "baseline" / "state.json").read_text(encoding="utf-8"))
            self.assertFalse(state["baseline_complete"])
            observation_path.write_text(json.dumps(baseline_observation()), encoding="utf-8")
            self.assertEqual(
                main(
                    [
                        "mark-baseline",
                        "--run-dir",
                        str(root / "baseline"),
                        "--observation",
                        str(observation_path),
                    ]
                ),
                0,
            )

    def test_finalize_cli_returns_four_for_failed_record(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            store = EvidenceStore.create(pathlib.Path(directory), REVISION, "run-4")
            failed = passing_record(EXPECTED_SCENARIO_IDS[0])
            failed["status"] = "FAIL"
            failed["failures"] = ["oracle failed"]
            store.record_scenario(failed)
            self.assertEqual(store.finalize(cleanup_ok=True), 4)

    def test_prepare_reboot_requires_completed_passing_prerequisites(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            store = EvidenceStore.create(root, REVISION, "reboot-prerequisite")
            checkpoint_path = root / "checkpoint.json"
            checkpoint_path.write_text(json.dumps(reboot_checkpoint()), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "prerequisites"):
                main(
                    [
                        "prepare-reboot",
                        "--run-dir",
                        str(store.run_dir),
                        "--checkpoint-input",
                        str(checkpoint_path),
                    ]
                )

            state_path = store.run_dir / "state.json"
            state = json.loads(state_path.read_text(encoding="utf-8"))
            state["baseline_complete"] = True
            state["completed"] = list(EXPECTED_SCENARIO_IDS[:-1])
            state_path.write_text(json.dumps(state), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "passing evidence"):
                main(
                    [
                        "prepare-reboot",
                        "--run-dir",
                        str(store.run_dir),
                        "--checkpoint-input",
                        str(checkpoint_path),
                    ]
                )

    def test_post_reboot_records_final_scenario_and_cleanup_is_evidence_based(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            store = EvidenceStore.create(root, REVISION, "reboot-pass")
            for scenario_id in EXPECTED_SCENARIO_IDS[:-1]:
                store.record_scenario(passing_record(scenario_id))
            state_path = store.run_dir / "state.json"
            state = json.loads(state_path.read_text(encoding="utf-8"))
            state["baseline_complete"] = True
            state["completed"] = list(EXPECTED_SCENARIO_IDS[:-1])
            state_path.write_text(json.dumps(state), encoding="utf-8")
            checkpoint_path = root / "checkpoint.json"
            checkpoint_path.write_text(json.dumps(reboot_checkpoint()), encoding="utf-8")
            self.assertEqual(
                main(
                    [
                        "prepare-reboot",
                        "--run-dir",
                        str(store.run_dir),
                        "--checkpoint-input",
                        str(checkpoint_path),
                    ]
                ),
                0,
            )
            observation_path = root / "post-reboot.json"
            observation_path.write_text(
                json.dumps(passing_post_reboot_observation()), encoding="utf-8"
            )
            self.assertEqual(
                main(
                    [
                        "post-reboot",
                        "--run-dir",
                        str(store.run_dir),
                        "--observation",
                        str(observation_path),
                    ]
                ),
                0,
            )
            final_state = json.loads(state_path.read_text(encoding="utf-8"))
            self.assertIn(EXPECTED_SCENARIO_IDS[-1], final_state["completed"])
            self.assertTrue(
                (store.run_dir / "scenarios" / EXPECTED_SCENARIO_IDS[-1] / "summary.json").is_file()
            )

            cleanup_path = root / "cleanup.json"
            cleanup_path.write_text(
                json.dumps(
                    {
                        "temporary_builds_absent": True,
                        "temporary_helpers_absent": True,
                        "pycache_absent": True,
                        "gateway_active": True,
                        "mosquitto_active": True,
                        "hardware_topology_restored": True,
                        "residual_test_processes": 0,
                        "disk_free_bytes": 20 * 1024**3,
                    }
                ),
                encoding="utf-8",
            )
            self.assertEqual(
                main(
                    [
                        "finalize",
                        "--run-dir",
                        str(store.run_dir),
                        "--cleanup-observation",
                        str(cleanup_path),
                    ]
                ),
                0,
            )
            self.assertTrue((store.run_dir / "PASS").is_file())


if __name__ == "__main__":
    unittest.main()

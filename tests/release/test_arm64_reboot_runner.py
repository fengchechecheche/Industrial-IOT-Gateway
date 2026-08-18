from __future__ import annotations

import inspect
import json
import os
import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock

from tools.release.arm64_reboot_runner import (
    RebootContractError,
    _fixture_unit_text,
    _validate_bundle_manifest,
    _write_json,
    evaluate_cleanup_observation,
    evaluate_post_reboot,
    parse_fixture_ready,
    validate_checkpoint,
)
from tools.release.manifest import REQUIRED_PACKAGE_PATHS, sha256_file


REVISION = "2a0c961bb9677fb3cc54b1e57be88a96cd9db82b"
TOOL_REVISION = "b" * 40


def passing_observation() -> dict[str, object]:
    return {
        "boot_id_before": "boot-a",
        "boot_id_after": "boot-b",
        "fixture_active": True,
        "gateway_active": True,
        "ready_within_seconds": 12.5,
        "slave_successes": {"1": 5, "2": 10, "3": 5},
        "mqtt_publish_successes": 21,
        "nrestarts": 0,
        "fixture_nrestarts": 0,
        "failed_units": [],
        "restart_loop": False,
        "temperature_peak_c": 56.2,
        "throttled_current_bits": 0,
        "throttled_history_new_bits": 0,
        "observation_seconds": 600.0,
    }


def passing_cleanup_observation() -> dict[str, object]:
    return {
        "disable_ok": True,
        "daemon_reload_ok": True,
        "reset_failed_ok": True,
        "gateway_unit_absent": True,
        "fixture_unit_absent": True,
        "gateway_disabled": True,
        "fixture_disabled": True,
        "gateway_inactive": True,
        "fixture_inactive": True,
        "user_absent": True,
        "group_absent": True,
        "owned_paths_absent": True,
        "owned_directories_absent": True,
        "runtime_directory_absent": True,
        "broker_port_released": True,
    }


class Arm64RebootRunnerTest(unittest.TestCase):
    def test_evidence_json_rejects_non_finite_numbers(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            target = pathlib.Path(directory) / "evidence.json"
            with self.assertRaises(ValueError):
                _write_json(target, {"ready_within_seconds": float("inf")})

    def test_fixture_start_limit_is_declared_in_unit_section(self) -> None:
        unit = _fixture_unit_text()
        unit_section, service_and_install = unit.split("[Service]", 1)
        service_section = service_and_install.split("[Install]", 1)[0]
        self.assertIn("StartLimitIntervalSec=60", unit_section)
        self.assertIn("StartLimitBurst=3", unit_section)
        self.assertNotIn("StartLimitIntervalSec", service_section)
        self.assertNotIn("StartLimitBurst", service_section)

    def test_fixture_ready_requires_numeric_devpts_target(self) -> None:
        master_fd, slave_fd = os.openpty()
        try:
            expected = os.ttyname(slave_fd)
            self.assertEqual(
                parse_fixture_ready(
                    json.dumps({"event": "pty_bus_ready", "path": expected})
                ),
                expected,
            )
            with self.assertRaisesRegex(RebootContractError, "pty_bus_ready"):
                parse_fixture_ready('{"event":"wrong","path":"/dev/pts/1"}')
            with self.assertRaisesRegex(RebootContractError, "beneath /dev/pts"):
                parse_fixture_ready(
                    '{"event":"pty_bus_ready","path":"/tmp/not-a-pty"}'
                )
        finally:
            os.close(slave_fd)
            os.close(master_fd)

    def test_checkpoint_freezes_pre_reboot_identity_and_owned_paths(self) -> None:
        checkpoint = {
            "schema_version": "p3-s7-arm64-reboot-checkpoint-v1",
            "phase": "PREPARED_FOR_REBOOT",
            "source_revision": REVISION,
            "acceptance_tool_revision": TOOL_REVISION,
            "runner_sha256": "d" * 64,
            "boot_id_before": "boot-a",
            "prepared_utc": "2026-08-15T05:00:00+00:00",
            "owned_paths": [
                "/etc/systemd/system/industrial_iot_gateway-reboot-fixture.service",
                "/etc/systemd/system/industrial_iot_gateway.service.d/reboot-test.conf",
                "/usr/local/bin/gateway_app",
            ],
            "installed_sha256": {
                "/etc/systemd/system/industrial_iot_gateway-reboot-fixture.service": "b" * 64,
                "/etc/systemd/system/industrial_iot_gateway.service.d/reboot-test.conf": "c" * 64,
                "/usr/local/bin/gateway_app": "a" * 64,
            },
            "owned_directories": [
                "/etc/industrial_iot_gateway",
                "/etc/systemd/system/industrial_iot_gateway.service.d",
                "/usr/local/libexec/industrial_iot_gateway",
                "/usr/local/share/doc/industrial_iot_gateway",
                "/usr/local/share/industrial_iot_gateway",
                "/var/lib/industrial_iot_gateway",
            ],
        }
        validate_checkpoint(checkpoint, REVISION, TOOL_REVISION)
        with self.assertRaisesRegex(RebootContractError, "source revision"):
            validate_checkpoint(checkpoint, "0" * 40, TOOL_REVISION)
        with self.assertRaisesRegex(RebootContractError, "tool revision"):
            validate_checkpoint(checkpoint, REVISION, "c" * 40)
        with self.assertRaisesRegex(RebootContractError, "phase"):
            validate_checkpoint(
                {**checkpoint, "phase": "PASS"}, REVISION, TOOL_REVISION
            )
        with self.assertRaisesRegex(RebootContractError, "runner hash"):
            validate_checkpoint(
                {**checkpoint, "runner_sha256": "invalid"}, REVISION, TOOL_REVISION
            )
        with self.assertRaisesRegex(RebootContractError, "absolute"):
            validate_checkpoint(
                {**checkpoint, "owned_paths": ["relative"]}, REVISION, TOOL_REVISION
            )
        with self.assertRaisesRegex(RebootContractError, "allowlist"):
            validate_checkpoint(
                {
                    **checkpoint,
                    "owned_paths": ["/etc/passwd"],
                    "installed_sha256": {"/etc/passwd": "a" * 64},
                },
                REVISION,
                TOOL_REVISION,
            )
        with self.assertRaisesRegex(RebootContractError, "same paths"):
            validate_checkpoint(
                {
                    **checkpoint,
                    "owned_paths": checkpoint["owned_paths"][:-1],
                },
                REVISION,
                TOOL_REVISION,
            )
        with self.assertRaisesRegex(RebootContractError, "directories"):
            validate_checkpoint(
                {**checkpoint, "owned_directories": ["/etc"]},
                REVISION,
                TOOL_REVISION,
            )

    def test_post_reboot_oracle_accepts_complete_ten_minute_recovery(self) -> None:
        self.assertEqual(evaluate_post_reboot(passing_observation()), [])

    def test_post_reboot_oracle_reports_every_frozen_failure(self) -> None:
        failed = {
            **passing_observation(),
            "boot_id_after": "boot-a",
            "fixture_active": False,
            "gateway_active": False,
            "ready_within_seconds": 121.0,
            "slave_successes": {"1": 1, "2": 0},
            "mqtt_publish_successes": 0,
            "nrestarts": 1,
            "fixture_nrestarts": 1,
            "failed_units": ["industrial_iot_gateway.service"],
            "restart_loop": True,
            "temperature_peak_c": 80.0,
            "throttled_current_bits": 1,
            "throttled_history_new_bits": 65536,
            "observation_seconds": 599.0,
        }
        failures = evaluate_post_reboot(failed)
        combined = "\n".join(failures)
        for marker in (
            "boot ID",
            "fixture",
            "gateway",
            "120",
            "slave 2",
            "slave 3",
            "MQTT",
            "NRestarts",
            "failed unit",
            "restart loop",
            "80",
            "current throttled",
            "historical throttled",
            "600",
        ):
            with self.subTest(marker=marker):
                self.assertIn(marker, combined)

    def test_cleanup_oracle_accepts_complete_system_cleanup(self) -> None:
        self.assertEqual(evaluate_cleanup_observation(passing_cleanup_observation()), [])

    def test_cleanup_oracle_reports_every_frozen_postcondition(self) -> None:
        failed = {key: False for key in passing_cleanup_observation()}
        failures = evaluate_cleanup_observation(failed)
        combined = "\n".join(failures)
        for marker in (
            "disable",
            "daemon-reload",
            "reset-failed",
            "gateway unit",
            "fixture unit",
            "gateway enable",
            "fixture enable",
            "gateway active",
            "fixture active",
            "user",
            "group",
            "owned path",
            "owned director",
            "runtime directory",
            "18884",
        ):
            with self.subTest(marker=marker):
                self.assertIn(marker, combined)

    def test_reset_failed_units_skips_reset_for_non_failed_or_missing_units(self) -> None:
        module = __import__(
            "tools.release.arm64_reboot_runner", fromlist=["_reset_failed_units"]
        )
        results = [
            subprocess.CompletedProcess([], 1, "active\n", ""),
            subprocess.CompletedProcess([], 1, "inactive\n", ""),
            subprocess.CompletedProcess([], 4, "inactive\n", ""),
        ]
        units = ("active.service", "inactive.service", "missing.service")
        with mock.patch.object(module, "_run", side_effect=results) as run:
            self.assertTrue(module._reset_failed_units(units))
        self.assertEqual(
            [call.args[0] for call in run.call_args_list],
            [["systemctl", "is-failed", unit] for unit in units],
        )

    def test_reset_failed_units_resets_failed_unit_and_confirms(self) -> None:
        module = __import__(
            "tools.release.arm64_reboot_runner", fromlist=["_reset_failed_units"]
        )
        results = [
            subprocess.CompletedProcess([], 0, "failed\n", ""),
            subprocess.CompletedProcess([], 0, "", ""),
            subprocess.CompletedProcess([], 1, "inactive\n", ""),
        ]
        with mock.patch.object(module, "_run", side_effect=results) as run:
            self.assertTrue(module._reset_failed_units(("failed.service",)))
        self.assertEqual(
            [call.args[0] for call in run.call_args_list],
            [
                ["systemctl", "is-failed", "failed.service"],
                ["systemctl", "reset-failed", "failed.service"],
                ["systemctl", "is-failed", "failed.service"],
            ],
        )

    def test_reset_failed_units_rejects_reset_failure(self) -> None:
        module = __import__(
            "tools.release.arm64_reboot_runner", fromlist=["_reset_failed_units"]
        )
        results = [
            subprocess.CompletedProcess([], 0, "failed\n", ""),
            subprocess.CompletedProcess([], 1, "", "permission denied"),
        ]
        with mock.patch.object(module, "_run", side_effect=results):
            self.assertFalse(module._reset_failed_units(("failed.service",)))

    def test_reset_failed_units_rejects_unexplained_probe_failure(self) -> None:
        module = __import__(
            "tools.release.arm64_reboot_runner", fromlist=["_reset_failed_units"]
        )
        probe = subprocess.CompletedProcess([], 1, "", "failed to connect to bus")
        with mock.patch.object(module, "_run", return_value=probe) as run:
            self.assertFalse(module._reset_failed_units(("broken.service",)))
        run.assert_called_once_with(["systemctl", "is-failed", "broken.service"])

    def test_cleanup_resets_failed_before_removing_unit_files(self) -> None:
        module = __import__(
            "tools.release.arm64_reboot_runner", fromlist=["_cleanup_owned"]
        )
        source = inspect.getsource(module._cleanup_owned)
        reset_failed = source.index("_reset_failed_units")
        unlink_files = source.index("path.unlink")
        daemon_reload = source.index('["systemctl", "daemon-reload"')
        self.assertLess(reset_failed, unlink_files)
        self.assertLess(unlink_files, daemon_reload)

    def test_bundle_manifest_requires_exact_arm64_candidate_capabilities(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            package_root = root / "package"
            records = []
            for index, relative in enumerate(REQUIRED_PACKAGE_PATHS):
                path = package_root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(f"fixture-{index}\n", encoding="utf-8")
                records.append({"path": relative, "sha256": sha256_file(path)})
            manifest_path = root / "install_manifest.json"
            manifest = {
                "schema_version": "p3-s7-release-manifest-v2",
                "source_revision": REVISION,
                "target_platform": "linux-arm64",
                "package_architecture": "arm64",
                "release_status": "candidate",
                "capabilities": {
                    "x86_64_validated": False,
                    "arm64_native_build_validated": True,
                    "arm64_systemd_validated": True,
                    "arm64_long_soak_validated": False,
                    "arm64_release_bundle_ready": False,
                    "hardware_validated": False,
                },
                "hardware_validated": False,
                "published": False,
                "tag": None,
                "files": records,
            }
            _write_json(manifest_path, manifest)
            _validate_bundle_manifest(package_root, manifest_path, REVISION)

            invalid_cases = {
                "release_status": {**manifest, "release_status": "final"},
                "tag": {**manifest, "tag": "v0.1.0"},
            }
            for capability, invalid_value in (
                ("x86_64_validated", True),
                ("arm64_native_build_validated", False),
                ("arm64_systemd_validated", False),
                ("arm64_long_soak_validated", True),
                ("arm64_release_bundle_ready", True),
                ("hardware_validated", True),
            ):
                changed = json.loads(json.dumps(manifest))
                changed["capabilities"][capability] = invalid_value
                invalid_cases[capability] = changed

            for label, invalid in invalid_cases.items():
                with self.subTest(label=label):
                    _write_json(manifest_path, invalid)
                    with self.assertRaisesRegex(
                        RebootContractError, "ARM64 candidate boundary"
                    ):
                        _validate_bundle_manifest(package_root, manifest_path, REVISION)

    def test_source_does_not_reboot_or_use_shell_and_global_kill(self) -> None:
        source = pathlib.Path(
            __import__("tools.release.arm64_reboot_runner", fromlist=["__file__"]).__file__
        ).read_text(encoding="utf-8")
        self.assertNotIn("shell=True", source)
        self.assertNotIn("pkill", source)
        self.assertNotIn("killall", source)
        self.assertNotIn('["reboot"]', source)
        self.assertNotIn('["systemctl", "reboot"]', source)

    def test_fixture_owns_children_and_publishes_stable_ready_signal(self) -> None:
        source = pathlib.Path(
            __import__("tools.release.arm64_reboot_fixture", fromlist=["__file__"]).__file__
        ).read_text(encoding="utf-8")
        self.assertNotIn("shell=True", source)
        self.assertNotIn("pkill", source)
        self.assertNotIn("killall", source)
        self.assertIn("systemd-notify", source)
        self.assertIn("mosquitto_sub", source)
        self.assertIn("os.replace", source)
        self.assertIn("start_new_session=True", source)


if __name__ == "__main__":
    unittest.main()

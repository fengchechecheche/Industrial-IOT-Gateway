from __future__ import annotations

import os
import pathlib
import tempfile
import unittest
from unittest import mock

from tools.release.native_systemd_runner import (
    G5_GATEWAY_ID,
    G5_MQTT_CLIENT_ID,
    CommandResult,
    NativeG5Runner,
    NativeInputs,
    input_evidence,
    resolve_shared_pty_path,
)
from tools.release.systemd_scenarios import EvidenceStore, G5ContractError


REVISION = "dd671cf3c8fda523ad5cc1e8539135fceda2b112"


class NativeSystemdRunnerTest(unittest.TestCase):
    def test_cleanup_before_ownership_does_not_operate_on_systemd_or_fixed_paths(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            inputs = NativeInputs(
                source_revision=REVISION,
                unit_file=root / "unit",
                gateway_app=root / "gateway",
                pty_bus=root / "pty",
                register_map=root / "map",
                scenario_config=root / "scenario",
                artifact_root=root,
                allow_full_vm=True,
            )
            store = EvidenceStore.create(root, REVISION)
            runner = NativeG5Runner(inputs, store)
            commands: list[list[str]] = []

            def forbidden_command(_label: str, arguments: list[str], _timeout: float = 15.0):
                commands.append(arguments)
                raise AssertionError("cleanup must not issue commands before ownership")

            runner.command = forbidden_command  # type: ignore[method-assign]
            self.assertTrue(runner.cleanup())
            self.assertEqual(commands, [])

    def test_g5_mqtt_client_id_obeys_runtime_identifier_contract(self) -> None:
        self.assertTrue(G5_MQTT_CLIENT_ID.isalnum())
        self.assertLessEqual(len(G5_MQTT_CLIENT_ID), 23)
        self.assertTrue(
            all(value.isalnum() or value == "_" for value in G5_GATEWAY_ID)
        )
        self.assertLessEqual(len(G5_GATEWAY_ID), 64)

    def test_shared_pty_resolver_returns_real_devpts_path(self) -> None:
        master_fd, slave_fd = os.openpty()
        try:
            expected = os.ttyname(slave_fd)
            with tempfile.TemporaryDirectory() as directory:
                alias = pathlib.Path(directory) / "gateway-serial"
                alias.symlink_to(expected)
                self.assertEqual(resolve_shared_pty_path(str(alias)), expected)
        finally:
            os.close(slave_fd)
            os.close(master_fd)

    def test_shared_pty_resolver_rejects_non_devpts_target(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            target = pathlib.Path(directory) / "not-a-pty"
            target.touch()
            alias = pathlib.Path(directory) / "gateway-serial"
            alias.symlink_to(target)
            with self.assertRaisesRegex(
                G5ContractError, "PTY alias must resolve beneath /dev/pts"
            ):
                resolve_shared_pty_path(str(alias))

    def test_cleanup_accepts_primary_group_already_removed_by_userdel(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            inputs = NativeInputs(
                source_revision=REVISION,
                unit_file=root / "unit",
                gateway_app=root / "gateway",
                pty_bus=root / "pty",
                register_map=root / "map",
                scenario_config=root / "scenario",
                artifact_root=root,
                allow_full_vm=True,
            )
            store = EvidenceStore.create(root, REVISION)
            runner = NativeG5Runner(inputs, store)
            runner.created_group = True
            commands: list[list[str]] = []

            def command(
                _label: str, arguments: list[str], _timeout: float = 15.0
            ) -> CommandResult:
                commands.append(arguments)
                return CommandResult(
                    command=arguments,
                    returncode=2 if arguments == ["getent", "group", "iot-gw"] else 0,
                    stdout="",
                    stderr="",
                    duration_ms=1,
                )

            runner.command = command  # type: ignore[method-assign]
            self.assertTrue(runner.cleanup())
            self.assertIn(["getent", "group", "iot-gw"], commands)
            self.assertNotIn(["groupdel", "iot-gw"], commands)

    def test_preexisting_state_directory_is_a_collision(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            state_root = root / "state"
            state_root.mkdir()
            inputs = NativeInputs(
                source_revision=REVISION,
                unit_file=root / "unit",
                gateway_app=root / "gateway",
                pty_bus=root / "pty",
                register_map=root / "map",
                scenario_config=root / "scenario",
                artifact_root=root,
                allow_full_vm=False,
                target_platform="linux-arm64",
            )
            store = EvidenceStore.create(root, REVISION)
            runner = NativeG5Runner(inputs, store)

            def command(
                _label: str, arguments: list[str], _timeout: float = 15.0
            ) -> CommandResult:
                return CommandResult(arguments, 1, "", "", 1)

            runner.command = command  # type: ignore[method-assign]
            with (
                mock.patch(
                    "tools.release.native_systemd_runner.STATE_ROOT", state_root
                ),
                mock.patch(
                    "tools.release.native_systemd_runner.pwd.getpwnam",
                    side_effect=KeyError,
                ),
                mock.patch(
                    "tools.release.native_systemd_runner.loopback_port_in_use",
                    return_value=False,
                ),
            ):
                with self.assertRaisesRegex(G5ContractError, str(state_root)):
                    runner._check_collisions()  # pylint: disable=protected-access

    def test_cleanup_removes_only_an_owned_state_directory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            state_root = root / "state"
            state_root.mkdir()
            (state_root / "runtime.json").write_text("{}\n", encoding="utf-8")
            inputs = NativeInputs(
                source_revision=REVISION,
                unit_file=root / "unit",
                gateway_app=root / "gateway",
                pty_bus=root / "pty",
                register_map=root / "map",
                scenario_config=root / "scenario",
                artifact_root=root,
                allow_full_vm=False,
                target_platform="linux-arm64",
            )
            store = EvidenceStore.create(root, REVISION)
            runner = NativeG5Runner(inputs, store)
            runner.owns_state_root = True
            with mock.patch(
                "tools.release.native_systemd_runner.STATE_ROOT", state_root
            ):
                self.assertTrue(runner.cleanup())
            self.assertFalse(state_root.exists())

    def test_cleanup_preserves_unowned_state_directory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            state_root = root / "state"
            state_root.mkdir()
            inputs = NativeInputs(
                source_revision=REVISION,
                unit_file=root / "unit",
                gateway_app=root / "gateway",
                pty_bus=root / "pty",
                register_map=root / "map",
                scenario_config=root / "scenario",
                artifact_root=root,
                allow_full_vm=False,
            )
            store = EvidenceStore.create(root, REVISION)
            runner = NativeG5Runner(inputs, store)
            with mock.patch(
                "tools.release.native_systemd_runner.STATE_ROOT", state_root
            ):
                self.assertTrue(runner.cleanup())
            self.assertTrue(state_root.is_dir())

    def test_broker_port_collision_is_detected_before_setup(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            inputs = NativeInputs(
                source_revision=REVISION,
                unit_file=root / "unit",
                gateway_app=root / "gateway",
                pty_bus=root / "pty",
                register_map=root / "map",
                scenario_config=root / "scenario",
                artifact_root=root,
                allow_full_vm=False,
            )
            store = EvidenceStore.create(root, REVISION)
            runner = NativeG5Runner(inputs, store)

            def command(
                _label: str, arguments: list[str], _timeout: float = 15.0
            ) -> CommandResult:
                return CommandResult(arguments, 1, "", "", 1)

            runner.command = command  # type: ignore[method-assign]
            with (
                mock.patch(
                    "tools.release.native_systemd_runner.pwd.getpwnam",
                    side_effect=KeyError,
                ),
                mock.patch(
                    "tools.release.native_systemd_runner.loopback_port_in_use",
                    return_value=True,
                ),
            ):
                with self.assertRaisesRegex(G5ContractError, "127.0.0.1:18884"):
                    runner._check_collisions()  # pylint: disable=protected-access

    def test_input_evidence_hashes_all_runtime_and_runner_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            paths = {
                "unit_file": root / "unit",
                "gateway_app": root / "gateway",
                "pty_bus": root / "pty",
                "register_map": root / "map",
                "scenario_config": root / "scenario",
            }
            for name, path in paths.items():
                path.write_text(f"{name}\n", encoding="utf-8")
            inputs = NativeInputs(
                source_revision=REVISION,
                artifact_root=root,
                allow_full_vm=False,
                target_platform="linux-arm64",
                **paths,
            )
            with mock.patch(
                "tools.release.native_systemd_runner.package_versions",
                return_value={"cmake": "test"},
            ):
                evidence = input_evidence(inputs)
            self.assertEqual(evidence["source_revision"], REVISION)
            self.assertEqual(evidence["expected_platform"], "linux-arm64")
            self.assertEqual(evidence["package_versions"], {"cmake": "test"})
            files = evidence["files"]
            self.assertIsInstance(files, dict)
            for name in (*paths, "native_runner", "platform_classifier", "scenario_oracles"):
                record = files[name]
                self.assertRegex(record["sha256"], r"^[0-9a-f]{64}$")
                self.assertGreater(record["size_bytes"], 0)

    def test_broker_outage_refreshes_pty_before_starting_service(self) -> None:
        source = pathlib.Path(
            __import__("tools.release.native_systemd_runner", fromlist=["__file__"]).__file__
        ).read_text(encoding="utf-8")
        start = source.index("    def scenario_broker_unavailable")
        end = source.index("    def scenario_journal_observability", start)
        scenario = source[start:end]
        refresh = scenario.index("self._restart_pty_fixture()")
        service_start = scenario.index('self._start_service("g508")')
        self.assertLess(refresh, service_start)

    def test_source_uses_owned_process_groups_and_no_global_kill(self) -> None:
        source = pathlib.Path(
            __import__("tools.release.native_systemd_runner", fromlist=["__file__"]).__file__
        ).read_text(encoding="utf-8")
        self.assertNotIn("pkill", source)
        self.assertNotIn("killall", source)
        self.assertNotIn("shell=True", source)
        self.assertIn("os.killpg(process.pid", source)


if __name__ == "__main__":
    unittest.main()

from __future__ import annotations

import pathlib
import tempfile
import unittest

from tools.release.native_systemd_runner import (
    G5_GATEWAY_ID,
    G5_MQTT_CLIENT_ID,
    CommandResult,
    NativeG5Runner,
    NativeInputs,
)
from tools.release.systemd_scenarios import EvidenceStore


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

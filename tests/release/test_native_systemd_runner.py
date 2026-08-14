from __future__ import annotations

import pathlib
import tempfile
import unittest

from tools.release.native_systemd_runner import NativeG5Runner, NativeInputs
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

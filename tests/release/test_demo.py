from __future__ import annotations

import contextlib
import io
import pathlib
import signal
import tempfile
import unittest
from unittest import mock

from tools import demo
from tools.fault_matrix import process_manager
from tools.fault_matrix.models import ProcessResult


def _result(command: list[str], *, returncode: int = 0, timed_out: bool = False) -> ProcessResult:
    return ProcessResult(
        command=command,
        returncode=returncode,
        stdout="step output\n",
        stderr="",
        duration_ms=10,
        timed_out=timed_out,
        termination_signal="SIGKILL" if timed_out else None,
    )


class FakeRunner:
    def __init__(self, results: list[ProcessResult] | None = None) -> None:
        self.results = list(results or [])
        self.calls: list[tuple[list[str], float, str | None]] = []

    def __call__(
        self, command: list[str], *, timeout_seconds: float, cwd: str | None = None
    ) -> ProcessResult:
        self.calls.append((list(command), timeout_seconds, cwd))
        if self.results:
            result = self.results.pop(0)
            return ProcessResult(
                command=list(command),
                returncode=result.returncode,
                stdout=result.stdout,
                stderr=result.stderr,
                duration_ms=result.duration_ms,
                timed_out=result.timed_out,
                termination_signal=result.termination_signal,
            )
        return _result(list(command))


def _which(name: str) -> str | None:
    return f"/usr/bin/{name}"


class DemoCliTest(unittest.TestCase):
    def test_parser_rejects_unknown_profile(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            demo.parse_arguments(["--profile", "unknown"])
        self.assertEqual(raised.exception.code, 2)

    def test_success_uses_configure_build_and_one_precise_ctest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            (root / "build").mkdir()
            runner = FakeRunner()
            output: list[str] = []
            code = demo.execute_demo(
                demo.DemoOptions(profile="pty-mqtt", build_dir=None, jobs=2),
                repository_root=root,
                command_runner=runner,
                which=_which,
                platform="linux",
                machine="x86_64",
                emit=output.append,
            )
        self.assertEqual(code, 0)
        self.assertEqual(len(runner.calls), 3)
        configure, build, ctest = [call[0] for call in runner.calls]
        self.assertEqual(configure[:3], ["cmake", "-S", str(root)])
        self.assertIn("-DGATEWAY_ENABLE_MQTT=ON", configure)
        self.assertIn("-DGATEWAY_BUILD_PTY_SLAVE=ON", configure)
        self.assertIn("gateway_mqtt_pty_runtime_integration_test", build)
        self.assertIn("--no-tests=error", ctest)
        self.assertIn(demo.DEMO_TEST_REGEX, ctest)
        rendered = "\n".join(output)
        self.assertIn("DEMO_RESULT=PASS", rendered)
        self.assertIn("HARDWARE_VALIDATED=false", rendered)
        self.assertNotIn("HARDWARE_VALIDATED=true", rendered)

    def test_missing_dependency_returns_three_without_running_commands(self) -> None:
        runner = FakeRunner()
        output: list[str] = []
        code = demo.execute_demo(
            demo.DemoOptions(profile="pty-mqtt", build_dir=None, jobs=2),
            repository_root=pathlib.Path("/tmp/repository"),
            command_runner=runner,
            which=lambda name: None if name == "mosquitto" else f"/usr/bin/{name}",
            platform="linux",
            machine="x86_64",
            emit=output.append,
        )
        self.assertEqual(code, 3)
        self.assertEqual(runner.calls, [])
        self.assertIn("mosquitto", "\n".join(output))

    def test_each_failed_step_returns_four_and_stops(self) -> None:
        for failure_index in range(3):
            with self.subTest(failure_index=failure_index):
                results = [_result([], returncode=0) for _ in range(failure_index)]
                results.append(_result([], returncode=7))
                runner = FakeRunner(results)
                code = demo.execute_demo(
                    demo.DemoOptions(profile="pty-mqtt", build_dir=None, jobs=2),
                    repository_root=pathlib.Path("/tmp/repository"),
                    command_runner=runner,
                    which=_which,
                    platform="linux",
                    machine="x86_64",
                    emit=lambda _line: None,
                )
                self.assertEqual(code, 4)
                self.assertEqual(len(runner.calls), failure_index + 1)

    def test_timeout_returns_five(self) -> None:
        runner = FakeRunner([_result([], timed_out=True, returncode=-9)])
        code = demo.execute_demo(
            demo.DemoOptions(profile="pty-mqtt", build_dir=None, jobs=2),
            repository_root=pathlib.Path("/tmp/repository"),
            command_runner=runner,
            which=_which,
            platform="linux",
            machine="x86_64",
            emit=lambda _line: None,
        )
        self.assertEqual(code, 5)

    def test_platform_architecture_and_escaping_build_directory_are_rejected(self) -> None:
        runner = FakeRunner()
        options = demo.DemoOptions(profile="pty-mqtt", build_dir=None, jobs=2)
        self.assertEqual(
            demo.execute_demo(
                options,
                repository_root=pathlib.Path("/tmp/repository"),
                command_runner=runner,
                which=_which,
                platform="win32",
                machine="x86_64",
                emit=lambda _line: None,
            ),
            3,
        )
        self.assertEqual(
            demo.execute_demo(
                options,
                repository_root=pathlib.Path("/tmp/repository"),
                command_runner=runner,
                which=_which,
                platform="linux",
                machine="aarch64",
                emit=lambda _line: None,
            ),
            3,
        )
        self.assertEqual(
            demo.execute_demo(
                demo.DemoOptions(
                    profile="pty-mqtt", build_dir=pathlib.Path("../escape"), jobs=2
                ),
                repository_root=pathlib.Path("/tmp/repository"),
                command_runner=runner,
                which=_which,
                platform="linux",
                machine="x86_64",
                emit=lambda _line: None,
            ),
            2,
        )
        self.assertEqual(runner.calls, [])

    def test_source_does_not_invoke_soak_or_global_kill(self) -> None:
        source = pathlib.Path(demo.__file__).read_text(encoding="utf-8")
        self.assertNotIn("run_soak", source)
        self.assertNotIn("pkill", source)
        self.assertNotIn("killall", source)
        self.assertNotIn("shell=True", source)

    def test_process_runner_cleans_its_group_on_keyboard_interrupt(self) -> None:
        process = mock.Mock()
        process.pid = 4321
        process.communicate.side_effect = [KeyboardInterrupt(), ("", "")]
        process.wait.return_value = 0
        with mock.patch.object(process_manager.subprocess, "Popen", return_value=process) as popen:
            with mock.patch.object(process_manager.os, "killpg") as killpg:
                with self.assertRaises(KeyboardInterrupt):
                    process_manager.run_process(["demo-child"], timeout_seconds=1.0)
        self.assertTrue(popen.call_args.kwargs["start_new_session"])
        killpg.assert_called_once_with(4321, signal.SIGTERM)


if __name__ == "__main__":
    unittest.main()

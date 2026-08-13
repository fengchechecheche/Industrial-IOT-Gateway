from __future__ import annotations

import json
import pathlib
import tempfile
import unittest
from datetime import UTC, datetime

from tools.fault_matrix.models import ProcessResult
from tools.release.release_runner import (
    CommandStep,
    ReleaseInputError,
    build_wsl_preflight_steps,
    create_run_directory,
    execute_steps,
    parse_arguments,
    prepare_work_root,
    resolve_under,
    validate_release_inputs,
)


REVISION = "4af261498a6c2561c6085a2f1ca57ef68924ea17"


def result(returncode: int = 0, *, timed_out: bool = False) -> ProcessResult:
    return ProcessResult(
        command=["fixture"],
        returncode=returncode,
        stdout="stdout\n",
        stderr="stderr\n" if returncode else "",
        duration_ms=25,
        timed_out=timed_out,
        termination_signal="SIGTERM" if timed_out else None,
    )


class ReleaseRunnerTest(unittest.TestCase):
    def test_default_artifact_root_uses_frozen_version_label(self) -> None:
        arguments = parse_arguments(
            ["--mode", "wsl-preflight", "--source-revision", REVISION]
        )
        self.assertEqual(
            arguments.artifact_root.parts[-3:],
            ("artifacts", "releases", "v0.1.0"),
        )

    def test_only_frozen_version_and_full_revision_are_accepted(self) -> None:
        validate_release_inputs("0.1.0", REVISION, 2)
        for version, revision, jobs in (
            ("0.2.0", REVISION, 2),
            ("0.1.0", "4af2614", 2),
            ("0.1.0", REVISION, 0),
            ("0.1.0", REVISION, 65),
        ):
            with self.subTest(version=version, revision=revision, jobs=jobs):
                with self.assertRaises(ReleaseInputError):
                    validate_release_inputs(version, revision, jobs)

    def test_task_paths_must_remain_below_approved_root(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            self.assertEqual(resolve_under(root, root / "child"), (root / "child").resolve())
            with self.assertRaises(ReleaseInputError):
                resolve_under(root, root.parent / "escape")

    def test_run_directory_is_unique_and_never_overwritten(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            now = datetime(2026, 8, 13, 12, 0, 0, tzinfo=UTC)
            first = create_run_directory(root, REVISION, now=now)
            second = create_run_directory(root, REVISION, now=now)
            self.assertNotEqual(first, second)
            self.assertTrue((first / "IN_PROGRESS").is_file())
            self.assertTrue((second / "IN_PROGRESS").is_file())

    def test_success_writes_commands_summary_and_pass_marker(self) -> None:
        calls: list[tuple[list[str], dict[str, object]]] = []

        def runner(command: list[str], **kwargs: object) -> ProcessResult:
            calls.append((command, kwargs))
            return result()

        with tempfile.TemporaryDirectory() as directory:
            evidence = pathlib.Path(directory)
            rc = execute_steps(
                [CommandStep("configure", ["cmake", "-S", "."], 10.0)],
                evidence_dir=evidence,
                cwd=evidence,
                command_runner=runner,
            )
            summary = json.loads((evidence / "summary.json").read_text(encoding="utf-8"))
            command_record = json.loads(
                (evidence / "commands.jsonl").read_text(encoding="utf-8").strip()
            )
            self.assertEqual(rc, 0)
            self.assertEqual(summary["status"], "PASS")
            self.assertTrue((evidence / "PASS").is_file())
            self.assertFalse((evidence / "IN_PROGRESS").exists())
            self.assertEqual(command_record["command"], ["cmake", "-S", "."])
            self.assertEqual(calls[0][1]["timeout_seconds"], 10.0)

    def test_failure_is_nonzero_and_preserves_failure_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            evidence = pathlib.Path(directory)
            rc = execute_steps(
                [CommandStep("build", ["cmake", "--build", "build"], 10.0)],
                evidence_dir=evidence,
                cwd=evidence,
                command_runner=lambda *_args, **_kwargs: result(7),
            )
            failures = json.loads((evidence / "failures.json").read_text(encoding="utf-8"))
            self.assertEqual(rc, 4)
            self.assertEqual(failures[0]["step"], "build")
            self.assertEqual(failures[0]["returncode"], 7)
            self.assertFalse((evidence / "PASS").exists())

    def test_timeout_has_distinct_exit_code(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            evidence = pathlib.Path(directory)
            rc = execute_steps(
                [CommandStep("test", ["ctest"], 1.0)],
                evidence_dir=evidence,
                cwd=evidence,
                command_runner=lambda *_args, **_kwargs: result(-15, timed_out=True),
            )
            self.assertEqual(rc, 5)
            summary = json.loads((evidence / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["status"], "TIMEOUT")

    def test_commands_are_argument_arrays_without_shell_metacharacter_expansion(self) -> None:
        with self.assertRaises(ReleaseInputError):
            CommandStep("unsafe", ["bash", "-lc", "rm -rf /"], 1.0)
        with self.assertRaises(ReleaseInputError):
            CommandStep("unsafe", ["echo", "ok;", "touch", "bad"], 1.0)

    def test_source_contains_no_global_process_kill(self) -> None:
        source = pathlib.Path(
            __import__("tools.release.release_runner", fromlist=["__file__"]).__file__
        ).read_text(encoding="utf-8")
        self.assertNotIn("pkill", source)
        self.assertNotIn("killall", source)
        self.assertNotIn("shell=True", source)

    def test_preflight_plan_contains_all_required_development_gates(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repository_root = pathlib.Path(directory)
            work_root = repository_root / "build" / "t04-preflight"
            steps = build_wsl_preflight_steps(repository_root, work_root, jobs=2)
            labels = {step.label for step in steps}
            for required in (
                "debug_configure",
                "debug_test",
                "asan_test",
                "release_test",
                "tsan_test",
                "mqtt_off_test",
                "tidy_build",
                "clang_format",
                "python_compile",
                "demo",
                "install",
                "systemd_verify",
            ):
                self.assertIn(required, labels)
            encoded = json.dumps([step.command for step in steps])
            self.assertIn("GATEWAY_ENABLE_MQTT=ON", encoded)
            self.assertIn("GATEWAY_ENABLE_MQTT=OFF", encoded)
            self.assertIn("--recursive-errors=no", encoded)
            self.assertIn(f"--root={work_root / 'stage'}", encoded)
            self.assertIn(
                str(
                    work_root
                    / "stage"
                    / "usr"
                    / "local"
                    / "lib"
                    / "systemd"
                    / "system"
                    / "industrial_iot_gateway.service"
                ),
                encoded,
            )
            self.assertNotIn("184/184", encoded)

    def test_preflight_work_root_must_be_new_and_below_build(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repository_root = pathlib.Path(directory)
            work_root = repository_root / "build" / "t04-preflight"
            prepared = prepare_work_root(repository_root, work_root)
            self.assertEqual(prepared, work_root.resolve())
            with self.assertRaisesRegex(ReleaseInputError, "already exists"):
                prepare_work_root(repository_root, work_root)
            with self.assertRaises(ReleaseInputError):
                prepare_work_root(repository_root, repository_root.parent / "outside")


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
from __future__ import annotations

import argparse
import dataclasses
import pathlib
import platform as platform_module
import shutil
import sys
from collections.abc import Callable, Sequence


REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[1]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from tools.fault_matrix.models import ProcessResult  # noqa: E402
from tools.fault_matrix.process_manager import run_process  # noqa: E402


DEMO_TEST_REGEX = (
    r"^MqttPtyRuntimeIntegrationTest\."
    r"SerialPollingContinuesWhileBrokerIsUnavailable$"
)


@dataclasses.dataclass(frozen=True)
class DemoOptions:
    profile: str
    build_dir: pathlib.Path | None
    jobs: int


CommandRunner = Callable[..., ProcessResult]
Emitter = Callable[[str], None]


def parse_arguments(argv: Sequence[str] | None = None) -> DemoOptions:
    parser = argparse.ArgumentParser(
        description="Run the Linux x86_64 PTY and MQTT software-only demonstration"
    )
    parser.add_argument("--profile", required=True, choices=("pty-mqtt",))
    parser.add_argument("--build-dir", type=pathlib.Path)
    parser.add_argument("--jobs", type=int, default=2)
    arguments = parser.parse_args(argv)
    return DemoOptions(
        profile=arguments.profile,
        build_dir=arguments.build_dir,
        jobs=arguments.jobs,
    )


def _inside(path: pathlib.Path, root: pathlib.Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def _build_directory(repository_root: pathlib.Path, value: pathlib.Path | None) -> pathlib.Path:
    build_root = (repository_root / "build").resolve()
    candidate = build_root / "demo-pty-mqtt" if value is None else value
    if not candidate.is_absolute():
        candidate = repository_root / candidate
    resolved = candidate.resolve()
    if not _inside(resolved, build_root):
        raise ValueError("build directory must remain under the repository build/ directory")
    return resolved


def _emit_process_result(label: str, result: ProcessResult, emit: Emitter) -> None:
    emit(f"DEMO_STEP={label} RETURN_CODE={result.returncode} DURATION_MS={result.duration_ms}")
    if result.stdout:
        emit(result.stdout.rstrip())
    if result.stderr:
        emit(result.stderr.rstrip())


def execute_demo(
    options: DemoOptions,
    *,
    repository_root: pathlib.Path = REPOSITORY_ROOT,
    command_runner: CommandRunner = run_process,
    which: Callable[[str], str | None] = shutil.which,
    platform: str = sys.platform,
    machine: str = platform_module.machine(),
    emit: Emitter = print,
) -> int:
    emit(f"DEMO_PROFILE={options.profile}")
    emit("DEMO_SCOPE=Linux_x86_64_PTY_MQTT_software_only")
    emit("HARDWARE_VALIDATED=false")
    if not platform.startswith("linux"):
        emit("DEMO_ERROR=unsupported_platform")
        emit("DEMO_RESULT=FAIL")
        return 3
    if machine.lower() not in {"x86_64", "amd64"}:
        emit(f"DEMO_ERROR=unsupported_architecture:{machine}")
        emit("DEMO_RESULT=FAIL")
        return 3
    if options.jobs < 1 or options.jobs > 64:
        emit("DEMO_ERROR=jobs_must_be_between_1_and_64")
        emit("DEMO_RESULT=FAIL")
        return 2
    missing = [name for name in ("cmake", "ctest", "c++", "mosquitto") if which(name) is None]
    if missing:
        emit("DEMO_ERROR=missing_dependencies:" + ",".join(missing))
        emit("DEMO_RESULT=FAIL")
        return 3
    try:
        build_dir = _build_directory(repository_root, options.build_dir)
    except ValueError as error:
        emit(f"DEMO_ERROR={error}")
        emit("DEMO_RESULT=FAIL")
        return 2

    commands = (
        (
            "configure",
            [
                "cmake",
                "-S",
                str(repository_root),
                "-B",
                str(build_dir),
                "-DCMAKE_BUILD_TYPE=Release",
                "-DGATEWAY_BUILD_TESTS=ON",
                "-DGATEWAY_BUILD_PTY_SLAVE=ON",
                "-DGATEWAY_ENABLE_MQTT=ON",
                "-DGATEWAY_ENABLE_WARNINGS_AS_ERRORS=ON",
            ],
            60.0,
        ),
        (
            "build",
            [
                "cmake",
                "--build",
                str(build_dir),
                "--target",
                "gateway_mqtt_pty_runtime_integration_test",
                "--parallel",
                str(options.jobs),
            ],
            180.0,
        ),
        (
            "test",
            [
                "ctest",
                "--test-dir",
                str(build_dir),
                "--output-on-failure",
                "--no-tests=error",
                "-R",
                DEMO_TEST_REGEX,
            ],
            45.0,
        ),
    )
    try:
        for label, command, timeout_seconds in commands:
            result = command_runner(
                command,
                timeout_seconds=timeout_seconds,
                cwd=str(repository_root),
            )
            _emit_process_result(label, result, emit)
            if result.timed_out:
                emit(f"DEMO_ERROR={label}_timeout")
                emit("DEMO_RESULT=TIMEOUT")
                return 5
            if result.returncode != 0:
                emit(f"DEMO_ERROR={label}_failed")
                emit("DEMO_RESULT=FAIL")
                return 4
    except KeyboardInterrupt:
        emit("DEMO_ERROR=interrupted")
        emit("DEMO_RESULT=INTERRUPTED")
        return 5
    except OSError as error:
        emit(f"DEMO_ERROR=process_start_failed:{error}")
        emit("DEMO_RESULT=FAIL")
        return 4

    emit("DEMO_RESULT=PASS")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    return execute_demo(parse_arguments(argv))


if __name__ == "__main__":
    raise SystemExit(main())

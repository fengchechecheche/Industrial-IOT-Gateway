#!/usr/bin/env python3
from __future__ import annotations

import argparse
import dataclasses
import json
import pathlib
import re
import sys
from collections.abc import Callable, Sequence
from datetime import UTC, datetime
from typing import Any

REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from tools.fault_matrix.models import ProcessResult  # noqa: E402
from tools.fault_matrix.process_manager import run_process
from tools.release.manifest import (
    ManifestError,
    build_manifest,
    scan_release_tree,
    validate_source_revision,
    write_manifest,
    write_sha256sums,
)
from tools.release.systemd_runner import (
    classify_environment,
    collect_environment_identity,
    evaluate_g5,
)


FROZEN_VERSION = "0.1.0"
SAFE_LABEL = re.compile(r"^[a-z0-9][a-z0-9_-]*$")
UNSAFE_TOKENS = (";", "&&", "||", "`", "$(", "\n", "\r")


class ReleaseInputError(ValueError):
    """Raised when a release runner input crosses the frozen safety contract."""


@dataclasses.dataclass(frozen=True)
class CommandStep:
    label: str
    command: list[str]
    timeout_seconds: float

    def __post_init__(self) -> None:
        if SAFE_LABEL.fullmatch(self.label) is None:
            raise ReleaseInputError(f"unsafe step label: {self.label}")
        if not self.command or not all(isinstance(item, str) and item for item in self.command):
            raise ReleaseInputError("command must be a non-empty argument array")
        if self.timeout_seconds <= 0:
            raise ReleaseInputError("step timeout must be positive")
        lowered = [item.lower() for item in self.command]
        if lowered[0] in {"bash", "sh", "zsh"} and any("c" in item for item in lowered[1:2]):
            raise ReleaseInputError("shell command strings are not permitted")
        if any(token in item for item in self.command for token in UNSAFE_TOKENS):
            raise ReleaseInputError("shell metacharacters are not permitted in command arguments")


CommandRunner = Callable[..., ProcessResult]


def validate_release_inputs(version: str, source_revision: str, jobs: int) -> None:
    if version != FROZEN_VERSION:
        raise ReleaseInputError(f"only frozen version {FROZEN_VERSION} is accepted")
    try:
        validate_source_revision(source_revision)
    except ManifestError as error:
        raise ReleaseInputError(str(error)) from error
    if jobs < 1 or jobs > 64:
        raise ReleaseInputError("jobs must be between 1 and 64")


def resolve_under(root: pathlib.Path, candidate: pathlib.Path) -> pathlib.Path:
    resolved_root = root.resolve()
    resolved_candidate = candidate.resolve()
    try:
        resolved_candidate.relative_to(resolved_root)
    except ValueError as error:
        raise ReleaseInputError(f"path must remain below {resolved_root}") from error
    return resolved_candidate


def prepare_work_root(repository_root: pathlib.Path, candidate: pathlib.Path) -> pathlib.Path:
    build_root = (repository_root / "build").resolve()
    resolved = resolve_under(build_root, candidate)
    if resolved.exists():
        raise ReleaseInputError(f"preflight work root already exists: {resolved}")
    resolved.mkdir(parents=True)
    return resolved


def create_run_directory(
    artifact_root: pathlib.Path,
    source_revision: str,
    *,
    now: datetime | None = None,
) -> pathlib.Path:
    validate_source_revision(source_revision)
    root = artifact_root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    current = now or datetime.now(UTC)
    stem = f"{current.strftime('%Y%m%dT%H%M%SZ')}_{source_revision[:7]}"
    for sequence in range(1, 1000):
        candidate = root / f"{stem}_{sequence:03d}"
        try:
            candidate.mkdir()
        except FileExistsError:
            continue
        (candidate / "IN_PROGRESS").write_text("in_progress\n", encoding="utf-8")
        return candidate
    raise ReleaseInputError("could not allocate a unique release run directory")


def _write_json(path: pathlib.Path, value: Any) -> None:
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def _record_command(path: pathlib.Path, record: dict[str, Any]) -> None:
    with path.open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(record, ensure_ascii=False, sort_keys=True) + "\n")


def execute_steps(
    steps: Sequence[CommandStep],
    *,
    evidence_dir: pathlib.Path,
    cwd: pathlib.Path,
    command_runner: CommandRunner = run_process,
) -> int:
    evidence_dir.mkdir(parents=True, exist_ok=True)
    in_progress = evidence_dir / "IN_PROGRESS"
    if not in_progress.exists():
        in_progress.write_text("in_progress\n", encoding="utf-8")
    command_log = evidence_dir / "commands.jsonl"
    failures: list[dict[str, Any]] = []
    completed: list[dict[str, Any]] = []
    status = "PASS"
    exit_code = 0

    try:
        for step in steps:
            started = datetime.now(UTC).isoformat()
            result = command_runner(
                step.command,
                timeout_seconds=step.timeout_seconds,
                cwd=str(cwd),
            )
            record = {
                "step": step.label,
                "command": step.command,
                "started_utc": started,
                "duration_ms": result.duration_ms,
                "returncode": result.returncode,
                "timed_out": result.timed_out,
                "termination_signal": result.termination_signal,
            }
            _record_command(command_log, record)
            (evidence_dir / f"{step.label}.stdout.txt").write_text(
                result.stdout, encoding="utf-8"
            )
            (evidence_dir / f"{step.label}.stderr.txt").write_text(
                result.stderr, encoding="utf-8"
            )
            completed.append(record)
            if result.timed_out:
                failures.append(record)
                status = "TIMEOUT"
                exit_code = 5
                break
            if result.returncode != 0:
                failures.append(record)
                status = "FAIL"
                exit_code = 4
                break
    except KeyboardInterrupt:
        status = "INTERRUPTED"
        exit_code = 5
        failures.append({"step": "runner", "reason": "interrupted"})
    except OSError as error:
        status = "FAIL"
        exit_code = 4
        failures.append({"step": "runner", "reason": f"process_start_failed:{error}"})

    _write_json(evidence_dir / "failures.json", failures)
    _write_json(
        evidence_dir / "summary.json",
        {"status": status, "completed_steps": completed, "failure_count": len(failures)},
    )
    if status == "PASS":
        in_progress.unlink(missing_ok=True)
        (evidence_dir / "PASS").write_text("pass\n", encoding="utf-8")
    return exit_code


def _configure_command(
    repository_root: pathlib.Path,
    build_dir: pathlib.Path,
    *,
    build_type: str,
    mqtt: bool,
    extra: Sequence[str] = (),
) -> list[str]:
    return [
        "cmake",
        "-S",
        str(repository_root),
        "-B",
        str(build_dir),
        "-G",
        "Ninja",
        f"-DCMAKE_BUILD_TYPE={build_type}",
        "-DGATEWAY_BUILD_TESTS=ON",
        "-DGATEWAY_BUILD_PTY_SLAVE=ON",
        f"-DGATEWAY_ENABLE_MQTT={'ON' if mqtt else 'OFF'}",
        "-DGATEWAY_ENABLE_WARNINGS_AS_ERRORS=ON",
        *extra,
    ]


def build_wsl_preflight_steps(
    repository_root: pathlib.Path, work_root: pathlib.Path, *, jobs: int
) -> list[CommandStep]:
    validate_release_inputs(FROZEN_VERSION, "0" * 40, jobs)
    repository_root = repository_root.resolve()
    work_root = resolve_under((repository_root / "build").resolve(), work_root)
    steps: list[CommandStep] = []
    configurations = (
        ("debug", "Debug", True, ()),
        ("asan", "Debug", True, ("-DGATEWAY_ENABLE_SANITIZERS=ON",)),
        ("release", "Release", True, ()),
        (
            "tsan",
            "Debug",
            True,
            (
                "-DGATEWAY_ENABLE_TSAN=ON",
                "-DGATEWAY_ENABLE_WSL_TSAN_WORKAROUND=ON",
            ),
        ),
        ("mqtt_off", "Release", False, ()),
    )
    for label, build_type, mqtt, extra in configurations:
        build_dir = work_root / label
        steps.extend(
            (
                CommandStep(
                    f"{label}_configure",
                    _configure_command(
                        repository_root,
                        build_dir,
                        build_type=build_type,
                        mqtt=mqtt,
                        extra=extra,
                    ),
                    120.0,
                ),
                CommandStep(
                    f"{label}_build",
                    ["cmake", "--build", str(build_dir), "--parallel", str(jobs)],
                    900.0,
                ),
                CommandStep(
                    f"{label}_test",
                    [
                        "ctest",
                        "--test-dir",
                        str(build_dir),
                        "--output-on-failure",
                        "--no-tests=error",
                    ],
                    900.0,
                ),
            )
        )

    tidy_dir = work_root / "tidy"
    steps.extend(
        (
            CommandStep(
                "tidy_configure",
                _configure_command(
                    repository_root,
                    tidy_dir,
                    build_type="Debug",
                    mqtt=True,
                    extra=(
                        "-DCMAKE_CXX_COMPILER=clang++-18",
                        "-DGATEWAY_ENABLE_CLANG_TIDY=ON",
                    ),
                ),
                120.0,
            ),
            CommandStep(
                "tidy_build",
                ["cmake", "--build", str(tidy_dir), "--parallel", str(jobs)],
                1200.0,
            ),
        )
    )

    cpp_files = sorted(
        str(path)
        for top in ("include", "src", "tests", "tools")
        for path in (repository_root / top).rglob("*")
        if path.is_file() and path.suffix in {".cpp", ".hpp", ".cc", ".hh", ".cxx", ".hxx"}
    )
    python_files = sorted(
        str(path)
        for top in ("tools", "tests")
        for path in (repository_root / top).rglob("*.py")
        if path.is_file()
    )
    steps.extend(
        (
            CommandStep("clang_format", ["clang-format-18", "--dry-run", "--Werror", *cpp_files], 180.0),
            CommandStep("python_compile", ["python3", "-m", "py_compile", *python_files], 180.0),
            CommandStep(
                "demo",
                [
                    "python3",
                    str(repository_root / "tools" / "demo.py"),
                    "--profile",
                    "pty-mqtt",
                    "--build-dir",
                    str(work_root / "demo"),
                    "--jobs",
                    str(jobs),
                ],
                360.0,
            ),
            CommandStep(
                "install",
                [
                    "cmake",
                    "--install",
                    str(work_root / "release"),
                    "--prefix",
                    str(work_root / "stage" / "usr" / "local"),
                ],
                180.0,
            ),
            CommandStep(
                "systemd_verify",
                [
                    "systemd-analyze",
                    "verify",
                    "--recursive-errors=no",
                    f"--root={work_root / 'stage'}",
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
                ],
                60.0,
            ),
        )
    )
    return steps


def _mark_post_step_failure(evidence_dir: pathlib.Path, reason: str) -> None:
    (evidence_dir / "PASS").unlink(missing_ok=True)
    failures_path = evidence_dir / "failures.json"
    failures = json.loads(failures_path.read_text(encoding="utf-8"))
    failures.append({"step": "post_validation", "reason": reason})
    _write_json(failures_path, failures)
    summary_path = evidence_dir / "summary.json"
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    summary["status"] = "FAIL"
    summary["failure_count"] = len(failures)
    _write_json(summary_path, summary)


def run_wsl_preflight(
    *,
    repository_root: pathlib.Path,
    source_revision: str,
    artifact_root: pathlib.Path,
    work_root: pathlib.Path,
    jobs: int,
) -> int:
    validate_release_inputs(FROZEN_VERSION, source_revision, jobs)
    repository_root = repository_root.resolve()
    allowed_artifacts = (repository_root / "artifacts" / "releases").resolve()
    artifact_root = resolve_under(allowed_artifacts, artifact_root)

    revision_result = run_process(
        ["git", "rev-parse", "HEAD"], timeout_seconds=10.0, cwd=str(repository_root)
    )
    if revision_result.returncode != 0 or revision_result.stdout.strip() != source_revision:
        raise ReleaseInputError("source revision does not match the committed HEAD")

    prepared_work_root = prepare_work_root(repository_root, work_root)
    evidence_dir = create_run_directory(artifact_root, source_revision)
    identity = collect_environment_identity()
    environment_class = classify_environment(identity)
    environment = {
        **dataclasses.asdict(identity),
        "environment_class": environment_class,
        "evidence_class": "DEVELOPMENT_ONLY",
        "release_candidate_bound": False,
    }
    _write_json(evidence_dir / "environment.json", environment)
    (evidence_dir / "source_revision.txt").write_text(source_revision + "\n", encoding="utf-8")
    if environment_class != "DEVELOPMENT_ONLY":
        _mark_post_step_failure(evidence_dir, f"expected WSL development environment, got {environment_class}")
        return 3

    steps = build_wsl_preflight_steps(repository_root, prepared_work_root, jobs=jobs)
    exit_code = execute_steps(
        steps,
        evidence_dir=evidence_dir,
        cwd=repository_root,
    )
    if exit_code != 0:
        print(f"T04_PREFLIGHT=FAIL EVIDENCE_DIR={evidence_dir}")
        return exit_code

    try:
        stage_root = prepared_work_root / "stage"
        gates = {
            "G1": "DEVELOPMENT_PASS",
            "G2": "DEVELOPMENT_PASS",
            "G3": "DEVELOPMENT_PASS",
            "G4": "INHERITANCE_NOT_YET_FROZEN",
            "G5": "DEVELOPMENT_ONLY",
            "G6": "WAITING_FOR_HARDWARE",
        }
        manifest = build_manifest(
            stage_root,
            release_name="industrial_iot_gateway-0.1.0-linux-x86_64-preflight",
            source_revision=source_revision,
            environment=environment,
            gates=gates,
        )
        install_dir = evidence_dir / "install"
        manifest_path = install_dir / "install_manifest.json"
        write_manifest(manifest, manifest_path)
        findings = scan_release_tree(stage_root)
        _write_json(install_dir / "sensitive_scan.json", findings)
        if findings:
            raise ManifestError(f"release tree sensitive scan found {len(findings)} blocker(s)")
        write_sha256sums(
            [manifest_path, install_dir / "sensitive_scan.json"],
            root=evidence_dir,
            output=install_dir / "SHA256SUMS",
        )
        g5_summary = evaluate_g5({}, environment_class=environment_class)
        systemd_dir = evidence_dir / "systemd"
        systemd_dir.mkdir(parents=True, exist_ok=True)
        _write_json(systemd_dir / "summary.json", g5_summary)
        summary_path = evidence_dir / "summary.json"
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        summary.update(
            {
                "evidence_class": "DEVELOPMENT_ONLY",
                "gates": gates,
                "hardware_validated": False,
                "arm64_validated": False,
            }
        )
        _write_json(summary_path, summary)
    except (ManifestError, OSError, ValueError) as error:
        _mark_post_step_failure(evidence_dir, str(error))
        print(f"T04_PREFLIGHT=FAIL EVIDENCE_DIR={evidence_dir}")
        return 4

    print(f"T04_PREFLIGHT=PASS EVIDENCE_DIR={evidence_dir}")
    print("G5=DEVELOPMENT_ONLY")
    print("HARDWARE_VALIDATED=false")
    return 0


def parse_arguments(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the P3-S7-T04 release preflight")
    parser.add_argument("--mode", choices=("wsl-preflight",), required=True)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--version", default=FROZEN_VERSION)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument(
        "--artifact-root",
        type=pathlib.Path,
        default=REPOSITORY_ROOT / "artifacts" / "releases" / f"v{FROZEN_VERSION}",
    )
    parser.add_argument(
        "--work-root",
        type=pathlib.Path,
        default=REPOSITORY_ROOT / "build" / "t04-preflight",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parse_arguments(argv)
    try:
        validate_release_inputs(arguments.version, arguments.source_revision, arguments.jobs)
        return run_wsl_preflight(
            repository_root=REPOSITORY_ROOT,
            source_revision=arguments.source_revision,
            artifact_root=arguments.artifact_root,
            work_root=arguments.work_root,
            jobs=arguments.jobs,
        )
    except ReleaseInputError as error:
        print(f"T04_PREFLIGHT_ERROR={error}")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

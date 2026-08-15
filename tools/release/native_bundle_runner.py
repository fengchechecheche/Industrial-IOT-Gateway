#!/usr/bin/env python3
from __future__ import annotations

import argparse
import dataclasses
import gzip
import json
import os
import pathlib
import platform as platform_module
import select
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tarfile
import time
from collections.abc import Sequence
from datetime import UTC, datetime
from typing import Any

REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from tools.fault_matrix.process_manager import run_process  # noqa: E402
from tools.release.manifest import (  # noqa: E402
    REQUIRED_PACKAGE_PATHS,
    ManifestError,
    build_manifest_v2,
    scan_release_tree,
    sha256_file,
    validate_source_revision,
    write_manifest,
    write_sha256sums,
)
from tools.release.release_runner import (  # noqa: E402
    CommandStep,
    ReleaseInputError,
    create_run_directory,
    execute_steps,
    resolve_under,
)
from tools.release.systemd_runner import (  # noqa: E402
    collect_environment_identity,
    platform_for_machine,
)


FROZEN_VERSION = "0.1.0"
TARGET_ARCHITECTURES = {
    "linux-x86_64": (frozenset({"x86_64", "amd64"}), "x86_64", "X86-64"),
    "linux-arm64": (frozenset({"aarch64", "arm64"}), "arm64", "AArch64"),
}
PASS_MARKER = "PASS_P3_S7_ARM64_T03_REPRODUCIBLE_BUNDLE"


class NativeBundleInputError(ValueError):
    """Raised when the native bundle request violates the frozen release contract."""


@dataclasses.dataclass(frozen=True)
class PlatformContract:
    target_platform: str
    machine: str
    package_architecture: str
    readelf_machine: str


def platform_contract(target_platform: str, machine: str) -> PlatformContract:
    contract = TARGET_ARCHITECTURES.get(target_platform)
    if contract is None:
        raise NativeBundleInputError(f"unsupported target platform: {target_platform}")
    aliases, package_architecture, readelf_machine = contract
    normalized_machine = machine.lower()
    if normalized_machine not in aliases:
        raise NativeBundleInputError(
            f"target platform {target_platform} does not match machine {machine}"
        )
    return PlatformContract(
        target_platform=target_platform,
        machine=machine,
        package_architecture=package_architecture,
        readelf_machine=readelf_machine,
    )


def validate_native_platform(
    target_platform: str,
    *,
    machine: str | None = None,
    system: str | None = None,
) -> PlatformContract:
    actual_system = system or platform_module.system()
    if actual_system.lower() != "linux":
        raise NativeBundleInputError(f"native bundle requires Linux, got {actual_system}")
    return platform_contract(target_platform, machine or platform_module.machine())


def package_filename(version: str, target_platform: str) -> str:
    if version != FROZEN_VERSION:
        raise NativeBundleInputError(f"only frozen version {FROZEN_VERSION} is accepted")
    if target_platform not in TARGET_ARCHITECTURES:
        raise NativeBundleInputError(f"unsupported target platform: {target_platform}")
    return f"industrial_iot_gateway-{version}-{target_platform}.tar.gz"


def validate_repository_state(
    repository_root: pathlib.Path,
    source_revision: str,
    *,
    command_runner: Any = run_process,
) -> None:
    revision = command_runner(
        ["git", "rev-parse", "HEAD"], timeout_seconds=10.0, cwd=str(repository_root)
    )
    if revision.returncode != 0 or revision.stdout.strip() != source_revision:
        raise NativeBundleInputError("source revision does not match the committed HEAD")
    status = command_runner(
        ["git", "status", "--porcelain", "--untracked-files=all"],
        timeout_seconds=10.0,
        cwd=str(repository_root),
    )
    if status.returncode != 0:
        raise NativeBundleInputError("could not verify the repository worktree state")
    if status.stdout.strip():
        raise NativeBundleInputError("formal native bundle requires a clean committed worktree")


def _validate_archive_root(value: str) -> str:
    path = pathlib.PurePosixPath(value)
    if not value or path.is_absolute() or ".." in path.parts or len(path.parts) != 1:
        raise NativeBundleInputError(f"unsafe archive root: {value!r}")
    return value


def _ensure_archive_output_outside_tree(root: pathlib.Path, output: pathlib.Path) -> None:
    try:
        output.resolve().relative_to(root.resolve())
    except ValueError:
        return
    raise NativeBundleInputError("archive output must remain outside package root")


def _tar_info(name: str, *, mode: int, entry_type: bytes) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.type = entry_type
    info.mode = mode
    info.uid = 0
    info.gid = 0
    info.uname = "root"
    info.gname = "root"
    info.mtime = 0
    info.pax_headers = {}
    return info


def _archive_symlink_target(path: pathlib.Path, root: pathlib.Path) -> str:
    target = os.readlink(path)
    resolved = (path.parent / target).resolve()
    try:
        resolved.relative_to(root.resolve())
    except ValueError as error:
        raise NativeBundleInputError(f"symlink escapes package root: {path}") from error
    return target


def create_deterministic_archive(
    package_root: pathlib.Path,
    output: pathlib.Path,
    *,
    archive_root: str,
) -> pathlib.Path:
    root = package_root.resolve()
    if not root.is_dir():
        raise NativeBundleInputError(f"package root is not a directory: {package_root}")
    _ensure_archive_output_outside_tree(root, output)
    archive_root = _validate_archive_root(archive_root)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".tmp")
    temporary.unlink(missing_ok=True)
    try:
        with temporary.open("wb") as raw:
            with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
                with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
                    root_info = _tar_info(archive_root, mode=0o755, entry_type=tarfile.DIRTYPE)
                    archive.addfile(root_info)
                    paths = sorted(
                        root.rglob("*"), key=lambda item: item.relative_to(root).as_posix()
                    )
                    for path in paths:
                        relative = path.relative_to(root).as_posix()
                        name = f"{archive_root}/{relative}"
                        if path.is_symlink():
                            info = _tar_info(name, mode=0o777, entry_type=tarfile.SYMTYPE)
                            info.linkname = _archive_symlink_target(path, root)
                            archive.addfile(info)
                        elif path.is_dir():
                            archive.addfile(
                                _tar_info(name, mode=0o755, entry_type=tarfile.DIRTYPE)
                            )
                        elif path.is_file():
                            executable = bool(path.stat().st_mode & stat.S_IXUSR)
                            info = _tar_info(
                                name,
                                mode=0o755 if executable else 0o644,
                                entry_type=tarfile.REGTYPE,
                            )
                            info.size = path.stat().st_size
                            with path.open("rb") as stream:
                                archive.addfile(info, stream)
        temporary.replace(output)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise
    return output


def _safe_extract(archive_path: pathlib.Path, destination: pathlib.Path) -> pathlib.Path:
    if destination.exists():
        raise NativeBundleInputError(f"clean extraction directory already exists: {destination}")
    destination.mkdir(parents=True)
    root = destination.resolve()
    with tarfile.open(archive_path, mode="r:gz") as archive:
        for member in archive.getmembers():
            member_path = pathlib.PurePosixPath(member.name)
            if member_path.is_absolute() or ".." in member_path.parts:
                raise NativeBundleInputError(f"unsafe archive member: {member.name}")
            resolved = (root / member.name).resolve()
            try:
                resolved.relative_to(root)
            except ValueError as error:
                raise NativeBundleInputError(f"archive member escapes destination: {member.name}") from error
            if member.issym() or member.islnk():
                link_target = (resolved.parent / member.linkname).resolve()
                try:
                    link_target.relative_to(root)
                except ValueError as error:
                    raise NativeBundleInputError(
                        f"archive link escapes destination: {member.name}"
                    ) from error
        archive.extractall(destination, filter="data")
    roots = [path for path in destination.iterdir() if path.is_dir()]
    if len(roots) != 1:
        raise NativeBundleInputError("archive must contain exactly one top-level directory")
    return roots[0]


def build_native_bundle_steps(
    repository_root: pathlib.Path,
    work_root: pathlib.Path,
    *,
    jobs: int,
) -> list[CommandStep]:
    if jobs < 1 or jobs > 64:
        raise NativeBundleInputError("jobs must be between 1 and 64")
    repository_root = repository_root.resolve()
    build_dir = work_root / "release"
    stage_a = work_root / "stage-a"
    stage_b = work_root / "stage-b"
    unit_relative = pathlib.Path(
        "usr/local/lib/systemd/system/industrial_iot_gateway.service"
    )
    configure = [
        "cmake",
        "-S",
        str(repository_root),
        "-B",
        str(build_dir),
        "-G",
        "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DGATEWAY_BUILD_TESTS=ON",
        "-DGATEWAY_BUILD_PTY_SLAVE=ON",
        "-DGATEWAY_ENABLE_MQTT=ON",
        "-DGATEWAY_ENABLE_WARNINGS_AS_ERRORS=ON",
    ]
    return [
        CommandStep("release_configure", configure, 180.0),
        CommandStep(
            "release_build",
            ["cmake", "--build", str(build_dir), "--parallel", str(jobs)],
            1800.0,
        ),
        CommandStep(
            "release_test",
            [
                "ctest",
                "--test-dir",
                str(build_dir),
                "--output-on-failure",
                "--no-tests=error",
            ],
            1800.0,
        ),
        CommandStep(
            "install_a",
            ["cmake", "--install", str(build_dir), "--prefix", str(stage_a / "usr/local")],
            180.0,
        ),
        CommandStep(
            "install_b",
            ["cmake", "--install", str(build_dir), "--prefix", str(stage_b / "usr/local")],
            180.0,
        ),
        CommandStep(
            "systemd_verify_a",
            [
                "systemd-analyze",
                "verify",
                "--recursive-errors=no",
                f"--root={stage_a}",
                str(stage_a / unit_relative),
            ],
            60.0,
        ),
        CommandStep(
            "systemd_verify_b",
            [
                "systemd-analyze",
                "verify",
                "--recursive-errors=no",
                f"--root={stage_b}",
                str(stage_b / unit_relative),
            ],
            60.0,
        ),
    ]


def _write_json(path: pathlib.Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def _post_failure(evidence_dir: pathlib.Path, reason: str) -> None:
    (evidence_dir / "PASS").unlink(missing_ok=True)
    failures_path = evidence_dir / "failures.json"
    failures = json.loads(failures_path.read_text(encoding="utf-8"))
    failures.append({"step": "post_validation", "reason": reason})
    _write_json(failures_path, failures)
    summary_path = evidence_dir / "summary.json"
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    summary.update({"status": "FAIL", "failure_count": len(failures)})
    _write_json(summary_path, summary)


def _run_checked(
    command: list[str],
    *,
    cwd: pathlib.Path,
    evidence_dir: pathlib.Path,
    label: str,
    timeout_seconds: float = 30.0,
) -> str:
    result = run_process(command, timeout_seconds=timeout_seconds, cwd=str(cwd))
    (evidence_dir / f"{label}.stdout.txt").write_text(result.stdout, encoding="utf-8")
    (evidence_dir / f"{label}.stderr.txt").write_text(result.stderr, encoding="utf-8")
    if result.timed_out or result.returncode != 0:
        raise NativeBundleInputError(
            f"{label} failed: returncode={result.returncode} timed_out={result.timed_out}"
        )
    return result.stdout


def _validate_binary(
    binary: pathlib.Path,
    *,
    contract: PlatformContract,
    cwd: pathlib.Path,
    evidence_dir: pathlib.Path,
    label: str,
) -> None:
    header = _run_checked(
        ["readelf", "-h", str(binary)],
        cwd=cwd,
        evidence_dir=evidence_dir,
        label=f"{label}_readelf",
    )
    if contract.readelf_machine not in header:
        raise NativeBundleInputError(
            f"{label} ELF machine mismatch: expected {contract.readelf_machine}"
        )
    dependencies = _run_checked(
        ["ldd", str(binary)],
        cwd=cwd,
        evidence_dir=evidence_dir,
        label=f"{label}_ldd",
    )
    if "not found" in dependencies.lower():
        raise NativeBundleInputError(f"{label} has unresolved dynamic dependencies")


def _free_loopback_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as stream:
        stream.bind(("127.0.0.1", 0))
        return int(stream.getsockname()[1])


def _wait_for_port(port: int, timeout_seconds: float) -> bool:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def _readline(process: subprocess.Popen[str], timeout_seconds: float) -> str:
    if process.stdout is None:
        raise NativeBundleInputError("child stdout is unavailable")
    ready, _, _ = select.select([process.stdout], [], [], timeout_seconds)
    if not ready:
        return ""
    return process.stdout.readline()


def resolve_pty_alias(
    alias: str, *, allowed_parent: pathlib.Path = pathlib.Path("/dev/pts")
) -> str:
    try:
        resolved = pathlib.Path(alias).resolve(strict=True)
    except OSError as error:
        raise NativeBundleInputError(f"cannot resolve PTY alias {alias!r}: {error}") from error
    if resolved.parent != allowed_parent.resolve() or not resolved.name.isdecimal():
        raise NativeBundleInputError(
            f"PTY alias must resolve beneath {allowed_parent}, got {str(resolved)!r}"
        )
    return str(resolved)


def _stop_owned_process(process: subprocess.Popen[str] | None) -> tuple[str, str]:
    if process is None:
        return "", ""
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
    try:
        return process.communicate(timeout=5.0)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        return process.communicate(timeout=2.0)


def _json_events(text: str) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    for line in text.splitlines():
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            events.append(value)
    return events


def run_clean_smoke(
    *,
    package_root: pathlib.Path,
    pty_bus: pathlib.Path,
    evidence_dir: pathlib.Path,
) -> dict[str, Any]:
    binary = package_root / "usr/local/bin/gateway_app"
    register_map = (
        package_root
        / "usr/local/share/industrial_iot_gateway/config/register_map.yaml"
    )
    scenario_config = (
        package_root
        / "usr/local/share/industrial_iot_gateway/config/pty_slave_scenarios.yaml"
    )
    port = _free_loopback_port()
    broker: subprocess.Popen[str] | None = None
    fixture: subprocess.Popen[str] | None = None
    gateway: subprocess.Popen[str] | None = None
    fixture_ready = ""
    gateway_prefix = ""
    gateway_remaining = ""
    gateway_error = ""
    gateway_captured = False
    shutdown_ms = 0
    try:
        broker = subprocess.Popen(
            ["mosquitto", "-p", str(port), "-v"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            start_new_session=True,
        )
        if not _wait_for_port(port, 3.0):
            raise NativeBundleInputError("clean smoke Mosquitto did not become ready")
        fixture = subprocess.Popen(
            [
                str(pty_bus),
                "--register-map",
                str(register_map),
                "--scenario-config",
                str(scenario_config),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            start_new_session=True,
        )
        fixture_ready = _readline(fixture, 5.0).strip()
        if not fixture_ready:
            raise NativeBundleInputError("timed out waiting for PTY fixture readiness")
        try:
            ready_event = json.loads(fixture_ready)
        except json.JSONDecodeError as error:
            raise NativeBundleInputError(f"invalid PTY ready event: {fixture_ready!r}") from error
        serial_alias = ready_event.get("path")
        if ready_event.get("event") != "pty_bus_ready" or not isinstance(serial_alias, str):
            raise NativeBundleInputError(f"PTY fixture did not report a serial path: {ready_event!r}")
        serial_path = resolve_pty_alias(serial_alias)
        gateway = subprocess.Popen(
            [
                str(binary),
                "--serial-device",
                serial_path,
                "--register-map",
                str(register_map),
                "--mqtt-broker-uri",
                f"tcp://127.0.0.1:{port}",
                "--mqtt-client-id",
                f"bundlesmoke{os.getpid()}",
                "--gateway-id",
                "bundle_smoke",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            start_new_session=True,
        )
        deadline = time.monotonic() + 12.0
        prefix_lines: list[str] = []
        while time.monotonic() < deadline:
            line = _readline(gateway, min(1.0, max(0.01, deadline - time.monotonic())))
            prefix_lines.append(line)
            events = _json_events("".join(prefix_lines))
            ready = any(event.get("event") == "gateway_ready" for event in events)
            published = any(event.get("event") == "mqtt_publish_attempt" for event in events)
            requested = any(event.get("event") == "request_completed" for event in events)
            if ready and published and requested:
                break
        else:
            raise NativeBundleInputError("clean smoke did not observe ready/request/MQTT events")
        gateway_prefix = "".join(prefix_lines)
        started = time.monotonic()
        os.killpg(gateway.pid, signal.SIGTERM)
        gateway_remaining, gateway_error = gateway.communicate(timeout=5.0)
        gateway_captured = True
        shutdown_ms = int((time.monotonic() - started) * 1000)
        gateway_stdout = gateway_prefix + gateway_remaining
        events = _json_events(gateway_stdout)
        summaries = [event for event in events if event.get("event") == "gateway_summary"]
        if gateway.returncode != 0 or not summaries:
            raise NativeBundleInputError(
                f"clean smoke gateway shutdown failed: returncode={gateway.returncode}"
            )
        summary = summaries[-1]
        if (
            summary.get("stopped") is not True
            or int(summary.get("requests_succeeded", 0)) < 1
            or int(summary.get("mqtt_publish_successes", 0)) < 1
            or shutdown_ms > 5000
        ):
            raise NativeBundleInputError(f"clean smoke summary violates contract: {summary}")
        return {
            "status": "PASS",
            "gateway_exit_code": gateway.returncode,
            "shutdown_ms": shutdown_ms,
            "requests_succeeded": int(summary["requests_succeeded"]),
            "mqtt_publish_successes": int(summary["mqtt_publish_successes"]),
        }
    finally:
        fixture_stdout, fixture_stderr = _stop_owned_process(fixture)
        broker_stdout, broker_stderr = _stop_owned_process(broker)
        if gateway is not None and not gateway_captured:
            gateway_remaining, gateway_error = _stop_owned_process(gateway)
        (evidence_dir / "clean_smoke_gateway.stdout.jsonl").write_text(
            gateway_prefix + gateway_remaining, encoding="utf-8"
        )
        (evidence_dir / "clean_smoke_gateway.stderr.txt").write_text(
            gateway_error, encoding="utf-8"
        )
        (evidence_dir / "clean_smoke_fixture.stdout.txt").write_text(
            fixture_ready + ("\n" if fixture_ready else "") + fixture_stdout,
            encoding="utf-8",
        )
        (evidence_dir / "clean_smoke_fixture.stderr.txt").write_text(
            fixture_stderr, encoding="utf-8"
        )
        (evidence_dir / "clean_smoke_broker.stdout.txt").write_text(
            broker_stdout, encoding="utf-8"
        )
        (evidence_dir / "clean_smoke_broker.stderr.txt").write_text(
            broker_stderr, encoding="utf-8"
        )


def _candidate_capabilities(target_platform: str) -> dict[str, bool]:
    return {
        "x86_64_validated": target_platform == "linux-x86_64",
        "arm64_native_build_validated": target_platform == "linux-arm64",
        "arm64_systemd_validated": target_platform == "linux-arm64",
        "arm64_long_soak_validated": False,
        "arm64_release_bundle_ready": False,
        "hardware_validated": False,
    }


def run_native_bundle(
    *,
    repository_root: pathlib.Path,
    source_revision: str,
    target_platform: str,
    version: str,
    jobs: int,
    artifact_root: pathlib.Path,
    work_root: pathlib.Path,
    release_status: str,
) -> int:
    try:
        validate_source_revision(source_revision)
    except ManifestError as error:
        raise NativeBundleInputError(str(error)) from error
    if version != FROZEN_VERSION:
        raise NativeBundleInputError(f"only frozen version {FROZEN_VERSION} is accepted")
    if release_status not in {"candidate", "final"}:
        raise NativeBundleInputError(f"unsupported release status: {release_status}")
    contract = validate_native_platform(target_platform)
    repository_root = repository_root.resolve()
    artifact_root = resolve_under(
        (repository_root / "artifacts" / "releases").resolve(), artifact_root
    )
    work_root = resolve_under((repository_root / "build").resolve(), work_root)
    if work_root.exists():
        raise NativeBundleInputError(f"native bundle work root already exists: {work_root}")
    validate_repository_state(repository_root, source_revision)

    work_root.mkdir(parents=True)
    evidence_dir = create_run_directory(artifact_root, source_revision)
    (evidence_dir / "source_revision.txt").write_text(source_revision + "\n", encoding="utf-8")
    identity = collect_environment_identity()
    environment = {
        **dataclasses.asdict(identity),
        "actual_platform": platform_for_machine(identity.machine),
        "target_platform": contract.target_platform,
        "python": platform_module.python_version(),
        "generated_utc": datetime.now(UTC).isoformat(),
    }
    _write_json(evidence_dir / "environment.json", environment)
    steps = build_native_bundle_steps(repository_root, work_root, jobs=jobs)
    result = execute_steps(steps, evidence_dir=evidence_dir, cwd=repository_root)
    if result != 0:
        print(f"NATIVE_BUNDLE=FAIL EVIDENCE_DIR={evidence_dir}")
        return result

    try:
        release_name = package_filename(version, target_platform).removesuffix(".tar.gz")
        gates = {
            "native_build": "PASS",
            "full_ctest": "PASS",
            "native_systemd": "PASS",
            "arm64_long_soak": "NOT_RUN" if target_platform == "linux-arm64" else "NOT_APPLICABLE",
            "hardware_g6": "WAITING_FOR_HARDWARE",
        }
        manifests: list[dict[str, Any]] = []
        for label in ("a", "b"):
            stage = work_root / f"stage-{label}"
            manifest = build_manifest_v2(
                stage,
                release_name=release_name,
                source_revision=source_revision,
                environment=environment,
                gates=gates,
                target_platform=contract.target_platform,
                machine=contract.machine,
                package_architecture=contract.package_architecture,
                release_status=release_status,
                capabilities=_candidate_capabilities(target_platform),
            )
            if len(manifest["files"]) != len(REQUIRED_PACKAGE_PATHS):
                raise NativeBundleInputError(
                    f"stage-{label} contains {len(manifest['files'])} files, expected 9"
                )
            findings = scan_release_tree(stage)
            if findings:
                raise NativeBundleInputError(
                    f"stage-{label} sensitive scan found {len(findings)} blocker(s)"
                )
            install_dir = evidence_dir / f"install-{label}"
            write_manifest(manifest, install_dir / "install_manifest.json")
            _write_json(install_dir / "sensitive_scan.json", findings)
            manifests.append(manifest)
        if manifests[0]["files"] != manifests[1]["files"]:
            raise NativeBundleInputError("independent install manifests are not identical")

        archive_a = work_root / "archive-a" / package_filename(version, target_platform)
        archive_b = work_root / "archive-b" / package_filename(version, target_platform)
        create_deterministic_archive(work_root / "stage-a", archive_a, archive_root=release_name)
        create_deterministic_archive(work_root / "stage-b", archive_b, archive_root=release_name)
        if sha256_file(archive_a) != sha256_file(archive_b):
            raise NativeBundleInputError("normalized archives are not byte-identical")

        unpacked_root = _safe_extract(archive_a, work_root / "clean-unpack")
        clean_manifest = build_manifest_v2(
            unpacked_root,
            release_name=release_name,
            source_revision=source_revision,
            environment=environment,
            gates=gates,
            target_platform=contract.target_platform,
            machine=contract.machine,
            package_architecture=contract.package_architecture,
            release_status=release_status,
            capabilities=_candidate_capabilities(target_platform),
        )
        if clean_manifest["files"] != manifests[0]["files"]:
            raise NativeBundleInputError("clean unpack manifest differs from installed tree")
        binary = unpacked_root / "usr/local/bin/gateway_app"
        _validate_binary(
            binary,
            contract=contract,
            cwd=repository_root,
            evidence_dir=evidence_dir,
            label="clean_unpack_gateway",
        )
        smoke = run_clean_smoke(
            package_root=unpacked_root,
            pty_bus=work_root / "release/bin/gateway_pty_bus",
            evidence_dir=evidence_dir,
        )
        _write_json(evidence_dir / "clean_smoke_summary.json", smoke)

        bundle_dir = evidence_dir / "bundle"
        bundle_dir.mkdir(parents=True)
        final_archive = bundle_dir / package_filename(version, target_platform)
        shutil.copy2(archive_a, final_archive)
        manifest_path = bundle_dir / "install_manifest.json"
        write_manifest(manifests[0], manifest_path)
        write_sha256sums(
            [final_archive, manifest_path], root=evidence_dir, output=bundle_dir / "SHA256SUMS"
        )
        summary_path = evidence_dir / "summary.json"
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        summary.update(
            {
                "status": "PASS",
                "pass_marker": PASS_MARKER,
                "target_platform": target_platform,
                "machine": contract.machine,
                "package_architecture": contract.package_architecture,
                "package_name": final_archive.name,
                "package_bytes": final_archive.stat().st_size,
                "package_sha256": sha256_file(final_archive),
                "install_file_count": len(manifests[0]["files"]),
                "reproducible_archive": True,
                "clean_smoke": smoke,
                "hardware_validated": False,
            }
        )
        _write_json(summary_path, summary)
        (evidence_dir / PASS_MARKER).write_text("pass\n", encoding="utf-8")
    except (ManifestError, NativeBundleInputError, OSError, ValueError, subprocess.SubprocessError) as error:
        _post_failure(evidence_dir, str(error))
        print(f"NATIVE_BUNDLE=FAIL EVIDENCE_DIR={evidence_dir}")
        return 4

    print(f"NATIVE_BUNDLE=PASS EVIDENCE_DIR={evidence_dir}")
    print(f"TARGET_PLATFORM={target_platform}")
    print("HARDWARE_VALIDATED=false")
    return 0


def parse_arguments(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build a reproducible native release bundle")
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--target-platform", choices=tuple(TARGET_ARCHITECTURES), required=True)
    parser.add_argument("--version", default=FROZEN_VERSION)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument(
        "--artifact-root",
        type=pathlib.Path,
        default=REPOSITORY_ROOT / "artifacts" / "releases" / f"v{FROZEN_VERSION}" / "native",
    )
    parser.add_argument(
        "--work-root",
        type=pathlib.Path,
        default=REPOSITORY_ROOT / "build" / "native-bundle",
    )
    parser.add_argument("--release-status", choices=("candidate", "final"), required=True)
    return parser.parse_args(argv)


def _absolute_from_repository(path: pathlib.Path) -> pathlib.Path:
    return path if path.is_absolute() else REPOSITORY_ROOT / path


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parse_arguments(argv)
    try:
        return run_native_bundle(
            repository_root=REPOSITORY_ROOT,
            source_revision=arguments.source_revision,
            target_platform=arguments.target_platform,
            version=arguments.version,
            jobs=arguments.jobs,
            artifact_root=_absolute_from_repository(arguments.artifact_root),
            work_root=_absolute_from_repository(arguments.work_root),
            release_status=arguments.release_status,
        )
    except (NativeBundleInputError, ManifestError, ReleaseInputError) as error:
        print(f"NATIVE_BUNDLE_ERROR={error}")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

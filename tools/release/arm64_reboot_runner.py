from __future__ import annotations

import argparse
import json
import math
import os
import pathlib
import pwd
import shutil
import socket
import subprocess
import tarfile
import time
from collections.abc import Mapping, Sequence
from datetime import UTC, datetime
from typing import Any

from tools.release.manifest import (
    REQUIRED_PACKAGE_PATHS,
    ManifestError,
    sha256_file,
    validate_source_revision,
    write_sha256sums,
)


CHECKPOINT_SCHEMA = "p3-s7-arm64-reboot-checkpoint-v1"
PREPARED_PHASE = "PREPARED_FOR_REBOOT"
EXPECTED_SLAVES = ("1", "2", "3")
READY_BUDGET_SECONDS = 120.0
OBSERVATION_BUDGET_SECONDS = 600.0
MAX_TEMPERATURE_C = 80.0
FIXTURE_UNIT = "industrial_iot_gateway-reboot-fixture.service"
GATEWAY_UNIT = "industrial_iot_gateway.service"
FIXTURE_UNIT_PATH = pathlib.Path("/etc/systemd/system") / FIXTURE_UNIT
GATEWAY_DROPIN = pathlib.Path("/etc/systemd/system/industrial_iot_gateway.service.d/reboot-test.conf")
ENV_PATH = pathlib.Path("/etc/industrial_iot_gateway/gateway.env")
STATE_ROOT = pathlib.Path("/var/lib/industrial_iot_gateway")
PTY_BUS_PATH = pathlib.Path("/usr/local/libexec/industrial_iot_gateway/gateway_pty_bus")
FIXTURE_PATH = pathlib.Path("/usr/local/libexec/industrial_iot_gateway/arm64_reboot_fixture.py")
SERIAL_ALIAS = pathlib.Path("/run/industrial_iot_gateway-reboot/serial")
BOOT_ID_PATH = pathlib.Path("/proc/sys/kernel/random/boot_id")
THROTTLED_CURRENT_MASK = 0xF
THROTTLED_HISTORY_MASK = 0xF0000
PASS_MARKER = "PASS_P3_S7_ARM64_T04_LONG_SOAK_AND_REBOOT"
PRODUCT_SHARE_ROOT = pathlib.Path("/usr/local/share/industrial_iot_gateway")
PRODUCT_DOC_ROOT = pathlib.Path("/usr/local/share/doc/industrial_iot_gateway")
CLEANUP_DIRECTORIES = (
    ENV_PATH.parent,
    STATE_ROOT,
    GATEWAY_DROPIN.parent,
    PTY_BUS_PATH.parent,
    PRODUCT_SHARE_ROOT,
    PRODUCT_DOC_ROOT,
)
ALLOWED_OWNED_PATHS = frozenset(
    {
        *(str(pathlib.Path("/") / relative) for relative in REQUIRED_PACKAGE_PATHS),
        str(PTY_BUS_PATH),
        str(FIXTURE_PATH),
        str(FIXTURE_UNIT_PATH),
        str(GATEWAY_DROPIN),
        str(ENV_PATH),
    }
)


class RebootContractError(ValueError):
    """Raised when the controlled reboot evidence violates the frozen contract."""


def parse_fixture_ready(line: str) -> str:
    """Return a validated numeric devpts path from the PTY fixture ready event."""

    try:
        event = json.loads(line)
    except json.JSONDecodeError as error:
        raise RebootContractError("invalid pty_bus_ready JSON") from error
    if not isinstance(event, dict) or event.get("event") != "pty_bus_ready":
        raise RebootContractError("missing pty_bus_ready event")
    value = event.get("path")
    if not isinstance(value, str):
        raise RebootContractError("pty_bus_ready path must be a string")
    try:
        resolved = pathlib.Path(value).resolve(strict=True)
    except OSError as error:
        raise RebootContractError(
            f"PTY path must resolve beneath /dev/pts: {error}"
        ) from error
    if resolved.parent != pathlib.Path("/dev/pts") or not resolved.name.isdecimal():
        raise RebootContractError(f"PTY path must resolve beneath /dev/pts: {resolved}")
    return str(resolved)


def validate_checkpoint(
    checkpoint: Mapping[str, Any], source_revision: str, acceptance_tool_revision: str
) -> None:
    """Validate the durable prepare-phase checkpoint before post-reboot work."""

    try:
        validate_source_revision(source_revision)
        validate_source_revision(acceptance_tool_revision)
    except ManifestError as error:
        raise RebootContractError(str(error)) from error
    if checkpoint.get("schema_version") != CHECKPOINT_SCHEMA:
        raise RebootContractError("unsupported checkpoint schema")
    if checkpoint.get("phase") != PREPARED_PHASE:
        raise RebootContractError("checkpoint phase is not PREPARED_FOR_REBOOT")
    if checkpoint.get("source_revision") != source_revision:
        raise RebootContractError("checkpoint source revision does not match candidate")
    if checkpoint.get("acceptance_tool_revision") != acceptance_tool_revision:
        raise RebootContractError("checkpoint acceptance tool revision does not match runner")
    runner_digest = checkpoint.get("runner_sha256")
    if (
        not isinstance(runner_digest, str)
        or len(runner_digest) != 64
        or any(character not in "0123456789abcdef" for character in runner_digest)
    ):
        raise RebootContractError("checkpoint runner hash is invalid")
    if not str(checkpoint.get("boot_id_before", "")).strip():
        raise RebootContractError("checkpoint boot ID is missing")
    if not str(checkpoint.get("prepared_utc", "")).strip():
        raise RebootContractError("checkpoint preparation timestamp is missing")
    paths = checkpoint.get("owned_paths")
    if not isinstance(paths, list) or not paths:
        raise RebootContractError("checkpoint owned paths are missing")
    for value in paths:
        if not isinstance(value, str) or not pathlib.PurePosixPath(value).is_absolute():
            raise RebootContractError("checkpoint owned paths must be absolute")
        if value not in ALLOWED_OWNED_PATHS:
            raise RebootContractError(f"checkpoint path is outside the cleanup allowlist: {value}")
    hashes = checkpoint.get("installed_sha256")
    if not isinstance(hashes, dict) or not hashes:
        raise RebootContractError("checkpoint installed hashes are missing")
    for path, digest in hashes.items():
        if (
            not isinstance(path, str)
            or not pathlib.PurePosixPath(path).is_absolute()
            or not isinstance(digest, str)
            or len(digest) != 64
            or any(character not in "0123456789abcdef" for character in digest)
        ):
            raise RebootContractError("checkpoint installed hashes are invalid")
    if set(paths) != set(hashes):
        raise RebootContractError("checkpoint owned paths and installed hashes must name the same paths")
    directories = checkpoint.get("owned_directories")
    if not isinstance(directories, list) or set(directories) != {
        str(path) for path in CLEANUP_DIRECTORIES
    }:
        raise RebootContractError("checkpoint owned directories do not match the cleanup allowlist")


def _number(observation: Mapping[str, Any], name: str, default: float = 0.0) -> float:
    value = observation.get(name, default)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return default
    return float(value)


def evaluate_post_reboot(observation: Mapping[str, Any]) -> list[str]:
    """Return every failed post-reboot oracle without hiding concurrent failures."""

    failures: list[str] = []
    before = str(observation.get("boot_id_before", ""))
    after = str(observation.get("boot_id_after", ""))
    if not before or not after or before == after:
        failures.append("boot ID did not change after the controlled reboot")
    if observation.get("fixture_active") is not True:
        failures.append("fixture service is not active")
    if observation.get("gateway_active") is not True:
        failures.append("gateway service is not active")
    ready_seconds = _number(observation, "ready_within_seconds", float("inf"))
    if ready_seconds < 0.0 or ready_seconds > READY_BUDGET_SECONDS:
        failures.append("gateway did not become ready within 120 seconds")

    raw_slaves = observation.get("slave_successes")
    slaves = raw_slaves if isinstance(raw_slaves, dict) else {}
    for slave_id in EXPECTED_SLAVES:
        successes = slaves.get(slave_id, 0)
        if isinstance(successes, bool) or not isinstance(successes, int) or successes < 1:
            failures.append(f"slave {slave_id} has no successful request after reboot")
    if _number(observation, "mqtt_publish_successes") < 1.0:
        failures.append("MQTT has no successful publish after reboot")
    if _number(observation, "nrestarts") != 0.0:
        failures.append("gateway NRestarts must remain 0")
    if _number(observation, "fixture_nrestarts") != 0.0:
        failures.append("fixture NRestarts must remain 0")
    failed_units = observation.get("failed_units")
    if not isinstance(failed_units, list) or failed_units:
        failures.append("failed unit list is not empty")
    if observation.get("restart_loop") is not False:
        failures.append("restart loop was detected")

    temperature = _number(observation, "temperature_peak_c", float("inf"))
    if temperature >= MAX_TEMPERATURE_C:
        failures.append("SoC temperature must remain below 80 C")
    if _number(observation, "throttled_current_bits") != 0.0:
        failures.append("current throttled bits must remain zero")
    if _number(observation, "throttled_history_new_bits") != 0.0:
        failures.append("historical throttled bits changed after baseline")
    if _number(observation, "observation_seconds") < OBSERVATION_BUDGET_SECONDS:
        failures.append("post-reboot observation must reach 600 seconds")
    return failures


def evaluate_cleanup_observation(observation: Mapping[str, Any]) -> list[str]:
    """Evaluate every frozen system cleanup postcondition."""

    checks = (
        ("disable_ok", "systemd disable --now failed"),
        ("daemon_reload_ok", "systemd daemon-reload failed"),
        ("reset_failed_ok", "systemd reset-failed failed"),
        ("gateway_unit_absent", "gateway unit still exists"),
        ("fixture_unit_absent", "fixture unit still exists"),
        ("gateway_disabled", "gateway enable state still present"),
        ("fixture_disabled", "fixture enable state still present"),
        ("gateway_inactive", "gateway active state still present"),
        ("fixture_inactive", "fixture active state still present"),
        ("user_absent", "iot-gw user still exists"),
        ("group_absent", "iot-gw group still exists"),
        ("owned_paths_absent", "owned path residue remains"),
        ("owned_directories_absent", "owned directory residue remains"),
        ("runtime_directory_absent", "runtime directory residue remains"),
        ("broker_port_released", "broker port 18884 remains in use"),
    )
    return [message for key, message in checks if observation.get(key) is not True]


def _run(arguments: list[str], timeout: float = 30.0) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        arguments,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
        check=False,
        shell=False,
    )


def _write_json(path: pathlib.Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def _require_root() -> None:
    if os.geteuid() != 0:
        raise RebootContractError("ARM64 reboot runner requires one root invocation")


def _boot_id() -> str:
    value = BOOT_ID_PATH.read_text(encoding="utf-8").strip()
    if not value:
        raise RebootContractError("boot ID is unavailable")
    return value


def _throttled_value() -> int:
    result = _run(["vcgencmd", "get_throttled"], 5.0)
    value = result.stdout.strip()
    if result.returncode != 0 or not value.startswith("throttled=0x"):
        raise RebootContractError("vcgencmd get_throttled is unavailable")
    try:
        return int(value.split("=", 1)[1], 16)
    except ValueError as error:
        raise RebootContractError(f"invalid get_throttled value: {value}") from error


def _temperature_c() -> float:
    result = _run(["vcgencmd", "measure_temp"], 5.0)
    value = result.stdout.strip()
    if result.returncode == 0 and value.startswith("temp=") and value.endswith("'C"):
        try:
            return float(value[5:-2])
        except ValueError:
            pass
    try:
        return int(pathlib.Path("/sys/class/thermal/thermal_zone0/temp").read_text()) / 1000.0
    except (OSError, ValueError) as error:
        raise RebootContractError("SoC temperature is unavailable") from error


def _create_run_dir(root: pathlib.Path, source_revision: str) -> pathlib.Path:
    root.mkdir(parents=True, exist_ok=True)
    prefix = datetime.now(UTC).strftime("%Y%m%dT%H%M%SZ") + f"_reboot_{source_revision[:7]}"
    for sequence in range(1, 1000):
        candidate = root / f"{prefix}_{sequence:03d}"
        try:
            candidate.mkdir()
            return candidate
        except FileExistsError:
            continue
    raise RebootContractError("cannot allocate reboot evidence directory")


def _safe_extract_bundle(bundle: pathlib.Path, destination: pathlib.Path) -> pathlib.Path:
    if not bundle.is_file():
        raise RebootContractError(f"bundle is not a file: {bundle}")
    destination.mkdir(parents=True, exist_ok=False)
    resolved_destination = destination.resolve()
    with tarfile.open(bundle, mode="r:gz") as archive:
        members = archive.getmembers()
        if not members:
            raise RebootContractError("bundle is empty")
        top_levels: set[str] = set()
        for member in members:
            pure = pathlib.PurePosixPath(member.name)
            if pure.is_absolute() or ".." in pure.parts or not pure.parts:
                raise RebootContractError(f"unsafe bundle path: {member.name}")
            if member.issym() or member.islnk() or member.isdev():
                raise RebootContractError(f"unsupported bundle entry: {member.name}")
            top_levels.add(pure.parts[0])
            target = (destination / pathlib.Path(*pure.parts)).resolve()
            try:
                target.relative_to(resolved_destination)
            except ValueError as error:
                raise RebootContractError(f"bundle path escapes destination: {member.name}") from error
        if len(top_levels) != 1:
            raise RebootContractError("bundle must contain exactly one release root")
        archive.extractall(destination, filter="data")
    root = destination / next(iter(top_levels))
    if not root.is_dir():
        raise RebootContractError("bundle release root is missing")
    return root


def _validate_bundle_manifest(
    package_root: pathlib.Path,
    manifest_path: pathlib.Path,
    source_revision: str,
) -> None:
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RebootContractError("bundle manifest is unavailable or invalid") from error
    candidate_capabilities = {
        "x86_64_validated": False,
        "arm64_native_build_validated": True,
        "arm64_systemd_validated": True,
        "arm64_long_soak_validated": False,
        "arm64_release_bundle_ready": False,
        "hardware_validated": False,
    }
    if (
        manifest.get("schema_version") != "p3-s7-release-manifest-v2"
        or manifest.get("source_revision") != source_revision
        or manifest.get("target_platform") != "linux-arm64"
        or manifest.get("package_architecture") != "arm64"
        or manifest.get("release_status") != "candidate"
        or manifest.get("capabilities") != candidate_capabilities
        or manifest.get("published") is not False
        or manifest.get("hardware_validated") is not False
        or manifest.get("tag") is not None
    ):
        raise RebootContractError("bundle manifest does not match the ARM64 candidate boundary")
    entries = manifest.get("files")
    if not isinstance(entries, list) or len(entries) != len(REQUIRED_PACKAGE_PATHS):
        raise RebootContractError("bundle manifest must describe exactly 9 product files")
    records = {entry.get("path"): entry for entry in entries if isinstance(entry, dict)}
    if set(records) != set(REQUIRED_PACKAGE_PATHS):
        raise RebootContractError("bundle manifest product paths do not match the frozen set")
    for relative in REQUIRED_PACKAGE_PATHS:
        path = package_root / relative
        record = records[relative]
        if not path.is_file() or record.get("sha256") != sha256_file(path):
            raise RebootContractError(f"bundle file hash mismatch: {relative}")


def _fixture_unit_text() -> str:
    return """[Unit]
Description=Industrial IoT Gateway reboot-test PTY and MQTT fixture
Before=industrial_iot_gateway.service
StartLimitIntervalSec=60
StartLimitBurst=3

[Service]
Type=notify
NotifyAccess=all
User=iot-gw
Group=iot-gw
RuntimeDirectory=industrial_iot_gateway-reboot
RuntimeDirectoryMode=0750
Environment=PYTHONDONTWRITEBYTECODE=1
ExecStart=/usr/bin/python3 /usr/local/libexec/industrial_iot_gateway/arm64_reboot_fixture.py --pty-bus /usr/local/libexec/industrial_iot_gateway/gateway_pty_bus --register-map /usr/local/share/industrial_iot_gateway/config/register_map.yaml --scenario-config /usr/local/share/industrial_iot_gateway/config/pty_slave_scenarios.yaml --serial-alias /run/industrial_iot_gateway-reboot/serial --broker-port 18884
Restart=on-failure
RestartSec=2s
TimeoutStartSec=30s
TimeoutStopSec=8s
KillMode=control-group
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true

[Install]
WantedBy=multi-user.target
"""


def _dropin_text() -> str:
    return """[Unit]
Requires=industrial_iot_gateway-reboot-fixture.service
After=industrial_iot_gateway-reboot-fixture.service
"""


def _environment_text() -> str:
    return (
        f"GATEWAY_SERIAL_DEVICE={SERIAL_ALIAS}\n"
        "GATEWAY_REGISTER_MAP=/usr/local/share/industrial_iot_gateway/config/register_map.yaml\n"
        "GATEWAY_MQTT_ARGS=--mqtt-broker-uri tcp://127.0.0.1:18884 "
        "--mqtt-client-id iiotreboot --gateway-id arm64_reboot\n"
    )


def _repository_is_exact(repository_root: pathlib.Path, source_revision: str) -> None:
    head = _run(["git", "-C", str(repository_root), "rev-parse", "HEAD"])
    status = _run(["git", "-C", str(repository_root), "status", "--porcelain=v1"])
    if head.returncode != 0 or head.stdout.strip() != source_revision:
        raise RebootContractError("repository HEAD does not match source revision")
    if status.returncode != 0 or status.stdout.strip():
        raise RebootContractError("repository must be a clean committed candidate")


def _path_collisions(paths: Sequence[pathlib.Path]) -> list[str]:
    return [str(path) for path in paths if path.exists() or path.is_symlink()]


def _port_in_use(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.settimeout(0.2)
        return probe.connect_ex(("127.0.0.1", port)) == 0


def _is_active(unit: str) -> bool:
    result = _run(["systemctl", "is-active", unit], 5.0)
    return result.returncode == 0 and result.stdout.strip() == "active"


def _wait_units(timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if _is_active(FIXTURE_UNIT) and _is_active(GATEWAY_UNIT):
            return True
        time.sleep(0.25)
    return _is_active(FIXTURE_UNIT) and _is_active(GATEWAY_UNIT)


def prepare(
    *,
    source_revision: str,
    acceptance_tool_revision: str,
    repository_root: pathlib.Path,
    bundle: pathlib.Path,
    bundle_manifest: pathlib.Path,
    pty_bus: pathlib.Path,
    artifact_root: pathlib.Path,
) -> pathlib.Path:
    _require_root()
    try:
        validate_source_revision(source_revision)
        validate_source_revision(acceptance_tool_revision)
    except ManifestError as error:
        raise RebootContractError(str(error)) from error
    _repository_is_exact(repository_root, source_revision)
    acceptance_tool_root = pathlib.Path(__file__).resolve().parents[2]
    _repository_is_exact(acceptance_tool_root, acceptance_tool_revision)
    if not pty_bus.is_file():
        raise RebootContractError("PTY bus input is missing")
    fixture_source = pathlib.Path(__file__).with_name("arm64_reboot_fixture.py")
    if not fixture_source.is_file():
        raise RebootContractError("standalone reboot fixture source is missing")

    run_dir = _create_run_dir(artifact_root, source_revision)
    unpack_parent = run_dir / "unpacked"
    try:
        package_root = _safe_extract_bundle(bundle, unpack_parent)
        _validate_bundle_manifest(package_root, bundle_manifest, source_revision)
    except Exception:
        shutil.rmtree(unpack_parent, ignore_errors=True)
        raise
    product_destinations = [pathlib.Path("/") / relative for relative in REQUIRED_PACKAGE_PATHS]
    fixed_files = [
        *product_destinations,
        PTY_BUS_PATH,
        FIXTURE_PATH,
        FIXTURE_UNIT_PATH,
        GATEWAY_DROPIN,
        ENV_PATH,
    ]
    collisions = _path_collisions([*fixed_files, *CLEANUP_DIRECTORIES])
    try:
        pwd.getpwnam("iot-gw")
        collisions.append("user:iot-gw")
    except KeyError:
        pass
    if _run(["getent", "group", "iot-gw"]).returncode == 0:
        collisions.append("group:iot-gw")
    if _run(["systemctl", "cat", GATEWAY_UNIT]).returncode == 0:
        collisions.append(f"unit:{GATEWAY_UNIT}")
    if _run(["systemctl", "cat", FIXTURE_UNIT]).returncode == 0:
        collisions.append(f"unit:{FIXTURE_UNIT}")
    if _port_in_use(18_884):
        collisions.append("127.0.0.1:18884")
    if collisions:
        shutil.rmtree(unpack_parent, ignore_errors=True)
        raise RebootContractError(f"dedicated-host collision: {sorted(collisions)}")

    written: list[pathlib.Path] = []
    created_user = False
    created_group = False
    try:
        if _run(["groupadd", "--system", "iot-gw"]).returncode != 0:
            raise RebootContractError("cannot create iot-gw group")
        created_group = True
        if _run(
            [
                "useradd",
                "--system",
                "--gid",
                "iot-gw",
                "--home-dir",
                str(STATE_ROOT),
                "--shell",
                "/usr/sbin/nologin",
                "iot-gw",
            ]
        ).returncode != 0:
            raise RebootContractError("cannot create iot-gw user")
        created_user = True
        if _run(["usermod", "--append", "--groups", "dialout", "iot-gw"]).returncode != 0:
            raise RebootContractError("cannot add iot-gw to dialout")

        for relative, destination in zip(REQUIRED_PACKAGE_PATHS, product_destinations, strict=True):
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(package_root / relative, destination)
            written.append(destination)
        PTY_BUS_PATH.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(pty_bus, PTY_BUS_PATH)
        os.chmod(PTY_BUS_PATH, 0o755)
        written.append(PTY_BUS_PATH)
        shutil.copy2(fixture_source, FIXTURE_PATH)
        os.chmod(FIXTURE_PATH, 0o755)
        written.append(FIXTURE_PATH)
        FIXTURE_UNIT_PATH.parent.mkdir(parents=True, exist_ok=True)
        FIXTURE_UNIT_PATH.write_text(_fixture_unit_text(), encoding="utf-8")
        os.chmod(FIXTURE_UNIT_PATH, 0o644)
        written.append(FIXTURE_UNIT_PATH)
        GATEWAY_DROPIN.parent.mkdir(parents=True, exist_ok=True)
        GATEWAY_DROPIN.write_text(_dropin_text(), encoding="utf-8")
        os.chmod(GATEWAY_DROPIN, 0o644)
        written.append(GATEWAY_DROPIN)
        ENV_PATH.parent.mkdir(parents=True, exist_ok=True)
        ENV_PATH.write_text(_environment_text(), encoding="utf-8")
        os.chmod(ENV_PATH, 0o640)
        account = pwd.getpwnam("iot-gw")
        os.chown(ENV_PATH.parent, account.pw_uid, account.pw_gid)
        os.chown(ENV_PATH, account.pw_uid, account.pw_gid)
        written.append(ENV_PATH)

        if _run(["systemctl", "daemon-reload"]).returncode != 0:
            raise RebootContractError("systemd daemon-reload failed")
        if _run(["systemctl", "enable", FIXTURE_UNIT, GATEWAY_UNIT]).returncode != 0:
            raise RebootContractError("cannot enable reboot-test units")
        if _run(["systemctl", "start", GATEWAY_UNIT], 20.0).returncode != 0:
            raise RebootContractError("cannot start gateway with reboot fixture")
        if not _wait_units(20.0):
            raise RebootContractError("reboot-test units did not become active")

        throttled = _throttled_value()
        if throttled & THROTTLED_CURRENT_MASK:
            raise RebootContractError("current throttled bits are nonzero before reboot")
        installed_hashes = {str(path): sha256_file(path) for path in written}
        checkpoint = {
            "schema_version": CHECKPOINT_SCHEMA,
            "phase": PREPARED_PHASE,
            "source_revision": source_revision,
            "acceptance_tool_revision": acceptance_tool_revision,
            "runner_sha256": sha256_file(pathlib.Path(__file__)),
            "boot_id_before": _boot_id(),
            "prepared_utc": datetime.now(UTC).isoformat(),
            "throttled_history_baseline": throttled & THROTTLED_HISTORY_MASK,
            "temperature_before_c": _temperature_c(),
            "bundle_sha256": sha256_file(bundle),
            "bundle_manifest_sha256": sha256_file(bundle_manifest),
            "pty_bus_sha256": sha256_file(pty_bus),
            "owned_paths": sorted(installed_hashes),
            "owned_directories": sorted(str(path) for path in CLEANUP_DIRECTORIES),
            "installed_sha256": installed_hashes,
        }
        shutil.rmtree(unpack_parent)
        checkpoint_path = run_dir / "checkpoint.json"
        _write_json(checkpoint_path, checkpoint)
        _write_json(
            run_dir / "summary.json",
            {
                "status": PREPARED_PHASE,
                "source_revision": source_revision,
                "checkpoint": str(checkpoint_path),
                "reboot_executed_by_runner": False,
                "hardware_validated": False,
            },
        )
        (run_dir / "failures.json").write_text("[]\n", encoding="utf-8")
        print(f"REBOOT_PREPARE=PASS CHECKPOINT={checkpoint_path}")
        print("REBOOT_EXECUTED_BY_RUNNER=false")
        return checkpoint_path
    except Exception:
        _cleanup_owned(written, created_user=created_user, created_group=created_group)
        shutil.rmtree(unpack_parent, ignore_errors=True)
        raise


def _journal_events(unit: str) -> list[tuple[dict[str, Any], float]]:
    result = _run(["journalctl", "-b", "-u", unit, "--no-pager", "-o", "json"], 20.0)
    events: list[tuple[dict[str, Any], float]] = []
    for line in result.stdout.splitlines():
        try:
            outer = json.loads(line)
            message = json.loads(str(outer.get("MESSAGE", "")))
            monotonic = float(outer.get("__MONOTONIC_TIMESTAMP", 0)) / 1_000_000.0
        except (json.JSONDecodeError, TypeError, ValueError):
            continue
        if isinstance(message, dict) and isinstance(message.get("event"), str):
            events.append((message, monotonic))
    return events


def _unit_nrestarts(unit: str) -> int:
    result = _run(["systemctl", "show", unit, "--property", "NRestarts", "--value"])
    try:
        return int(result.stdout.strip()) if result.returncode == 0 else -1
    except ValueError:
        return -1


def _failed_units() -> list[str]:
    result = _run(["systemctl", "--failed", "--plain", "--no-legend"], 10.0)
    if result.returncode not in {0, 1}:
        return ["systemctl-query-failed"]
    return [line.split()[0] for line in result.stdout.splitlines() if line.split()]


def _observed_traffic() -> tuple[float, dict[str, int], int]:
    ready_times = [
        timestamp
        for event, timestamp in _journal_events(GATEWAY_UNIT)
        if event.get("event") == "gateway_ready"
    ]
    slaves = {slave_id: 0 for slave_id in EXPECTED_SLAVES}
    for event, _timestamp in _journal_events(GATEWAY_UNIT):
        slave_id = str(event.get("slave_id", ""))
        if (
            event.get("event") == "request_completed"
            and event.get("result") == "success"
            and slave_id in slaves
        ):
            slaves[slave_id] += 1
    mqtt_messages = sum(
        1
        for event, _timestamp in _journal_events(FIXTURE_UNIT)
        if event.get("event") == "reboot_fixture_mqtt_message"
    )
    return (min(ready_times) if ready_times else float("inf"), slaves, mqtt_messages)


def _cleanup_owned(
    paths: Sequence[pathlib.Path],
    *,
    created_user: bool = True,
    created_group: bool = True,
    observation_out: dict[str, bool] | None = None,
) -> bool:
    disable_ok = (
        _run(["systemctl", "disable", "--now", GATEWAY_UNIT, FIXTURE_UNIT], 20.0).returncode
        == 0
    )
    reset_failed_ok = (
        _run(["systemctl", "reset-failed", GATEWAY_UNIT, FIXTURE_UNIT]).returncode == 0
    )
    unlink_ok = True
    for path in sorted(paths, key=lambda value: len(value.parts), reverse=True):
        if str(path) not in ALLOWED_OWNED_PATHS:
            unlink_ok = False
            continue
        try:
            path.unlink(missing_ok=True)
        except OSError:
            unlink_ok = False
    directory_cleanup_ok = True
    for directory in sorted(CLEANUP_DIRECTORIES, key=lambda value: len(value.parts), reverse=True):
        try:
            if directory.exists():
                if directory.is_symlink() or not directory.is_dir():
                    directory_cleanup_ok = False
                else:
                    shutil.rmtree(directory)
        except OSError:
            directory_cleanup_ok = False
    daemon_reload_ok = _run(["systemctl", "daemon-reload"]).returncode == 0
    gateway_unit_absent = _run(["systemctl", "cat", GATEWAY_UNIT], 5.0).returncode != 0
    fixture_unit_absent = _run(["systemctl", "cat", FIXTURE_UNIT], 5.0).returncode != 0
    gateway_disabled = _run(["systemctl", "is-enabled", GATEWAY_UNIT], 5.0).returncode != 0
    fixture_disabled = _run(["systemctl", "is-enabled", FIXTURE_UNIT], 5.0).returncode != 0
    gateway_inactive = _run(["systemctl", "is-active", GATEWAY_UNIT], 5.0).returncode != 0
    fixture_inactive = _run(["systemctl", "is-active", FIXTURE_UNIT], 5.0).returncode != 0
    user_delete_ok = True
    if created_user:
        user_delete_ok = _run(["userdel", "iot-gw"]).returncode in {0, 6}
    user_absent = user_delete_ok and _run(["getent", "passwd", "iot-gw"]).returncode != 0
    group_delete_ok = True
    if created_group:
        probe = _run(["getent", "group", "iot-gw"])
        if probe.returncode == 0:
            group_delete_ok = _run(["groupdel", "iot-gw"]).returncode == 0
    group_absent = group_delete_ok and _run(["getent", "group", "iot-gw"]).returncode != 0
    residual = any(path.exists() or path.is_symlink() for path in paths)
    directory_residual = any(
        directory.exists() or directory.is_symlink() for directory in CLEANUP_DIRECTORIES
    )
    runtime_residual = SERIAL_ALIAS.parent.exists() or SERIAL_ALIAS.parent.is_symlink()
    port_residual = _port_in_use(18_884)
    observation = {
        "disable_ok": disable_ok,
        "daemon_reload_ok": daemon_reload_ok,
        "reset_failed_ok": reset_failed_ok,
        "gateway_unit_absent": gateway_unit_absent,
        "fixture_unit_absent": fixture_unit_absent,
        "gateway_disabled": gateway_disabled,
        "fixture_disabled": fixture_disabled,
        "gateway_inactive": gateway_inactive,
        "fixture_inactive": fixture_inactive,
        "user_absent": user_absent,
        "group_absent": group_absent,
        "owned_paths_absent": unlink_ok and not residual,
        "owned_directories_absent": directory_cleanup_ok and not directory_residual,
        "runtime_directory_absent": not runtime_residual,
        "broker_port_released": not port_residual,
    }
    if observation_out is not None:
        observation_out.clear()
        observation_out.update(observation)
    return not evaluate_cleanup_observation(observation)


def _installed_hash_mismatches(installed_hashes: Mapping[str, Any]) -> list[str]:
    mismatches: list[str] = []
    for value, digest in installed_hashes.items():
        path = pathlib.Path(value)
        if (
            value not in ALLOWED_OWNED_PATHS
            or not isinstance(digest, str)
            or not path.is_file()
            or sha256_file(path) != digest
        ):
            mismatches.append(value)
    return sorted(mismatches)


def verify(
    *,
    checkpoint_path: pathlib.Path,
    source_revision: str,
    acceptance_tool_revision: str,
    observation_seconds: float = 600.0,
) -> int:
    _require_root()
    checkpoint = json.loads(checkpoint_path.read_text(encoding="utf-8"))
    validate_checkpoint(checkpoint, source_revision, acceptance_tool_revision)
    acceptance_tool_root = pathlib.Path(__file__).resolve().parents[2]
    _repository_is_exact(acceptance_tool_root, acceptance_tool_revision)
    if checkpoint["runner_sha256"] != sha256_file(pathlib.Path(__file__)):
        raise RebootContractError("reboot runner changed across reboot")
    run_dir = checkpoint_path.parent
    installed_hashes = checkpoint["installed_sha256"]
    mismatches = _installed_hash_mismatches(installed_hashes)
    if mismatches:
        raise RebootContractError(f"installed files changed across reboot: {mismatches}")
    boot_after = _boot_id()
    started = time.monotonic()
    observation: dict[str, Any] = {
        "boot_id_before": checkpoint["boot_id_before"],
        "boot_id_after": boot_after,
        "fixture_active": False,
        "gateway_active": False,
        "ready_within_seconds": None,
        "slave_successes": {slave_id: 0 for slave_id in EXPECTED_SLAVES},
        "mqtt_publish_successes": 0,
        "nrestarts": -1,
        "fixture_nrestarts": -1,
        "failed_units": ["observation-not-completed"],
        "restart_loop": True,
        "temperature_peak_c": None,
        "throttled_current_bits": None,
        "throttled_history_new_bits": None,
        "observation_seconds": 0.0,
    }
    failures: list[str] = []
    try:
        temperature_peak = _temperature_c()
        current_bits = 0
        history_new = 0
        fixture_active = True
        gateway_active = True
        ready_seconds = float("inf")
        slaves = {slave_id: 0 for slave_id in EXPECTED_SLAVES}
        mqtt_messages = 0
        deadline = started + max(observation_seconds, OBSERVATION_BUDGET_SECONDS)
        while time.monotonic() < deadline:
            fixture_active = fixture_active and _is_active(FIXTURE_UNIT)
            gateway_active = gateway_active and _is_active(GATEWAY_UNIT)
            temperature_peak = max(temperature_peak, _temperature_c())
            throttled = _throttled_value()
            current_bits |= throttled & THROTTLED_CURRENT_MASK
            history_new |= (throttled & THROTTLED_HISTORY_MASK) & ~int(
                checkpoint.get("throttled_history_baseline", 0)
            )
            observed_ready, observed_slaves, observed_mqtt = _observed_traffic()
            ready_seconds = min(ready_seconds, observed_ready)
            slaves = {
                slave_id: max(slaves[slave_id], observed_slaves[slave_id])
                for slave_id in EXPECTED_SLAVES
            }
            mqtt_messages = max(mqtt_messages, observed_mqtt)
            time.sleep(min(5.0, max(0.0, deadline - time.monotonic())))
        gateway_restarts = _unit_nrestarts(GATEWAY_UNIT)
        fixture_restarts = _unit_nrestarts(FIXTURE_UNIT)
        observation = {
            "boot_id_before": checkpoint["boot_id_before"],
            "boot_id_after": boot_after,
            "fixture_active": fixture_active,
            "gateway_active": gateway_active,
            "ready_within_seconds": ready_seconds if math.isfinite(ready_seconds) else None,
            "slave_successes": slaves,
            "mqtt_publish_successes": mqtt_messages,
            "nrestarts": gateway_restarts,
            "fixture_nrestarts": fixture_restarts,
            "failed_units": _failed_units(),
            "restart_loop": gateway_restarts != 0 or fixture_restarts != 0,
            "temperature_peak_c": temperature_peak,
            "throttled_current_bits": current_bits,
            "throttled_history_new_bits": history_new,
            "observation_seconds": time.monotonic() - started,
        }
        failures = evaluate_post_reboot(observation)
    except (OSError, RebootContractError, subprocess.SubprocessError) as error:
        observation["observation_seconds"] = time.monotonic() - started
        failures = [f"observation exception: {type(error).__name__}: {error}"]
    cleanup_mismatches = _installed_hash_mismatches(installed_hashes)
    cleanup_ok = False
    cleanup_observation: dict[str, bool] = {}
    if cleanup_mismatches:
        failures.append(f"installed files changed before cleanup: {cleanup_mismatches}")
    else:
        cleanup_ok = _cleanup_owned(
            [pathlib.Path(path) for path in checkpoint["owned_paths"]],
            observation_out=cleanup_observation,
        )
    observation["cleanup"] = cleanup_observation
    if not cleanup_ok:
        failures.append("owned reboot-test resources were not completely removed")
    status = "PASS" if not failures else "FAIL"
    _write_json(run_dir / "observation.json", observation)
    _write_json(run_dir / "failures.json", failures)
    _write_json(
        run_dir / "summary.json",
        {
            "status": status,
            "source_revision": source_revision,
            "pass_marker": PASS_MARKER if not failures else None,
            "cleanup_ok": cleanup_ok,
            "hardware_validated": False,
            "reboot_executed_by_runner": False,
        },
    )
    checksum_inputs = [
        checkpoint_path,
        run_dir / "observation.json",
        run_dir / "failures.json",
        run_dir / "summary.json",
    ]
    write_sha256sums(checksum_inputs, root=run_dir, output=run_dir / "SHA256SUMS")
    if not failures:
        (run_dir / PASS_MARKER).write_text("pass\n", encoding="utf-8")
        print(f"ARM64_REBOOT=PASS EVIDENCE_DIR={run_dir}")
        return 0
    print(f"ARM64_REBOOT=FAIL EVIDENCE_DIR={run_dir}")
    return 4


def cleanup(
    checkpoint_path: pathlib.Path, source_revision: str, acceptance_tool_revision: str
) -> int:
    _require_root()
    checkpoint = json.loads(checkpoint_path.read_text(encoding="utf-8"))
    validate_checkpoint(checkpoint, source_revision, acceptance_tool_revision)
    acceptance_tool_root = pathlib.Path(__file__).resolve().parents[2]
    _repository_is_exact(acceptance_tool_root, acceptance_tool_revision)
    if checkpoint["runner_sha256"] != sha256_file(pathlib.Path(__file__)):
        raise RebootContractError("reboot runner changed before cleanup")
    mismatches = _installed_hash_mismatches(checkpoint["installed_sha256"])
    if mismatches:
        print(f"ARM64_REBOOT_CLEANUP=REFUSED HASH_MISMATCHES={mismatches}")
        return 4
    ok = _cleanup_owned([pathlib.Path(path) for path in checkpoint["owned_paths"]])
    print(f"ARM64_REBOOT_CLEANUP={'PASS' if ok else 'FAIL'}")
    return 0 if ok else 4


def parse_arguments(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Prepare or verify a controlled ARM64 reboot")
    subparsers = parser.add_subparsers(dest="phase", required=True)
    prepare_parser = subparsers.add_parser("prepare")
    prepare_parser.add_argument("--source-revision", required=True)
    prepare_parser.add_argument("--acceptance-tool-revision", required=True)
    prepare_parser.add_argument("--repository-root", required=True, type=pathlib.Path)
    prepare_parser.add_argument("--bundle", required=True, type=pathlib.Path)
    prepare_parser.add_argument("--bundle-manifest", required=True, type=pathlib.Path)
    prepare_parser.add_argument("--pty-bus", required=True, type=pathlib.Path)
    prepare_parser.add_argument("--artifact-root", required=True, type=pathlib.Path)
    for name in ("verify", "cleanup"):
        phase_parser = subparsers.add_parser(name)
        phase_parser.add_argument("--source-revision", required=True)
        phase_parser.add_argument("--acceptance-tool-revision", required=True)
        phase_parser.add_argument("--checkpoint", required=True, type=pathlib.Path)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parse_arguments(argv)
    try:
        if arguments.phase == "prepare":
            prepare(
                source_revision=arguments.source_revision,
                acceptance_tool_revision=arguments.acceptance_tool_revision,
                repository_root=arguments.repository_root.resolve(),
                bundle=arguments.bundle.resolve(),
                bundle_manifest=arguments.bundle_manifest.resolve(),
                pty_bus=arguments.pty_bus.resolve(),
                artifact_root=arguments.artifact_root.resolve(),
            )
            return 0
        if arguments.phase == "verify":
            return verify(
                checkpoint_path=arguments.checkpoint.resolve(),
                source_revision=arguments.source_revision,
                acceptance_tool_revision=arguments.acceptance_tool_revision,
            )
        return cleanup(
            arguments.checkpoint.resolve(),
            arguments.source_revision,
            arguments.acceptance_tool_revision,
        )
    except (OSError, RebootContractError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        print(f"ARM64_REBOOT_ERROR={type(error).__name__}: {error}")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

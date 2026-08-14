from __future__ import annotations

import argparse
import dataclasses
import json
import pathlib
import platform
import subprocess
from collections.abc import Mapping
from typing import Any


REQUIRED_G5_SCENARIOS = (
    "unit_static_verify",
    "normal_start",
    "sigterm_stop",
    "sigint_direct",
    "abnormal_restart",
    "invalid_config",
    "missing_serial",
    "broker_unavailable",
    "journal_observability",
    "repeated_cycles",
)

TARGET_PLATFORM_MACHINES = {
    "linux-x86_64": frozenset({"x86_64", "amd64"}),
    "linux-arm64": frozenset({"aarch64", "arm64"}),
}


class SystemdContractError(ValueError):
    """Raised when the systemd unit or evidence violates the frozen G5 contract."""


@dataclasses.dataclass(frozen=True)
class EnvironmentIdentity:
    machine: str
    pid1: str
    kernel_release: str
    virtualization: str


def collect_environment_identity() -> EnvironmentIdentity:
    pid1_path = pathlib.Path("/proc/1/comm")
    pid1 = pid1_path.read_text(encoding="utf-8").strip() if pid1_path.is_file() else "unknown"
    try:
        completed = subprocess.run(
            ["systemd-detect-virt"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=5.0,
            check=False,
            shell=False,
        )
        virtualization = completed.stdout.strip() if completed.returncode == 0 else "none"
    except (OSError, subprocess.TimeoutExpired):
        virtualization = "unknown"
    return EnvironmentIdentity(
        machine=platform.machine(),
        pid1=pid1,
        kernel_release=platform.release(),
        virtualization=virtualization,
    )


def platform_for_machine(machine: str) -> str:
    normalized = machine.lower()
    for target_platform, machines in TARGET_PLATFORM_MACHINES.items():
        if normalized in machines:
            return target_platform
    return "unsupported"


def classify_environment(
    identity: EnvironmentIdentity,
    *,
    allow_full_vm: bool = False,
    target_platform: str = "linux-x86_64",
) -> str:
    expected_machines = TARGET_PLATFORM_MACHINES.get(target_platform)
    if expected_machines is None:
        raise SystemdContractError(f"unsupported target platform: {target_platform!r}")
    machine = identity.machine.lower()
    pid1 = identity.pid1.lower()
    kernel = identity.kernel_release.lower()
    virtualization = identity.virtualization.lower()
    if machine not in expected_machines or pid1 != "systemd":
        return "UNSUPPORTED"
    if "microsoft" in kernel or virtualization == "wsl":
        return "DEVELOPMENT_ONLY"
    if virtualization in {"docker", "podman", "container", "lxc", "lxc-libvirt"}:
        return "UNSUPPORTED"
    if virtualization in {"kvm", "qemu", "vmware", "oracle", "microsoft"}:
        return "NATIVE_ELIGIBLE" if allow_full_vm else "FULL_VM_REQUIRES_APPROVAL"
    if virtualization in {"", "none"}:
        return "NATIVE_ELIGIBLE"
    return "UNSUPPORTED"


def parse_systemd_properties(text: str) -> dict[str, str]:
    properties: dict[str, str] = {}
    for line in text.splitlines():
        if not line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        properties[key] = value
    return properties


def validate_unit_contract(properties: Mapping[str, str]) -> None:
    expected: dict[str, set[str]] = {
        "Restart": {"on-failure"},
        "RestartUSec": {"2s", "2s 0us", "2000000us"},
        "TimeoutStopUSec": {"5s", "5s 0us", "5000000us"},
        "StartLimitBurst": {"5"},
        "KillSignal": {"15", "SIGTERM"},
        "User": {"iot-gw"},
    }
    for key, accepted in expected.items():
        actual = properties.get(key)
        if actual not in accepted:
            raise SystemdContractError(
                f"{key} violates unit contract: expected {sorted(accepted)}, got {actual!r}"
            )


def evaluate_g5(
    records: Mapping[str, Mapping[str, Any]], *, environment_class: str
) -> dict[str, Any]:
    missing = sorted(set(REQUIRED_G5_SCENARIOS) - set(records))
    failed = sorted(
        name
        for name in REQUIRED_G5_SCENARIOS
        if name in records and records[name].get("status") != "PASS"
    )
    if environment_class != "NATIVE_ELIGIBLE":
        status = environment_class
    elif missing or failed:
        status = "FAIL"
    else:
        status = "PASS"
    return {
        "schema_version": "p3-s7-g5-summary-v1",
        "status": status,
        "environment_class": environment_class,
        "missing_scenarios": missing,
        "failed_scenarios": failed,
        "scenario_count": len(records),
    }


def _write_json(path: pathlib.Path, value: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Classify or evaluate the frozen G5 environment")
    parser.add_argument("--mode", choices=("detect", "evaluate"), required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--records", type=pathlib.Path)
    parser.add_argument("--allow-full-vm", action="store_true")
    parser.add_argument(
        "--target-platform",
        choices=tuple(TARGET_PLATFORM_MACHINES),
        default="linux-x86_64",
    )
    arguments = parser.parse_args(argv)

    identity = collect_environment_identity()
    environment_class = classify_environment(
        identity,
        allow_full_vm=arguments.allow_full_vm,
        target_platform=arguments.target_platform,
    )
    if arguments.mode == "detect":
        summary = {
            **dataclasses.asdict(identity),
            "environment_class": environment_class,
            "expected_platform": arguments.target_platform,
            "actual_platform": platform_for_machine(identity.machine),
        }
    else:
        if arguments.records is None:
            parser.error("--records is required for evaluate mode")
        records = json.loads(arguments.records.read_text(encoding="utf-8"))
        if not isinstance(records, dict):
            raise SystemdContractError("scenario records must be a JSON object")
        summary = evaluate_g5(records, environment_class=environment_class)
        summary.update(
            {
                "expected_platform": arguments.target_platform,
                "actual_platform": platform_for_machine(identity.machine),
                "actual_machine": identity.machine,
            }
        )
    _write_json(arguments.output, summary)
    print(f"G5_ENVIRONMENT={environment_class}")
    return 0 if environment_class == "NATIVE_ELIGIBLE" else 3


if __name__ == "__main__":
    raise SystemExit(main())

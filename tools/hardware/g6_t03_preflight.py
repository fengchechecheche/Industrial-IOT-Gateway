from __future__ import annotations

import argparse
import grp
import json
import pathlib
import platform
import re
import shutil
import stat
import subprocess
from collections.abc import Mapping, Sequence
from typing import Any

from tools.hardware.g6_t03_contract import FIXED_SERIAL_PATH
from tools.release.systemd_runner import parse_systemd_properties


ALLOWED_UNITS = frozenset({"mosquitto.service", "industrial_iot_gateway.service"})
MINIMUM_DISK_FREE_BYTES = 15 * 1024 * 1024 * 1024
CURRENT_THROTTLED_MASK = 0xF
HISTORICAL_THROTTLED_MASK = 0xF0000


def parse_vcgencmd_temperature(text: str) -> float:
    match = re.fullmatch(r"temp=(-?[0-9]+(?:\.[0-9]+)?)'C\s*", text)
    if match is None:
        raise ValueError("invalid vcgencmd temperature")
    return float(match.group(1))


def parse_throttled(text: str) -> int:
    match = re.fullmatch(r"throttled=(0x[0-9a-fA-F]+)\s*", text)
    if match is None:
        raise ValueError("invalid vcgencmd throttled value")
    return int(match.group(1), 16)


def systemctl_show_arguments(unit: str) -> list[str]:
    if unit not in ALLOWED_UNITS:
        raise ValueError(f"unit is outside G6-T03 allowlist: {unit}")
    return [
        "systemctl",
        "show",
        unit,
        "-p",
        "LoadState",
        "-p",
        "ActiveState",
        "-p",
        "SubState",
        "-p",
        "UnitFileState",
        "-p",
        "MainPID",
        "-p",
        "NRestarts",
    ]


def _check(identifier: str, passed: bool, message: str, actual: object) -> dict[str, Any]:
    return {
        "oracle_id": identifier,
        "passed": passed,
        "message": message,
        "actual": actual,
    }


def evaluate_preflight(observation: Mapping[str, Any]) -> dict[str, Any]:
    checks = [
        _check(
            "host.arm64",
            str(observation.get("machine", "")).lower() in {"aarch64", "arm64"},
            "host must be ARM64",
            observation.get("machine"),
        ),
        _check(
            "host.pid1",
            observation.get("pid1") == "systemd",
            "PID 1 must be systemd",
            observation.get("pid1"),
        ),
        _check(
            "serial.stable_path",
            observation.get("serial_path") == FIXED_SERIAL_PATH
            and observation.get("serial_exists") is True,
            "stable serial by-id path must exist",
            {
                "path": observation.get("serial_path"),
                "exists": observation.get("serial_exists"),
            },
        ),
        _check(
            "serial.character_device",
            observation.get("serial_is_character_device") is True,
            "serial target must be a character device",
            observation.get("serial_is_character_device"),
        ),
        _check(
            "serial.group",
            observation.get("serial_group") == "dialout",
            "serial group must be dialout",
            observation.get("serial_group"),
        ),
        _check(
            "mosquitto.loaded",
            observation.get("mosquitto_load_state") == "loaded",
            "Mosquitto must be loaded",
            observation.get("mosquitto_load_state"),
        ),
        _check(
            "mosquitto.active",
            observation.get("mosquitto_active_state") == "active",
            "Mosquitto must be active",
            observation.get("mosquitto_active_state"),
        ),
        _check(
            "gateway.install_state",
            observation.get("gateway_load_state") in {"not-found", "loaded"},
            "gateway unit must be not-found before install or loaded after install",
            observation.get("gateway_load_state"),
        ),
        _check(
            "host.disk",
            isinstance(observation.get("disk_free_bytes"), int)
            and not isinstance(observation.get("disk_free_bytes"), bool)
            and int(observation["disk_free_bytes"]) >= MINIMUM_DISK_FREE_BYTES,
            "disk free space must be at least 15 GiB",
            observation.get("disk_free_bytes"),
        ),
        _check(
            "host.temperature",
            isinstance(observation.get("temperature_c"), (int, float))
            and not isinstance(observation.get("temperature_c"), bool)
            and float(observation["temperature_c"]) < 80.0,
            "temperature must remain below 80 C",
            observation.get("temperature_c"),
        ),
        _check(
            "host.throttled_current",
            observation.get("throttled_current_bits") == 0,
            "current throttled bits must be zero",
            observation.get("throttled_current_bits"),
        ),
        _check(
            "host.throttled_history",
            observation.get("throttled_history_bits") == 0,
            "historical throttled bits must be zero at preflight",
            observation.get("throttled_history_bits"),
        ),
    ]
    failures = [
        f"{value['oracle_id']}: {value['message']}; actual={value['actual']!r}"
        for value in checks
        if value["passed"] is not True
    ]
    return {
        "schema_version": "p3-s7-g6-t03-preflight-v1",
        "status": "PASS" if not failures else "FAIL",
        "checks": checks,
        "failures": failures,
    }


def _run(arguments: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        arguments,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=10.0,
        check=False,
        shell=False,
    )


def _properties(unit: str) -> dict[str, str]:
    completed = _run(systemctl_show_arguments(unit))
    return parse_systemd_properties(completed.stdout)


def collect_preflight() -> dict[str, Any]:
    serial = pathlib.Path(FIXED_SERIAL_PATH)
    serial_exists = serial.exists()
    serial_target: pathlib.Path | None = None
    serial_is_character = False
    serial_group = ""
    if serial_exists:
        try:
            serial_target = serial.resolve(strict=True)
            metadata = serial_target.stat()
            serial_is_character = stat.S_ISCHR(metadata.st_mode)
            serial_group = grp.getgrgid(metadata.st_gid).gr_name
        except (KeyError, OSError):
            pass
    mosquitto = _properties("mosquitto.service")
    gateway = _properties("industrial_iot_gateway.service")
    temperature = _run(["vcgencmd", "measure_temp"])
    throttled = _run(["vcgencmd", "get_throttled"])
    raw_throttled = parse_throttled(throttled.stdout) if throttled.returncode == 0 else -1
    return {
        "machine": platform.machine(),
        "pid1": pathlib.Path("/proc/1/comm").read_text(encoding="utf-8").strip(),
        "serial_path": FIXED_SERIAL_PATH,
        "serial_exists": serial_exists,
        "serial_target": str(serial_target) if serial_target is not None else "",
        "serial_is_character_device": serial_is_character,
        "serial_group": serial_group,
        "mosquitto_load_state": mosquitto.get("LoadState", "unknown"),
        "mosquitto_active_state": mosquitto.get("ActiveState", "unknown"),
        "gateway_load_state": gateway.get("LoadState", "unknown"),
        "disk_free_bytes": shutil.disk_usage("/").free,
        "temperature_c": parse_vcgencmd_temperature(temperature.stdout)
        if temperature.returncode == 0
        else 999.0,
        "throttled_raw": raw_throttled,
        "throttled_current_bits": raw_throttled & CURRENT_THROTTLED_MASK,
        "throttled_history_bits": raw_throttled & HISTORICAL_THROTTLED_MASK,
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Collect read-only G6-T03 Raspberry Pi preflight")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    arguments = parser.parse_args(list(argv) if argv is not None else None)
    observation = collect_preflight()
    result = evaluate_preflight(observation)
    value = {"observation": observation, **result}
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"G6_T03_PREFLIGHT={result['status']}")
    return 0 if result["status"] == "PASS" else 4


if __name__ == "__main__":
    raise SystemExit(main())

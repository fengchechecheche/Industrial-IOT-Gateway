from __future__ import annotations

import os
import pathlib
import platform
import re
import shutil
import subprocess
import time
from typing import Any


ARM64_CURRENT_THROTTLED_MASK = 0xF
ARM64_HISTORY_THROTTLED_MASK = 0xF0000
_TEMPERATURE = re.compile(r"^temp=([-+]?[0-9]+(?:\.[0-9]+)?)'C$")
_THROTTLED = re.compile(r"^throttled=0x([0-9a-fA-F]+)$")


def parse_vcgencmd_temperature(output: str) -> float | None:
    match = _TEMPERATURE.fullmatch(output.strip())
    return float(match.group(1)) if match else None


def parse_vcgencmd_throttled(output: str) -> int | None:
    match = _THROTTLED.fullmatch(output.strip())
    return int(match.group(1), 16) if match else None


def evaluate_arm64_health(
    sample: dict[str, Any], *, maximum_temperature_c: float
) -> list[str]:
    failures: list[str] = []
    temperature = sample.get("soc_temperature_c")
    if not isinstance(temperature, (int, float)):
        failures.append("temperature_missing")
    elif float(temperature) >= maximum_temperature_c:
        failures.append("temperature_at_or_above_limit")
    current = sample.get("throttled_current_bits")
    if not isinstance(current, int):
        failures.append("throttled_status_missing")
    elif current != 0:
        failures.append("current_throttling_or_undervoltage")
    history = sample.get("throttled_history_new_bits")
    if not isinstance(history, int):
        failures.append("throttled_history_missing")
    elif history != 0:
        failures.append("new_historical_throttling_or_undervoltage")
    return failures


def _optional_command(arguments: list[str]) -> str:
    try:
        completed = subprocess.run(
            arguments,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=2.0,
            check=False,
            shell=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return completed.stdout.strip() if completed.returncode == 0 else ""


def _optional_text(path: pathlib.Path) -> str:
    try:
        return path.read_text(encoding="utf-8").strip("\x00\r\n ")
    except OSError:
        return ""


def _thermal_sysfs_celsius() -> float | None:
    raw = _optional_text(pathlib.Path("/sys/class/thermal/thermal_zone0/temp"))
    try:
        return round(int(raw) / 1000.0, 3)
    except ValueError:
        return None


def _memory_available_bytes() -> int | None:
    for line in _optional_text(pathlib.Path("/proc/meminfo")).splitlines():
        if line.startswith("MemAvailable:"):
            fields = line.split()
            return int(fields[1]) * 1024 if len(fields) >= 2 else None
    return None


class Arm64HostSampler:
    def __init__(self, *, maximum_temperature_c: float) -> None:
        self.maximum_temperature_c = maximum_temperature_c
        self.machine = platform.machine().lower()
        if self.machine not in {"aarch64", "arm64"}:
            raise RuntimeError(
                f"ARM64 soak profile requires aarch64/arm64 host, got {self.machine}"
            )
        self.boot_id = _optional_text(pathlib.Path("/proc/sys/kernel/random/boot_id"))
        if not self.boot_id:
            raise RuntimeError("ARM64 host boot ID is unavailable")
        throttled_raw = _optional_command(["vcgencmd", "get_throttled"])
        throttled = parse_vcgencmd_throttled(throttled_raw)
        if throttled is None:
            raise RuntimeError("vcgencmd get_throttled is unavailable or invalid")
        self.throttled_baseline = throttled
        self.throttled_history_baseline_bits = throttled & ARM64_HISTORY_THROTTLED_MASK
        baseline = self.sample()
        failures = evaluate_arm64_health(
            baseline, maximum_temperature_c=self.maximum_temperature_c
        )
        if failures:
            raise RuntimeError("ARM64 host preflight failed: " + ", ".join(failures))

    def environment(self) -> dict[str, Any]:
        return {
            "boot_id": self.boot_id,
            "kernel_release": platform.release(),
            "machine": self.machine,
            "target_platform": "linux-arm64",
            "temperature_state": "required_and_available",
            "temperature_max_c": self.maximum_temperature_c,
            "throttled_baseline": self.throttled_baseline,
            "throttled_history_baseline_bits": self.throttled_history_baseline_bits,
        }

    def sample(self) -> dict[str, Any]:
        vcgencmd_temperature = parse_vcgencmd_temperature(
            _optional_command(["vcgencmd", "measure_temp"])
        )
        thermal_sysfs = _thermal_sysfs_celsius()
        temperature = (
            vcgencmd_temperature if vcgencmd_temperature is not None else thermal_sysfs
        )
        throttled_raw = _optional_command(["vcgencmd", "get_throttled"])
        throttled = parse_vcgencmd_throttled(throttled_raw)
        current_bits = (
            throttled & ARM64_CURRENT_THROTTLED_MASK if throttled is not None else None
        )
        history_bits = (
            throttled & ARM64_HISTORY_THROTTLED_MASK if throttled is not None else None
        )
        new_history_bits = (
            history_bits & ~self.throttled_history_baseline_bits
            if history_bits is not None
            else None
        )
        load_1, load_5, load_15 = os.getloadavg()
        return {
            "host_boot_id": self.boot_id,
            "host_kernel_release": platform.release(),
            "host_machine": self.machine,
            "host_load_1": load_1,
            "host_load_5": load_5,
            "host_load_15": load_15,
            "host_memory_available_bytes": _memory_available_bytes(),
            "soc_temperature_c": temperature,
            "vcgencmd_temperature_c": vcgencmd_temperature,
            "thermal_sysfs_c": thermal_sysfs,
            "vcgencmd_throttled_raw": throttled_raw,
            "throttled_value": throttled,
            "throttled_current_bits": current_bits,
            "throttled_history_bits": history_bits,
            "throttled_history_new_bits": new_history_bits,
        }


class ProcSampler:
    def __init__(self, pid: int, output_directory: pathlib.Path) -> None:
        self.pid = pid
        self.output_directory = output_directory
        self.previous_ticks: int | None = None
        self.previous_time: float | None = None
        self.clock_ticks = os.sysconf(os.sysconf_names["SC_CLK_TCK"])

    def sample(self, elapsed_seconds: float) -> dict[str, Any]:
        proc = pathlib.Path("/proc") / str(self.pid)
        status: dict[str, str] = {}
        for line in (proc / "status").read_text(encoding="utf-8").splitlines():
            if ":" in line:
                key, value = line.split(":", 1)
                status[key] = value.strip()
        stat = (proc / "stat").read_text(encoding="utf-8").split()
        ticks = int(stat[13]) + int(stat[14])
        now = time.monotonic()
        cpu_percent = 0.0
        if self.previous_ticks is not None and self.previous_time is not None and now > self.previous_time:
            cpu_percent = (
                (ticks - self.previous_ticks) / self.clock_ticks / (now - self.previous_time) * 100.0
            )
        self.previous_ticks = ticks
        self.previous_time = now
        rss_kib = int(status.get("VmRSS", "0 kB").split()[0])
        disk = shutil.disk_usage(self.output_directory)
        evidence_bytes = sum(
            path.stat().st_size for path in self.output_directory.rglob("*") if path.is_file()
        )
        return {
            "elapsed_seconds": elapsed_seconds,
            "rss_mib": rss_kib / 1024.0,
            "cpu_percent_single_core": cpu_percent,
            "fd_count": len(list((proc / "fd").iterdir())),
            "thread_count": int(status.get("Threads", "0")),
            "disk_free_bytes": disk.free,
            "evidence_bytes": evidence_bytes,
        }

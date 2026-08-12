from __future__ import annotations

import os
import pathlib
import shutil
import time
from typing import Any


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

from __future__ import annotations

import os
import signal
import subprocess
import time
from collections.abc import Sequence

from .models import ProcessResult


def _terminate_process_group(process: subprocess.Popen[str], grace_seconds: float) -> str:
    termination_signal = "SIGTERM"
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return termination_signal
    try:
        process.wait(timeout=max(0.0, grace_seconds))
        return termination_signal
    except subprocess.TimeoutExpired:
        termination_signal = "SIGKILL"
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()
        return termination_signal


def run_process(
    command: Sequence[str],
    *,
    timeout_seconds: float,
    termination_grace_seconds: float = 1.0,
    cwd: str | None = None,
) -> ProcessResult:
    if not command:
        raise ValueError("command must not be empty")
    if timeout_seconds <= 0:
        raise ValueError("timeout_seconds must be positive")

    started = time.monotonic()
    process = subprocess.Popen(
        list(command),
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        shell=False,
        start_new_session=True,
    )
    timed_out = False
    termination_signal = None
    try:
        stdout, stderr = process.communicate(timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        timed_out = True
        termination_signal = _terminate_process_group(process, termination_grace_seconds)
        stdout, stderr = process.communicate()
    except KeyboardInterrupt:
        _terminate_process_group(process, termination_grace_seconds)
        process.communicate()
        raise

    duration_ms = int((time.monotonic() - started) * 1000)
    return ProcessResult(
        command=list(command),
        returncode=process.returncode,
        stdout=stdout,
        stderr=stderr,
        duration_ms=duration_ms,
        timed_out=timed_out,
        termination_signal=termination_signal,
    )

from __future__ import annotations

import os
import signal
import subprocess
import time
from typing import IO, Sequence


class ManagedProcess:
    def __init__(
        self,
        argv: Sequence[str],
        *,
        stdout: int | IO[bytes] = subprocess.PIPE,
        stderr: int | IO[bytes] = subprocess.PIPE,
    ) -> None:
        self.argv = list(argv)
        self.process = subprocess.Popen(
            self.argv,
            stdout=stdout,
            stderr=stderr,
            start_new_session=True,
        )

    @property
    def pid(self) -> int:
        return self.process.pid

    def poll(self) -> int | None:
        return self.process.poll()

    def stop(self, timeout_seconds: float = 5.0) -> int:
        if self.process.poll() is not None:
            return int(self.process.returncode)
        try:
            os.killpg(self.process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            return self.process.wait(timeout=timeout_seconds)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(self.process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            return self.process.wait(timeout=2.0)

    def wait(self, timeout_seconds: float | None = None) -> int:
        return self.process.wait(timeout=timeout_seconds)


def wait_for_exit(process: ManagedProcess, timeout_seconds: float) -> bool:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        if process.poll() is not None:
            return True
        time.sleep(0.02)
    return process.poll() is not None

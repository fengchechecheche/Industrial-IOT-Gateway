from __future__ import annotations

import dataclasses
import enum
from typing import Any


class ScenarioStatus(str, enum.Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    ERROR = "ERROR"
    TIMEOUT = "TIMEOUT"


@dataclasses.dataclass(frozen=True)
class ScenarioDefinition:
    scenario_id: str
    title: str
    command: list[str]
    timeout_seconds: float
    config: dict[str, Any] = dataclasses.field(default_factory=dict)


@dataclasses.dataclass(frozen=True)
class ProcessResult:
    command: list[str]
    returncode: int | None
    stdout: str
    stderr: str
    duration_ms: int
    timed_out: bool
    termination_signal: str | None = None


@dataclasses.dataclass(frozen=True)
class ScenarioExecution:
    scenario_id: str
    status: ScenarioStatus
    process: ProcessResult
    evidence_event_ids: list[str]


@dataclasses.dataclass(frozen=True)
class MatrixExecution:
    status: ScenarioStatus
    scenarios: list[ScenarioExecution]
    output_directory: str


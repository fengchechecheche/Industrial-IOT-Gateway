from __future__ import annotations

import pathlib
import re

from .evidence import EvidenceWriter
from .models import MatrixExecution, ScenarioDefinition, ScenarioExecution, ScenarioStatus
from .process_manager import run_process


_RECOVERY_METRIC = re.compile(
    r"^(?:\d+:\s*)?FAULT_RECOVERY_TIME_MS=(\d+)$", re.MULTILINE
)


def _parse_recovery_time_ms(output: str) -> int | None:
    matches = _RECOVERY_METRIC.findall(output)
    if not matches:
        return None
    return int(matches[-1])


def run_scenarios(
    definitions: list[ScenarioDefinition],
    output_directory: pathlib.Path,
    *,
    run_id: str,
    source_revision: str,
    exploratory: bool,
    stage: str = "S4",
    task: str = "P3-S4-T03",
) -> MatrixExecution:
    writer = EvidenceWriter(
        output_directory,
        run_id=run_id,
        source_revision=source_revision,
        exploratory=exploratory,
        stage=stage,
        task=task,
    )
    writer.begin_run(
        {
            "schema_version": "1.0.0",
            "profile": "software",
            "scenario_ids": [definition.scenario_id for definition in definitions],
        }
    )
    executions: list[ScenarioExecution] = []
    overall = ScenarioStatus.PASS
    for definition in definitions:
        writer.record_command(definition.scenario_id, definition.command)
        started_id = writer.emit_event(definition.scenario_id, "scenario_started", result="running")
        process = run_process(definition.command, timeout_seconds=definition.timeout_seconds)
        writer.append_log(definition.scenario_id, "stdout", process.stdout)
        writer.append_log(definition.scenario_id, "stderr", process.stderr)
        for stream_name, content in (("stdout", process.stdout), ("stderr", process.stderr)):
            for line in content.splitlines():
                writer.emit_event(
                    definition.scenario_id,
                    "process_output",
                    component="scenario_process",
                    severity="info" if stream_name == "stdout" else "warning",
                    result=stream_name,
                    details={"line": line},
                )
        recovery_time_ms = _parse_recovery_time_ms(process.stdout + "\n" + process.stderr)
        recovery_required = bool(definition.config.get("requires_recovery_metric", False))
        recovery_present = not recovery_required or recovery_time_ms is not None
        if process.timed_out:
            status = ScenarioStatus.TIMEOUT
        elif process.returncode == 0 and recovery_present:
            status = ScenarioStatus.PASS
        else:
            status = ScenarioStatus.FAIL
        completed_id = writer.emit_event(
            definition.scenario_id,
            "scenario_completed",
            severity="info" if status == ScenarioStatus.PASS else "error",
            result=status.value,
            details={"returncode": process.returncode, "duration_ms": process.duration_ms},
        )
        oracles = [{
            "oracle_id": f"{definition.scenario_id}.process_exit",
            "passed": not process.timed_out and process.returncode == 0,
            "expected": 0,
            "actual": process.returncode,
        }]
        if recovery_required:
            oracles.append(
                {
                    "oracle_id": f"{definition.scenario_id}.recovery_metric_present",
                    "passed": recovery_time_ms is not None,
                    "expected": "FAULT_RECOVERY_TIME_MS=<non-negative integer>",
                    "actual": recovery_time_ms,
                }
            )
        writer.write_scenario(
            definition.scenario_id,
            {"title": definition.title, **definition.config},
            oracles,
            status,
            recovery_time_ms=recovery_time_ms,
            actual_result=(
                "scenario oracle passed"
                if status == ScenarioStatus.PASS
                else "scenario oracle failed or timed out"
            ),
            process={
                "argv": process.command,
                "returncode": process.returncode,
                "timed_out": process.timed_out,
                "termination_signal": process.termination_signal,
                "duration_ms": process.duration_ms,
            },
            evidence_event_ids=[started_id, completed_id],
        )
        executions.append(
            ScenarioExecution(
                scenario_id=definition.scenario_id,
                status=status,
                process=process,
                evidence_event_ids=[started_id, completed_id],
            )
        )
        if status != ScenarioStatus.PASS:
            overall = ScenarioStatus.FAIL
    writer.finish_run(overall)
    if not writer.verify_checksums():
        overall = ScenarioStatus.ERROR
    return MatrixExecution(overall, executions, str(output_directory))

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import pathlib
import platform
import time
from collections.abc import Mapping, Sequence
from typing import Any

from tools.hardware.g6_t03_contract import (
    EXPECTED_SCENARIO_IDS,
    PROFILE_SCHEMA,
    SCENARIO_SCHEMA,
    ContractError,
    evaluate_baseline,
    evaluate_cleanup_observation,
    evaluate_scenario,
    validate_action_transition,
    validate_profile,
    validate_source_revision,
    validate_reboot_checkpoint,
)
from tools.hardware.g6_t03_preflight import collect_preflight, evaluate_preflight
from tools.hardware.g6_t03_reboot_controller import evaluate_post_reboot


RUN_SCHEMA = "p3-s7-g6-t03-run-v1"


def _utc_now() -> str:
    return dt.datetime.now(dt.UTC).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def _write_json(path: pathlib.Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
    )


class EvidenceStore:
    def __init__(self, run_dir: pathlib.Path, source_revision: str, run_id: str) -> None:
        self.run_dir = run_dir
        self.source_revision = source_revision
        self.run_id = run_id
        self.records: dict[str, dict[str, Any]] = {}

    @classmethod
    def create(
        cls, artifact_root: pathlib.Path, source_revision: str, run_id: str
    ) -> "EvidenceStore":
        validate_source_revision(source_revision)
        if not run_id or any(value in run_id for value in ("/", "\\", "\n", "\r")):
            raise ContractError("run_id must be a non-empty path-safe value")
        run_dir = artifact_root.resolve() / run_id
        run_dir.mkdir(parents=True, exist_ok=False)
        (run_dir / "scenarios").mkdir()
        store = cls(run_dir, source_revision, run_id)
        _write_json(
            run_dir / "manifest.json",
            {
                "schema_version": RUN_SCHEMA,
                "task_id": "P3-S7-G6-T03",
                "run_id": run_id,
                "source_revision": source_revision,
                "started_at_utc": _utc_now(),
                "status": "RUNNING",
            },
        )
        _write_json(
            run_dir / "environment.json",
            {
                "schema_version": RUN_SCHEMA,
                "machine": platform.machine(),
                "platform": platform.platform(),
                "python": platform.python_version(),
            },
        )
        _write_json(
            run_dir / "state.json",
            {
                "schema_version": RUN_SCHEMA,
                "baseline_complete": False,
                "active_scenario": None,
                "active_phase": None,
                "completed": [],
            },
        )
        _write_json(run_dir / "summary.json", {"status": "RUNNING"})
        _write_json(run_dir / "failures.json", [])
        (run_dir / "events.jsonl").write_text("", encoding="utf-8")
        (run_dir / "commands.jsonl").write_text("", encoding="utf-8")
        (run_dir / "IN_PROGRESS").write_text("RUNNING\n", encoding="utf-8")
        return store

    @classmethod
    def load(cls, run_dir: pathlib.Path) -> "EvidenceStore":
        manifest = json.loads((run_dir / "manifest.json").read_text(encoding="utf-8"))
        store = cls(run_dir, str(manifest["source_revision"]), str(manifest["run_id"]))
        for path in sorted((run_dir / "scenarios").glob("*/summary.json")):
            value = json.loads(path.read_text(encoding="utf-8"))
            store.records[str(value["scenario_id"])] = value
        return store

    def record_scenario(self, record: Mapping[str, Any]) -> None:
        scenario_id = record.get("scenario_id")
        if scenario_id not in EXPECTED_SCENARIO_IDS:
            raise ContractError(f"unknown scenario record: {scenario_id!r}")
        if scenario_id in self.records:
            raise ContractError(f"duplicate scenario record: {scenario_id}")
        if record.get("schema_version") != SCENARIO_SCHEMA:
            raise ContractError("scenario record has an unsupported schema")
        if record.get("status") not in {"PASS", "FAIL", "NOT_OBSERVED"}:
            raise ContractError("scenario status is invalid")
        value = dict(record)
        directory = self.run_dir / "scenarios" / str(scenario_id)
        directory.mkdir(parents=True, exist_ok=True)
        _write_json(directory / "summary.json", value)
        _write_json(directory / "failures.json", value.get("failures", []))
        self.records[str(scenario_id)] = value

    def append_event(self, event: Mapping[str, Any]) -> str:
        path = self.run_dir / "events.jsonl"
        with path.open(encoding="utf-8") as stream:
            sequence = sum(1 for _ in stream) + 1
        event_id = f"{self.run_id}:{sequence:06d}"
        value = {
            "schema_version": RUN_SCHEMA,
            "event_id": event_id,
            "run_id": self.run_id,
            "timestamp_utc": _utc_now(),
            "monotonic_ms": time.monotonic_ns() // 1_000_000,
            **dict(event),
        }
        with path.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(value, ensure_ascii=False, sort_keys=True) + "\n")
        return event_id

    def finalize(self, *, cleanup_ok: bool) -> int:
        missing = [value for value in EXPECTED_SCENARIO_IDS if value not in self.records]
        failed = [
            value
            for value, record in self.records.items()
            if record.get("status") != "PASS"
        ]
        failures: list[dict[str, Any]] = []
        for scenario_id in missing:
            failures.append({"scenario_id": scenario_id, "reason": "missing scenario"})
        for scenario_id in failed:
            failures.append(
                {
                    "scenario_id": scenario_id,
                    "reason": "scenario did not pass",
                    "details": self.records[scenario_id].get("failures", []),
                }
            )
        if not cleanup_ok:
            failures.append({"scenario_id": "cleanup", "reason": "cleanup failed"})
        status = "PASS" if not failures else "FAIL"
        _write_json(
            self.run_dir / "summary.json",
            {
                "schema_version": RUN_SCHEMA,
                "status": status,
                "source_revision": self.source_revision,
                "scenario_count": len(self.records),
                "missing_scenarios": missing,
                "failed_scenarios": failed,
                "cleanup_ok": cleanup_ok,
                "unclosed_failures": len(failures),
            },
        )
        _write_json(self.run_dir / "failures.json", failures)
        manifest = json.loads((self.run_dir / "manifest.json").read_text(encoding="utf-8"))
        manifest["ended_at_utc"] = _utc_now()
        manifest["status"] = status
        _write_json(self.run_dir / "manifest.json", manifest)
        for marker in ("PASS", "FAIL", "IN_PROGRESS"):
            (self.run_dir / marker).unlink(missing_ok=True)
        self._write_checksums()
        (self.run_dir / status).write_text(status + "\n", encoding="utf-8")
        return 0 if status == "PASS" else 4

    def _write_checksums(self) -> None:
        lines = []
        for path in sorted(self.run_dir.rglob("*")):
            if not path.is_file() or path.name in {"SHA256SUMS", "PASS", "FAIL", "IN_PROGRESS"}:
                continue
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            lines.append(f"{digest}  {path.relative_to(self.run_dir).as_posix()}")
        (self.run_dir / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="utf-8")

    def verify_checksums(self) -> bool:
        checksum_path = self.run_dir / "SHA256SUMS"
        if not checksum_path.is_file():
            return False
        for line in checksum_path.read_text(encoding="utf-8").splitlines():
            digest, relative = line.split("  ", 1)
            path = self.run_dir / relative
            if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                return False
        return True


def _profile_from_path(path: pathlib.Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ContractError("profile root must be an object")
    validate_profile(value)
    return value


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Manage P3-S7-G6-T03 real-hardware evidence")
    subparsers = parser.add_subparsers(dest="command", required=True)

    initialize = subparsers.add_parser("initialize")
    initialize.add_argument("--artifact-root", type=pathlib.Path, required=True)
    initialize.add_argument("--source-revision", required=True)
    initialize.add_argument("--run-id", required=True)
    initialize.add_argument("--profile", type=pathlib.Path, required=True)

    preflight = subparsers.add_parser("preflight")
    preflight.add_argument("--output", type=pathlib.Path, required=True)

    baseline = subparsers.add_parser("mark-baseline")
    baseline.add_argument("--run-dir", type=pathlib.Path, required=True)
    baseline.add_argument("--observation", type=pathlib.Path, required=True)

    action = subparsers.add_parser("record-action")
    action.add_argument("--run-dir", type=pathlib.Path, required=True)
    action.add_argument("--scenario-id", required=True)
    action.add_argument("--phase", choices=("trigger", "restore", "complete"), required=True)
    action.add_argument("--user-confirmation", required=True)

    evaluate = subparsers.add_parser("evaluate")
    evaluate.add_argument("--run-dir", type=pathlib.Path, required=True)
    evaluate.add_argument("--profile", type=pathlib.Path, required=True)
    evaluate.add_argument("--scenario-id", required=True)
    evaluate.add_argument("--scenario-kind", required=True)
    evaluate.add_argument("--observation", type=pathlib.Path, required=True)

    prepare_reboot = subparsers.add_parser("prepare-reboot")
    prepare_reboot.add_argument("--run-dir", type=pathlib.Path, required=True)
    prepare_reboot.add_argument("--checkpoint-input", type=pathlib.Path, required=True)

    post_reboot = subparsers.add_parser("post-reboot")
    post_reboot.add_argument("--run-dir", type=pathlib.Path, required=True)
    post_reboot.add_argument("--observation", type=pathlib.Path, required=True)

    finalize = subparsers.add_parser("finalize")
    finalize.add_argument("--run-dir", type=pathlib.Path, required=True)
    finalize.add_argument("--cleanup-observation", type=pathlib.Path, required=True)

    arguments = parser.parse_args(list(argv) if argv is not None else None)
    if arguments.command == "preflight":
        observation = collect_preflight()
        result = evaluate_preflight(observation)
        _write_json(arguments.output, {"observation": observation, **result})
        print(f"G6_T03_PREFLIGHT={result['status']}")
        return 0 if result["status"] == "PASS" else 4
    if arguments.command == "initialize":
        store = EvidenceStore.create(arguments.artifact_root, arguments.source_revision, arguments.run_id)
        profile = _profile_from_path(arguments.profile)
        _write_json(store.run_dir / "profile.json", profile)
        print(store.run_dir)
        return 0
    if arguments.command == "mark-baseline":
        state_path = arguments.run_dir / "state.json"
        state = json.loads(state_path.read_text(encoding="utf-8"))
        if state.get("active_scenario") is not None:
            raise ContractError("cannot mark baseline while a scenario is active")
        profile = _profile_from_path(arguments.run_dir / "profile.json")
        observation = json.loads(arguments.observation.read_text(encoding="utf-8"))
        if not isinstance(observation, dict):
            raise ContractError("baseline observation root must be an object")
        result = evaluate_baseline(observation, profile)
        _write_json(arguments.run_dir / "baseline_observation.json", observation)
        _write_json(arguments.run_dir / "baseline_summary.json", result)
        if result["status"] != "PASS":
            return 4
        state["baseline_complete"] = True
        _write_json(state_path, state)
        EvidenceStore.load(arguments.run_dir).append_event(
            {"event_type": "baseline_completed", "result": "success"}
        )
        return 0
    if arguments.command == "record-action":
        state_path = arguments.run_dir / "state.json"
        state = json.loads(state_path.read_text(encoding="utf-8"))
        updated = validate_action_transition(state, arguments.scenario_id, arguments.phase)
        _write_json(state_path, updated)
        EvidenceStore.load(arguments.run_dir).append_event(
            {
                "event_type": "manual_action_confirmed",
                "scenario_id": arguments.scenario_id,
                "phase": arguments.phase,
                "user_confirmation": arguments.user_confirmation,
                "timing_boundary": "confirmation_is_an_upper_bound",
            }
        )
        return 0
    if arguments.command == "evaluate":
        profile = _profile_from_path(arguments.profile)
        observation = json.loads(arguments.observation.read_text(encoding="utf-8"))
        if not isinstance(observation, dict):
            raise ContractError("observation root must be an object")
        record = evaluate_scenario(arguments.scenario_kind, observation, profile)
        record["scenario_id"] = arguments.scenario_id
        record["evidence_event_ids"] = observation.get("evidence_event_ids", [])
        EvidenceStore.load(arguments.run_dir).record_scenario(record)
        return 0 if record["status"] == "PASS" else 4
    if arguments.command == "prepare-reboot":
        store = EvidenceStore.load(arguments.run_dir)
        state = json.loads((arguments.run_dir / "state.json").read_text(encoding="utf-8"))
        prerequisites = set(EXPECTED_SCENARIO_IDS[:-1])
        if not prerequisites.issubset(set(state.get("completed", []))):
            missing = sorted(prerequisites - set(state.get("completed", [])))
            raise ContractError(f"reboot prerequisites are incomplete: {missing}")
        missing_evidence = sorted(
            scenario_id
            for scenario_id in prerequisites
            if store.records.get(scenario_id, {}).get("status") != "PASS"
        )
        if missing_evidence:
            raise ContractError(
                f"reboot prerequisites lack passing evidence: {missing_evidence}"
            )
        checkpoint = json.loads(arguments.checkpoint_input.read_text(encoding="utf-8"))
        if not isinstance(checkpoint, dict):
            raise ContractError("reboot checkpoint root must be an object")
        validate_reboot_checkpoint(checkpoint, store.source_revision)
        updated = validate_action_transition(state, EXPECTED_SCENARIO_IDS[-1], "trigger")
        _write_json(arguments.run_dir / "state.json", updated)
        _write_json(arguments.run_dir / "reboot_checkpoint.json", checkpoint)
        store.append_event(
            {
                "event_type": "reboot_prepared",
                "scenario_id": EXPECTED_SCENARIO_IDS[-1],
                "result": "waiting_for_explicit_authorization",
            }
        )
        return 0
    if arguments.command == "post-reboot":
        store = EvidenceStore.load(arguments.run_dir)
        checkpoint = json.loads(
            (arguments.run_dir / "reboot_checkpoint.json").read_text(encoding="utf-8")
        )
        validate_reboot_checkpoint(checkpoint, store.source_revision)
        observation = json.loads(arguments.observation.read_text(encoding="utf-8"))
        if not isinstance(observation, dict):
            raise ContractError("post-reboot observation root must be an object")
        failures = evaluate_post_reboot(observation, checkpoint)
        record = {
            "schema_version": SCENARIO_SCHEMA,
            "scenario_id": EXPECTED_SCENARIO_IDS[-1],
            "scenario_kind": "reboot",
            "status": "PASS" if not failures else "FAIL",
            "failures": failures,
            "oracles": [
                {
                    "oracle_id": "reboot.complete_recovery",
                    "passed": not failures,
                    "expected": "all frozen reboot checks pass",
                    "actual": failures,
                }
            ],
            "evidence_event_ids": observation.get("evidence_event_ids", []),
        }
        store.record_scenario(record)
        state_path = arguments.run_dir / "state.json"
        state = json.loads(state_path.read_text(encoding="utf-8"))
        restored = validate_action_transition(state, EXPECTED_SCENARIO_IDS[-1], "restore")
        completed = validate_action_transition(restored, EXPECTED_SCENARIO_IDS[-1], "complete")
        _write_json(state_path, completed)
        _write_json(arguments.run_dir / "post_reboot_observation.json", observation)
        return 0 if not failures else 4
    store = EvidenceStore.load(arguments.run_dir)
    observation = json.loads(arguments.cleanup_observation.read_text(encoding="utf-8"))
    if not isinstance(observation, dict):
        raise ContractError("cleanup observation root must be an object")
    cleanup = evaluate_cleanup_observation(observation)
    _write_json(arguments.run_dir / "cleanup_observation.json", cleanup)
    return store.finalize(cleanup_ok=cleanup["status"] == "PASS")


if __name__ == "__main__":
    raise SystemExit(main())

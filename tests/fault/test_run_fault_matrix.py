import json
import pathlib
import sys
import tempfile
import time
import unittest


REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from tools.fault_matrix.evidence import EvidenceWriter, validate_event
from tools.fault_matrix.models import ScenarioDefinition, ScenarioStatus
from tools.fault_matrix.process_manager import run_process
from tools.fault_matrix.runner import run_scenarios


class EventSchemaTest(unittest.TestCase):
    def test_rejects_missing_required_fields(self):
        with self.assertRaises(ValueError):
            validate_event({"event_id": "event-1"})

    def test_accepts_complete_event(self):
        validate_event(
            {
                "schema_version": "1.0.0",
                "event_id": "run:F01:000001",
                "run_id": "run",
                "scenario_id": "F01",
                "timestamp_utc": "2026-08-09T00:00:00.000Z",
                "monotonic_ms": 0,
                "component": "runner",
                "event_type": "scenario_started",
                "severity": "info",
                "result": "running",
                "queue_depths": {},
            }
        )


class ProcessManagerTest(unittest.TestCase):
    def test_timeout_terminates_and_waits_for_child(self):
        started = time.monotonic()
        result = run_process(
            [sys.executable, "-c", "import time; time.sleep(10)"],
            timeout_seconds=0.1,
            termination_grace_seconds=0.2,
        )
        self.assertTrue(result.timed_out)
        self.assertIsNotNone(result.returncode)
        self.assertLess(time.monotonic() - started, 3.0)


class EvidenceWriterTest(unittest.TestCase):
    def test_records_requested_stage_and_task_in_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = pathlib.Path(temporary) / "run"
            writer = EvidenceWriter(
                output,
                run_id="20260812T000000Z_g3_abcdef0_001",
                source_revision="abcdef0",
                exploratory=False,
                stage="S5",
                task="P3-S5-T01",
            )
            writer.begin_run({"profile": "software"})
            writer.finish_run(ScenarioStatus.PASS)

            manifest = json.loads((output / "manifest.json").read_text())
            self.assertEqual(manifest["gate"], "G3")
            self.assertEqual(manifest["stage"], "S5")
            self.assertEqual(manifest["task"], "P3-S5-T01")

    def test_writes_required_files_and_valid_checksums(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = pathlib.Path(temporary) / "run"
            writer = EvidenceWriter(
                output,
                run_id="20260809T000000Z_g3_abcdef0_001",
                source_revision="abcdef0",
                exploratory=True,
            )
            writer.begin_run({"profile": "software"})
            writer.write_scenario(
                "F01",
                {"fault": "silent"},
                [{"oracle_id": "F01.timeout", "passed": True}],
                ScenarioStatus.PASS,
                recovery_time_ms=120,
            )
            writer.finish_run(ScenarioStatus.PASS)

            for name in (
                "manifest.json",
                "config.yaml",
                "environment.json",
                "commands.txt",
                "events.jsonl",
                "summary.json",
                "failures.json",
                "run.log",
                "SHA256SUMS",
            ):
                self.assertTrue((output / name).is_file(), name)
            for name in ("config.yaml", "events.jsonl", "summary.json", "failures.json"):
                self.assertTrue((output / "scenarios" / "F01" / name).is_file(), name)
            self.assertEqual(json.loads((output / "failures.json").read_text()), [])
            self.assertTrue(writer.verify_checksums())


class RunnerTest(unittest.TestCase):
    def test_runs_all_scenarios_and_returns_failure_if_one_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            definitions = [
                ScenarioDefinition(
                    scenario_id="F01",
                    title="pass",
                    command=[sys.executable, "-c", "print('pass')"],
                    timeout_seconds=2.0,
                ),
                ScenarioDefinition(
                    scenario_id="F02",
                    title="fail",
                    command=[sys.executable, "-c", "raise SystemExit(7)"],
                    timeout_seconds=2.0,
                ),
            ]
            result = run_scenarios(
                definitions,
                pathlib.Path(temporary) / "run",
                run_id="20260809T000000Z_g3_abcdef0_001",
                source_revision="abcdef0",
                exploratory=True,
            )
            self.assertEqual(len(result.scenarios), 2)
            self.assertEqual(result.status, ScenarioStatus.FAIL)
            self.assertTrue((pathlib.Path(temporary) / "run" / "scenarios" / "F02").is_dir())

    def test_records_required_recovery_metric(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = pathlib.Path(temporary) / "run"
            definition = ScenarioDefinition(
                scenario_id="F06",
                title="recovery",
                command=[sys.executable, "-c", "print('160: FAULT_RECOVERY_TIME_MS=42')"],
                timeout_seconds=2.0,
                config={"requires_recovery_metric": True, "expected": "recover"},
            )
            result = run_scenarios(
                [definition],
                output,
                run_id="20260809T000000Z_g3_abcdef0_001",
                source_revision="abcdef0",
                exploratory=True,
            )
            self.assertEqual(result.status, ScenarioStatus.PASS)
            summary = json.loads((output / "scenarios" / "F06" / "summary.json").read_text())
            self.assertEqual(summary["recovery_time_ms"], 42)
            self.assertEqual(summary["actual_result"], "scenario oracle passed")

    def test_fails_when_required_recovery_metric_is_missing(self):
        with tempfile.TemporaryDirectory() as temporary:
            definition = ScenarioDefinition(
                scenario_id="F10",
                title="missing recovery",
                command=[sys.executable, "-c", "print('no metric')"],
                timeout_seconds=2.0,
                config={"requires_recovery_metric": True},
            )
            result = run_scenarios(
                [definition],
                pathlib.Path(temporary) / "run",
                run_id="20260809T000000Z_g3_abcdef0_001",
                source_revision="abcdef0",
                exploratory=True,
            )
            self.assertEqual(result.status, ScenarioStatus.FAIL)


if __name__ == "__main__":
    unittest.main()

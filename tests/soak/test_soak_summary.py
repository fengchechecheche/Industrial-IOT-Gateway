from __future__ import annotations

import json
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.soak.profile import load_and_validate_profile  # noqa: E402
from tools.soak.summary import summarize  # noqa: E402


def write_json(path: pathlib.Path, value: object) -> None:
    path.write_text(json.dumps(value) + "\n", encoding="utf-8")


class SoakSummaryUnitTest(unittest.TestCase):
    def make_evidence(self, directory: pathlib.Path, *, stopped: bool = True) -> None:
        profile = load_and_validate_profile(
            ROOT / "tests/data/soak_profiles/software_smoke.json", ROOT
        )
        write_json(directory / "profile.json", profile)
        write_json(
            directory / "manifest.json",
            {"run_id": "unit", "status": "PASS", "monotonic_duration_seconds": 90},
        )
        faults = [fault for fault in profile["faults"] if fault["actor"] == "driver"]
        driver = [{"event": "soak_driver_started", "monotonic_ms": 1000}]
        for fault in faults:
            driver.extend(
                [
                    {
                        "event": "fault_started",
                        "fault_id": fault["fault_id"],
                        "cycle": 0,
                        "monotonic_ms": 2000,
                    },
                    {"event": "fault_cleared", "fault_id": fault["fault_id"], "monotonic_ms": 2800},
                    {"event": "fault_recovered", "fault_id": fault["fault_id"], "monotonic_ms": 3000},
                ]
            )
        queue = {"capacity": 16, "maximum_depth": 2, "full": 0}
        publisher = {
            "connected_events": 2,
            "disconnected_events": 1,
            "critical_enqueue_failures": 0,
            "drain_expired": 0,
            "unconfirmed_on_close": 0,
            "dropped": 0,
            "expired_fresh_dropped": 0,
        }
        driver.append(
            {
                "event": "soak_driver_stopped",
                "monotonic_ms": 91000,
                "shutdown_duration_ms": 10,
                "internal_failure": False,
                "statistics": {
                    "stopped": stopped,
                    "request_queue": queue,
                    "measurement_queue": queue,
                    "publish_queue": queue,
                    "measurement_enqueue_failures": 0,
                    "publisher": publisher,
                },
            }
        )
        (directory / "driver_0001.jsonl").write_text(
            "".join(json.dumps(item) + "\n" for item in driver), encoding="utf-8"
        )
        gateway_events = []
        for fault in faults:
            gateway_events.append(
                {
                    "event": "request_completed",
                    "monotonic_ms": 2500,
                    "slave_id": 3,
                    "result": fault["expected_category"],
                    "duration_ms": 1,
                }
            )
            if fault["kind"] == "delayed_response":
                gateway_events.append(
                    {
                        "event": "late_response_discarded",
                        "monotonic_ms": 2600,
                        "slave_id": 3,
                        "result": "response_timeout",
                        "duration_ms": 152,
                    }
                )
        (directory / "gateway_0001.jsonl").write_text(
            "".join(json.dumps(item) + "\n" for item in gateway_events), encoding="utf-8"
        )
        (directory / "resource_samples.jsonl").write_text(
            json.dumps(
                {
                    "elapsed_seconds": 20,
                    "rss_mib": 10,
                    "cpu_percent_single_core": 1,
                    "fd_count": 4,
                   "thread_count": 5,
                    "disk_free_bytes": 10 * 1024**3,
                    "evidence_bytes": 1024,
                }
            )
            + "\n",
            encoding="utf-8",
        )
        runner = [
            {
                "event_type": "broker_fault_started",
                "details": {"fault_id": "broker_stop", "cycle": 0},
            },
            {
                "event_type": "broker_fault_recovered",
                "details": {"fault_id": "broker_stop", "cycle": 0},
            },
        ]
        (directory / "events.jsonl").write_text(
            "".join(json.dumps(item) + "\n" for item in runner), encoding="utf-8"
        )

    def test_smoke_summary_passes_but_does_not_claim_long_soak(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            self.make_evidence(directory)
            summary = summarize(directory)
            self.assertEqual(summary["status"], "PASS")
            self.assertFalse(summary["long_soak_pass"])

    def test_shutdown_cancellation_is_known_and_excluded_from_performance(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            self.make_evidence(directory)
            gateway_path = directory / "gateway_0001.jsonl"
            gateway = [json.loads(line) for line in gateway_path.read_text().splitlines()]
            gateway.extend(
                [
                    {
                        "event": "request_completed",
                        "monotonic_ms": 90500,
                        "slave_id": 1,
                        "result": "success",
                        "duration_ms": 1,
                    },
                    {
                        "event": "request_completed",
                        "monotonic_ms": 90700,
                        "slave_id": 2,
                        "result": "shutdown_cancelled",
                        "duration_ms": 1,
                    },
                ]
            )
            gateway_path.write_text(
                "".join(json.dumps(item) + "\n" for item in gateway), encoding="utf-8"
            )

            summary = summarize(directory)
            oracles = {item["oracle_id"]: item for item in summary["oracles"]}

            self.assertEqual(oracles["requests.unclassified_errors"]["actual"], 0)
            self.assertEqual(oracles["requests.normal_success_rate"]["actual"], 1.0)
            self.assertEqual(summary["status"], "PASS")

    def test_failed_stop_flag_returns_non_pass(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            self.make_evidence(directory, stopped=False)
            summary = summarize(directory)
            self.assertEqual(summary["status"], "FAIL")
            self.assertGreater(summary["unclosed_failures"], 0)

    def test_delayed_fault_requires_late_response_discard_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            self.make_evidence(directory)
            gateway_path = directory / "gateway_0001.jsonl"
            gateway = [json.loads(line) for line in gateway_path.read_text().splitlines()]
            gateway_path.write_text(
                "".join(
                    json.dumps(item) + "\n"
                    for item in gateway
                    if item.get("event") != "late_response_discarded"
                ),
                encoding="utf-8",
            )

            summary = summarize(directory)

            self.assertEqual(summary["status"], "FAIL")
            failed_oracles = {
                oracle["oracle_id"]
                for oracle in summary["oracles"]
                if oracle["enforced"] and not oracle["passed"]
            }
            self.assertIn("fault.delayed.cycle_0.late_response_discarded", failed_oracles)

    def test_missing_second_fault_cycle_returns_non_pass(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            self.make_evidence(directory)
            profile = json.loads((directory / "profile.json").read_text(encoding="utf-8"))
            profile["duration_seconds"] = 180
            profile["fault_cycle_count"] = 2
            write_json(directory / "profile.json", profile)
            driver_path = directory / "driver_0001.jsonl"
            driver = [json.loads(line) for line in driver_path.read_text().splitlines()]
            driver[-1]["monotonic_ms"] = 181000
            driver_path.write_text(
                "".join(json.dumps(item) + "\n" for item in driver), encoding="utf-8"
            )
            summary = summarize(directory)
            self.assertEqual(summary["status"], "FAIL")
            failed_oracles = {
                oracle["oracle_id"]
                for oracle in summary["oracles"]
                if oracle["enforced"] and not oracle["passed"]
            }
            self.assertIn("fault.silent.cycle_1.triggered", failed_oracles)


if __name__ == "__main__":
    unittest.main()

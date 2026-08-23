from __future__ import annotations

import copy
import json
import pathlib
import tempfile
import unittest

from tools.hardware.g6_t04_contract import (
    ContractError,
    EXPECTED_PROFILE_IDS,
    build_indices,
    evaluate_run,
    load_profile,
    validate_profile,
    validate_source_revision,
)


ROOT = pathlib.Path(__file__).resolve().parents[2]


def profile(kind: str = "preflight") -> dict:
    name = f"g6_t04_{kind}_profile.json"
    return json.loads((ROOT / "config/hardware" / name).read_text(encoding="utf-8"))


def passing_observation(kind: str = "preflight") -> dict:
    selected = profile(kind)
    start = 1_000_000
    end = start + selected["duration_seconds"] * 1000
    events = []
    request_id = 1
    task_rows = [
        (1, 3, 0), (1, 3, 1), (2, 3, 0), (2, 3, 1),
        *[(4, 4, address) for address in selected["task_addresses"]["4"]],
    ]
    for second in range(selected["warmup_seconds"], selected["duration_seconds"], 4):
        for slave_id, function, address in task_rows:
            events.append(
                {
                    "event": "request_completed",
                    "request_id": request_id,
                    "attempt": 1,
                    "slave_id": slave_id,
                    "function": function,
                    "address": address,
                    "result": "success",
                    "duration_ms": 8,
                    "monotonic_ms": start + second * 1000,
                }
            )
            request_id += 1
    mqtt = []
    sequence = 1
    for event in events:
        if event["result"] != "success":
            continue
        mqtt.append(
            {
                "received_monotonic_ms": event["monotonic_ms"] + 20,
                "topic": f"industrial_iot_gateway/devices/device_{event['slave_id']}/registers/{event['address']}",
                "qos": 1,
                "retain": False,
                "payload": {
                    "run_id": "run-1",
                    "sequence": sequence,
                    "quality": "fresh",
                    "slave_id": event["slave_id"],
                },
            }
        )
        sequence += 1
    resources = []
    for second in range(0, selected["duration_seconds"] + 1, 5):
        resources.append(
            {
                "elapsed_seconds": second,
                "rss_mib": 40.0 + second / 100000.0,
                "cpu_percent_single_core": 7.0,
                "fd_count": 14,
                "thread_count": 8,
                "disk_free_bytes": 30 * 1024**3,
                "evidence_bytes": min(second * 1024, 1024**3),
                "soc_temperature_c": 51.0,
                "throttled_current_bits": 0,
                "throttled_history_new_bits": 0,
                "host_boot_id": "boot-a",
                "gateway_active": True,
                "mosquitto_active": True,
                "gateway_nrestarts": 0,
                "mosquitto_nrestarts": 0,
                "main_pid": 123,
                "invocation_id": "inv-a",
            }
        )
    return {
        "source_revision": "a" * 40,
        "run_id": "run-1",
        "started_monotonic_ms": start,
        "ended_monotonic_ms": end,
        "gateway_events": events,
        "mqtt_messages": mqtt,
        "resource_samples": resources,
        "gateway_summary": {
            "event": "gateway_summary",
            "scheduler_feedback_delivery_failures": 0,
            "scheduler_transition_errors": 0,
            "mqtt_publish_failures": 0,
            "scheduler_feedback_queue_high_water": 3,
            "mqtt_queue_high_water": 5,
            "serial_open_successes": 1,
            "mqtt_dropped": 0,
            "mqtt_drain_expired": 0,
            "stopped": True,
        },
        "shutdown_duration_ms": 900,
        "collector_failures": [],
        "collection_gaps": [],
        "serial_reopen_events_after_warmup": 0,
        "unexpected_exits": 0,
        "final_evidence_bytes": 1024**3,
    }


class G6T04ContractTests(unittest.TestCase):
    def test_profiles_freeze_preflight_and_release_contracts(self) -> None:
        for kind, duration, warmup in (("preflight", 3600, 300), ("release", 28800, 600)):
            selected = profile(kind)
            validate_profile(selected)
            self.assertEqual(selected["profile_id"], EXPECTED_PROFILE_IDS[kind])
            self.assertEqual(selected["duration_seconds"], duration)
            self.assertEqual(selected["warmup_seconds"], warmup)
            self.assertEqual(selected["thresholds"]["evidence_max_bytes"], 10 * 1024**3)
            self.assertTrue(selected["evidence"]["retain_all_gateway_events"])
            self.assertTrue(selected["evidence"]["retain_all_mqtt_messages"])
            self.assertFalse(selected["evidence"]["allow_dynamic_downsampling"])

    def test_profile_rejects_weakened_capacity_and_sampling_contract(self) -> None:
        selected = profile()
        selected["thresholds"]["evidence_max_bytes"] = 11 * 1024**3
        with self.assertRaisesRegex(ContractError, "thresholds"):
            validate_profile(selected)
        selected = profile()
        selected["evidence"]["allow_dynamic_downsampling"] = True
        with self.assertRaisesRegex(ContractError, "evidence"):
            validate_profile(selected)

    def test_full_lowercase_revision_is_required(self) -> None:
        validate_source_revision("0" * 40)
        for value in ("0" * 7, "A" * 40, "uncommitted"):
            with self.assertRaises(ContractError):
                validate_source_revision(value)

    def test_load_profile_rejects_path_outside_repository(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "profile.json"
            path.write_text(json.dumps(profile()), encoding="utf-8")
            with self.assertRaisesRegex(ContractError, "config/hardware"):
                load_profile(path, ROOT)

    def test_passing_preflight_has_no_long_soak_claim(self) -> None:
        result = evaluate_run(profile(), passing_observation())
        self.assertEqual(result["status"], "PASS")
        self.assertFalse(result["hardware_long_soak_pass"])
        self.assertEqual(result["unclosed_failures"], 0)
        self.assertEqual(set(result["slaves"]), {"1", "2", "4"})
        self.assertEqual(result["task_coverage"]["covered"], 21)

    def test_passing_release_sets_hardware_long_soak(self) -> None:
        result = evaluate_run(profile("release"), passing_observation("release"))
        self.assertEqual(result["status"], "PASS")
        self.assertTrue(result["hardware_long_soak_pass"])

    def test_failed_request_rate_and_gap_are_not_hidden(self) -> None:
        observation = passing_observation()
        start = observation["started_monotonic_ms"] + profile()["warmup_seconds"] * 1000
        observation["gateway_events"].extend(
            [
                {
                    "event": "request_completed",
                    "request_id": 999000 + offset,
                    "attempt": 1,
                    "slave_id": 1,
                    "function": 3,
                    "address": 0,
                    "result": "response_timeout",
                    "duration_ms": 500,
                    "monotonic_ms": start + offset,
                }
                for offset in range(100, 20_000, 100)
            ]
        )
        result = evaluate_run(profile(), observation)
        self.assertEqual(result["status"], "FAIL")
        failed = {item["oracle_id"] for item in result["oracles"] if not item["passed"]}
        self.assertIn("requests.slave_1.success_rate", failed)

    def test_retry_attempts_are_reduced_to_one_logical_terminal_result(self) -> None:
        observation = passing_observation()
        start = observation["started_monotonic_ms"] + profile()["warmup_seconds"] * 1000
        observation["gateway_events"].extend(
            [
                {
                    "event": "request_completed", "request_id": 888001, "attempt": 1,
                    "slave_id": 1, "function": 3, "address": 0,
                    "result": "response_timeout", "duration_ms": 500,
                    "monotonic_ms": start + 100,
                },
                {
                    "event": "request_completed", "request_id": 888001, "attempt": 2,
                    "slave_id": 1, "function": 3, "address": 0,
                    "result": "success", "duration_ms": 8,
                    "monotonic_ms": start + 200,
                },
            ]
        )
        result = evaluate_run(profile(), observation)
        self.assertEqual(result["slaves"]["1"]["logical_attempt_retries"], 1)
        self.assertEqual(result["status"], "PASS")

    def test_old_mqtt_run_id_and_final_derived_capacity_fail(self) -> None:
        observation = passing_observation()
        observation["mqtt_messages"][0]["payload"]["run_id"] = "old-run"
        observation["final_evidence_bytes"] = 10 * 1024**3 + 1
        result = evaluate_run(profile(), observation)
        failed = {item["oracle_id"] for item in result["oracles"] if not item["passed"]}
        self.assertIn("mqtt.run_id_mismatch", failed)
        self.assertIn("evidence.maximum_bytes", failed)

    def test_collection_gap_and_over_capacity_fail(self) -> None:
        observation = passing_observation()
        observation["collection_gaps"] = [{"stream": "mqtt", "reason": "collector_pause"}]
        observation["resource_samples"][-1]["evidence_bytes"] = 10 * 1024**3 + 1
        result = evaluate_run(profile(), observation)
        failed = {item["oracle_id"] for item in result["oracles"] if not item["passed"]}
        self.assertIn("evidence.collection_gaps", failed)
        self.assertIn("evidence.maximum_bytes", failed)

    def test_indices_keep_source_file_and_line(self) -> None:
        events = [
            {
                "source_file": "gateway_0001.jsonl",
                "source_line": 7,
                "event": "request_completed",
                "request_id": 42,
                "slave_id": 1,
                "function": 3,
                "address": 0,
                "result": "success",
                "monotonic_ms": 1234,
            }
        ]
        mqtt = [
            {
                "source_file": "mqtt_messages_0001.jsonl",
                "source_line": 9,
                "topic": "industrial_iot_gateway/devices/tas_env_01/registers/relative_humidity",
                "received_monotonic_ms": 1250,
                "payload": {"sequence": 5, "quality": "fresh", "slave_id": 1},
            }
        ]
        indices = build_indices(events, mqtt, [])
        self.assertEqual(indices["requests"][0]["source_line"], 7)
        self.assertEqual(indices["mqtt"][0]["source_line"], 9)
        self.assertEqual([row["kind"] for row in indices["timeline"]], ["request", "mqtt"])

    def test_input_is_not_mutated_by_evaluation(self) -> None:
        observation = passing_observation()
        frozen = copy.deepcopy(observation)
        evaluate_run(profile(), observation)
        self.assertEqual(observation, frozen)


if __name__ == "__main__":
    unittest.main()

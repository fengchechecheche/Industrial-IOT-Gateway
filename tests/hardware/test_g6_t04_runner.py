from __future__ import annotations

import hashlib
import json
import pathlib
import tempfile
import unittest
from unittest import mock

from tools.hardware.g6_t04_runner import (
    EvidenceStore,
    SegmentIndex,
    parse_journal_line,
    parse_mqtt_line,
    safe_output_root,
)


class G6T04RunnerTests(unittest.TestCase):
    def test_output_root_must_remain_under_hardware_artifacts(self) -> None:
        root = pathlib.Path("/repo").resolve()
        self.assertEqual(
            safe_output_root(root / "artifacts/hardware/g6/t04", root),
            (root / "artifacts/hardware/g6/t04").resolve(),
        )
        with self.assertRaisesRegex(ValueError, "artifacts/hardware/g6/t04"):
            safe_output_root(root / "artifacts/other", root)

    def test_fixed_raspberry_pi_run_root_is_allowed(self) -> None:
        root = pathlib.Path("/repo").resolve()
        revision = "a" * 40
        target = pathlib.Path("/home/iot-rp/industrial_iot_gateway_runs/g6/t04") / revision
        self.assertEqual(safe_output_root(target, root), target.resolve())

    def test_journal_parser_preserves_raw_metadata_and_payload(self) -> None:
        value = parse_journal_line(
            json.dumps(
                {
                    "__CURSOR": "cursor-a",
                    "__REALTIME_TIMESTAMP": "123000",
                    "_SYSTEMD_INVOCATION_ID": "inv-a",
                    "MESSAGE": json.dumps(
                        {
                            "event": "request_completed",
                            "slave_id": 1,
                            "result": "success",
                            "monotonic_ms": 55,
                        }
                    ),
                }
            )
        )
        self.assertEqual(value["journal_cursor"], "cursor-a")
        self.assertEqual(value["invocation_id"], "inv-a")
        self.assertEqual(value["event"], "request_completed")
        self.assertEqual(value["journal_raw"]["__REALTIME_TIMESTAMP"], "123000")

    def test_mqtt_parser_preserves_topic_and_payload(self) -> None:
        value = parse_mqtt_line(
            'industrial_iot_gateway/devices/tas_env_01/registers/relative_humidity '
            '{"run_id":"r","sequence":9,"quality":"fresh"}',
            received_monotonic_ms=800,
        )
        self.assertEqual(value["payload"]["sequence"], 9)
        self.assertEqual(value["received_monotonic_ms"], 800)
        self.assertFalse(value["retain"])
        self.assertEqual(value["qos"], 1)

    def test_segment_index_records_hash_count_and_bounds(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            path = root / "gateway_0001.jsonl"
            path.write_text('{"event_id":"a","monotonic_ms":1}\n{"event_id":"b","monotonic_ms":2}\n', encoding="utf-8")
            result = SegmentIndex.from_jsonl(path, root)
            self.assertEqual(result.record_count, 2)
            self.assertEqual(result.first_event_id, "a")
            self.assertEqual(result.last_event_id, "b")
            self.assertEqual(result.sha256, hashlib.sha256(path.read_bytes()).hexdigest())

    def test_evidence_finalize_writes_indices_derived_and_checksums(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            store = EvidenceStore.create(
                root,
                source_revision="a" * 40,
                run_id="run-a",
                profile={"profile_id": "g6_t04_hardware_preflight_v1"},
            )
            store.gateway_writer.write(
                json.dumps(
                    {
                        "event": "request_completed",
                        "request_id": 1,
                        "slave_id": 1,
                        "function": 3,
                        "address": 0,
                        "result": "success",
                        "monotonic_ms": 100,
                    }
                )
                + "\n"
            )
            store.mqtt_writer.write(
                json.dumps(
                    {
                        "topic": "industrial_iot_gateway/devices/a/registers/b",
                        "received_monotonic_ms": 120,
                        "payload": {"sequence": 1, "quality": "fresh", "slave_id": 1},
                    }
                )
                + "\n"
            )
            store.resource_writer.write(
                json.dumps({"elapsed_seconds": 5, "rss_mib": 10, "cpu_percent_single_core": 2})
                + "\n"
            )
            store.close_writers()
            store.write_traceability_outputs()
            self.assertTrue((store.run_dir / "file_index.json").is_file())
            self.assertTrue((store.run_dir / "request_index.jsonl").is_file())
            self.assertTrue((store.run_dir / "derived/request_metrics_1m.csv").is_file())
            store.write_checksums()
            self.assertTrue(store.verify_checksums())

    def test_create_refuses_existing_run_directory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            EvidenceStore.create(
                root,
                source_revision="a" * 40,
                run_id="same",
                profile={"profile_id": "g6_t04_hardware_preflight_v1"},
            ).close_writers()
            with self.assertRaises(FileExistsError):
                EvidenceStore.create(
                    root,
                    source_revision="a" * 40,
                    run_id="same",
                    profile={"profile_id": "g6_t04_hardware_preflight_v1"},
                )

    @mock.patch("tools.hardware.g6_t04_runner.pathlib.Path.is_symlink", return_value=True)
    def test_symlink_artifact_root_is_rejected(self, _: mock.Mock) -> None:
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "symbolic link"):
                safe_output_root(pathlib.Path(directory), pathlib.Path(directory))


if __name__ == "__main__":
    unittest.main()

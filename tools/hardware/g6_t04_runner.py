from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import os
import pathlib
import platform
import shutil
import statistics
import subprocess
import threading
import time
from dataclasses import asdict, dataclass
from typing import Any, TextIO

from tools.hardware.g6_t04_contract import (
    ContractError,
    evaluate_run,
    load_profile,
    validate_source_revision,
)
from tools.soak.evidence import RotatingTextWriter, write_json_atomic
from tools.soak.monitor import Arm64HostSampler, ProcSampler


RUN_SCHEMA = "p3-s7-g6-t04-run-v1"
RASPBERRY_PI_ARTIFACT_ROOT = pathlib.Path(
    "/home/iot-rp/industrial_iot_gateway_runs/g6/t04"
)


def utc_now() -> str:
    return dt.datetime.now(dt.UTC).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def safe_output_root(path: pathlib.Path, repository_root: pathlib.Path) -> pathlib.Path:
    if path.is_symlink():
        raise ValueError("artifact root must not be a symbolic link")
    resolved = path.resolve()
    allowed_roots = (
        (repository_root / "artifacts/hardware/g6/t04").resolve(),
        RASPBERRY_PI_ARTIFACT_ROOT.resolve(),
    )
    if not any(resolved == allowed or resolved.is_relative_to(allowed) for allowed in allowed_roots):
        raise ValueError(
            "output root must remain under artifacts/hardware/g6/t04 or the fixed Raspberry Pi run root"
        )
    return resolved


def parse_journal_line(line: str) -> dict[str, Any]:
    outer = json.loads(line)
    if not isinstance(outer, dict):
        raise ValueError("journal record must be an object")
    message = outer.get("MESSAGE", "")
    try:
        payload = json.loads(message) if isinstance(message, str) else {}
    except json.JSONDecodeError:
        payload = {"event": "unparsed_journal_message", "message": message}
    if not isinstance(payload, dict):
        payload = {"event": "unparsed_journal_message", "message": message}
    return {
        "journal_cursor": outer.get("__CURSOR"),
        "journal_realtime_us": outer.get("__REALTIME_TIMESTAMP"),
        "invocation_id": outer.get("_SYSTEMD_INVOCATION_ID"),
        "journal_raw": outer,
        **payload,
    }


def parse_mqtt_line(line: str, *, received_monotonic_ms: int) -> dict[str, Any]:
    stripped = line.rstrip("\r\n")
    fields = stripped.split("\t", 3)
    retain = False
    qos = 1
    if len(fields) == 4 and fields[0] in {"0", "1"} and fields[1].isdigit():
        retain = fields[0] == "1"
        qos = int(fields[1])
        topic, payload_text = fields[2], fields[3]
    elif " " in stripped:
        topic, payload_text = stripped.split(" ", 1)
    else:
        raise ValueError("MQTT record does not contain topic and payload")
    payload = json.loads(payload_text)
    if not isinstance(payload, dict):
        raise ValueError("MQTT payload must be a JSON object")
    return {
        "received_at_utc": utc_now(),
        "received_monotonic_ms": received_monotonic_ms,
        "topic": topic,
        "qos": qos,
        "retain": retain,
        "payload_text": payload_text,
        "payload": payload,
    }


def journal_collector_argv(invocation_id: str) -> list[str]:
    if not invocation_id:
        raise ValueError("systemd invocation id must not be empty")
    return [
        "journalctl",
        f"_SYSTEMD_INVOCATION_ID={invocation_id}",
        "--follow",
        "--lines=all",
        "--output=json",
        "--no-pager",
    ]


def _gateway_run_id(mqtt: list[dict[str, Any]]) -> str | None:
    values = {
        str(payload["run_id"])
        for row in mqtt
        for payload in [row.get("payload")]
        if row.get("retain") is False
        and isinstance(payload, dict)
        and payload.get("message_type", "telemetry") == "telemetry"
        and isinstance(payload.get("run_id"), str)
        and payload["run_id"]
    }
    return next(iter(values)) if len(values) == 1 else None


def _json_lines(paths: list[pathlib.Path]) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    for path in paths:
        with path.open(encoding="utf-8") as stream:
            for line_number, line in enumerate(stream, 1):
                if not line.strip():
                    continue
                value = json.loads(line)
                if isinstance(value, dict):
                    value.setdefault("source_file", path.name)
                    value.setdefault("source_line", line_number)
                    output.append(value)
    return output


def _write_jsonl(path: pathlib.Path, values: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as stream:
        for value in values:
            stream.write(json.dumps(value, ensure_ascii=False, sort_keys=True) + "\n")


@dataclass(frozen=True)
class SegmentIndex:
    path: str
    record_count: int
    bytes: int
    sha256: str
    first_event_id: object
    last_event_id: object
    first_monotonic_ms: object
    last_monotonic_ms: object

    @classmethod
    def from_jsonl(cls, path: pathlib.Path, root: pathlib.Path) -> "SegmentIndex":
        count = 0
        first: dict[str, Any] | None = None
        last: dict[str, Any] | None = None
        with path.open(encoding="utf-8") as stream:
            for line in stream:
                if not line.strip():
                    continue
                value = json.loads(line)
                if not isinstance(value, dict):
                    continue
                count += 1
                first = first or value
                last = value
        first = first or {}
        last = last or {}
        return cls(
            path=path.relative_to(root).as_posix(),
            record_count=count,
            bytes=path.stat().st_size,
            sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            first_event_id=first.get("event_id"),
            last_event_id=last.get("event_id"),
            first_monotonic_ms=first.get("monotonic_ms", first.get("received_monotonic_ms")),
            last_monotonic_ms=last.get("monotonic_ms", last.get("received_monotonic_ms")),
        )


class EvidenceStore:
    def __init__(self, run_dir: pathlib.Path, profile: dict[str, Any]) -> None:
        self.run_dir = run_dir
        maximum = int(profile.get("thresholds", {}).get("segment_max_bytes", 64 * 1024**2))
        self.gateway_writer = RotatingTextWriter(run_dir, "gateway", "jsonl", maximum)
        self.mqtt_writer = RotatingTextWriter(run_dir, "mqtt_messages", "jsonl", maximum)
        self.resource_writer = RotatingTextWriter(run_dir, "resource_samples", "jsonl", maximum)
        self.collector_writer = RotatingTextWriter(run_dir, "collector", "log", maximum)
        self._writers_closed = False

    @classmethod
    def create(
        cls,
        artifact_root: pathlib.Path,
        *,
        source_revision: str,
        run_id: str,
        profile: dict[str, Any],
    ) -> "EvidenceStore":
        validate_source_revision(source_revision)
        if not run_id or any(value in run_id for value in ("/", "\\", "\n", "\r")):
            raise ValueError("run_id must be a non-empty path-safe value")
        run_dir = artifact_root.resolve() / run_id
        run_dir.mkdir(parents=True, exist_ok=False)
        (run_dir / "derived").mkdir()
        write_json_atomic(run_dir / "profile.json", profile)
        write_json_atomic(
            run_dir / "manifest.json",
            {
                "schema_version": RUN_SCHEMA,
                "task_id": "P3-S7-G6-T04",
                "run_id": run_id,
                "evidence_run_id": run_id,
                "source_revision": source_revision,
                "profile_id": profile["profile_id"],
                "profile_kind": profile.get("profile_kind"),
                "started_at_utc": utc_now(),
                "status": "RUNNING",
            },
        )
        write_json_atomic(
            run_dir / "environment.json",
            {
                "schema_version": RUN_SCHEMA,
                "machine": platform.machine(),
                "platform": platform.platform(),
                "python": platform.python_version(),
            },
        )
        write_json_atomic(run_dir / "summary.json", {"status": "RUNNING"})
        write_json_atomic(run_dir / "failures.json", [])
        for name in ("events.jsonl", "commands.jsonl", "heartbeats.jsonl", "system_snapshots.jsonl"):
            (run_dir / name).write_text("", encoding="utf-8")
        (run_dir / "RUNNING").write_text("RUNNING\n", encoding="utf-8")
        return cls(run_dir, profile)

    def append_jsonl(self, name: str, value: dict[str, Any]) -> None:
        with (self.run_dir / name).open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(value, ensure_ascii=False, sort_keys=True) + "\n")

    def close_writers(self) -> None:
        if self._writers_closed:
            return
        for writer in (self.gateway_writer, self.mqtt_writer, self.resource_writer, self.collector_writer):
            writer.close()
        self._writers_closed = True

    def _segments(self, pattern: str) -> list[pathlib.Path]:
        return sorted(self.run_dir.glob(pattern))

    def write_traceability_outputs(self) -> None:
        self.close_writers()
        gateway = _json_lines(self._segments("gateway_*.jsonl"))
        mqtt = _json_lines(self._segments("mqtt_messages_*.jsonl"))
        resources = _json_lines(self._segments("resource_samples_*.jsonl"))
        from tools.hardware.g6_t04_contract import build_indices

        indices = build_indices(gateway, mqtt, resources)
        _write_jsonl(self.run_dir / "request_index.jsonl", indices["requests"])
        _write_jsonl(self.run_dir / "mqtt_index.jsonl", indices["mqtt"])
        _write_jsonl(self.run_dir / "timeline_index.jsonl", indices["timeline"])
        gaps = self._collection_gaps(gateway, mqtt, resources)
        write_json_atomic(self.run_dir / "collection_gaps.json", gaps)
        segment_paths = [
            *self._segments("gateway_*.jsonl"),
            *self._segments("mqtt_messages_*.jsonl"),
            *self._segments("resource_samples_*.jsonl"),
            *self._segments("collector_*.log"),
        ]
        write_json_atomic(
            self.run_dir / "file_index.json",
            {"schema_version": RUN_SCHEMA, "segments": [asdict(SegmentIndex.from_jsonl(path, self.run_dir)) if path.suffix == ".jsonl" else self._plain_segment(path) for path in segment_paths]},
        )
        self._write_derived(gateway, mqtt, resources)

    def _plain_segment(self, path: pathlib.Path) -> dict[str, Any]:
        return {
            "path": path.relative_to(self.run_dir).as_posix(),
            "record_count": len(path.read_text(encoding="utf-8", errors="replace").splitlines()),
            "bytes": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "first_event_id": None,
            "last_event_id": None,
            "first_monotonic_ms": None,
            "last_monotonic_ms": None,
        }

    def _collection_gaps(
        self,
        gateway: list[dict[str, Any]],
        mqtt: list[dict[str, Any]],
        resources: list[dict[str, Any]],
    ) -> list[dict[str, Any]]:
        gaps: list[dict[str, Any]] = []
        for name, rows, field, maximum_gap in (
            ("resource", resources, "elapsed_seconds", 10.5),
            ("heartbeat", _json_lines([self.run_dir / "heartbeats.jsonl"]), "elapsed_seconds", 10.5),
        ):
            values = sorted(float(row[field]) for row in rows if isinstance(row.get(field), (int, float)))
            for left, right in zip(values, values[1:]):
                if right - left > maximum_gap:
                    gaps.append({"stream": name, "reason": "sampling_gap", "start": left, "end": right})
        if not gateway:
            gaps.append({"stream": "gateway", "reason": "no_records"})
        if not mqtt:
            gaps.append({"stream": "mqtt", "reason": "no_records"})
        return gaps

    def _write_derived(
        self,
        gateway: list[dict[str, Any]],
        mqtt: list[dict[str, Any]],
        resources: list[dict[str, Any]],
    ) -> None:
        derived = self.run_dir / "derived"
        request_groups: dict[tuple[int, int], list[dict[str, Any]]] = {}
        for row in gateway:
            if row.get("event") == "request_completed" and isinstance(row.get("monotonic_ms"), int):
                key = (int(row["monotonic_ms"]) // 60_000, int(row.get("slave_id", -1)))
                request_groups.setdefault(key, []).append(row)
        with (derived / "request_metrics_1m.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.writer(stream)
            writer.writerow(("minute", "slave_id", "requests", "successes", "success_rate", "p50_ms", "p95_ms", "p99_ms", "maximum_success_gap_ms"))
            for (minute, slave), rows in sorted(request_groups.items()):
                successes = [row for row in rows if row.get("result") == "success"]
                durations = sorted(float(row.get("duration_ms", 0)) for row in successes)
                times = sorted(int(row["monotonic_ms"]) for row in successes)
                gaps = [right - left for left, right in zip(times, times[1:])]
                percentile = lambda p: durations[min(len(durations) - 1, max(0, int(len(durations) * p)))] if durations else ""
                writer.writerow((minute, slave, len(rows), len(successes), len(successes) / len(rows), percentile(0.50), percentile(0.95), percentile(0.99), max(gaps, default=0)))
        with (derived / "mqtt_metrics_1m.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.writer(stream)
            writer.writerow(("minute", "messages", "fresh", "stale", "offline", "invalid"))
            groups: dict[int, list[dict[str, Any]]] = {}
            for row in mqtt:
                if isinstance(row.get("received_monotonic_ms"), int):
                    groups.setdefault(int(row["received_monotonic_ms"]) // 60_000, []).append(row)
            for minute, rows in sorted(groups.items()):
                qualities = [row.get("payload", {}).get("quality") for row in rows if isinstance(row.get("payload"), dict)]
                writer.writerow((minute, len(rows), *(qualities.count(value) for value in ("fresh", "stale", "offline", "invalid"))))
        with (derived / "resource_metrics_1m.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.writer(stream)
            writer.writerow(("minute", "samples", "rss_mean_mib", "rss_max_mib", "cpu_mean_percent", "fd_max", "thread_max", "temperature_max_c", "evidence_bytes_max"))
            groups: dict[int, list[dict[str, Any]]] = {}
            for row in resources:
                if isinstance(row.get("elapsed_seconds"), (int, float)):
                    groups.setdefault(int(float(row["elapsed_seconds"]) // 60), []).append(row)
            for minute, rows in sorted(groups.items()):
                values = lambda field: [float(row[field]) for row in rows if isinstance(row.get(field), (int, float))]
                rss, cpu, fd, threads, temp, evidence = (values(field) for field in ("rss_mib", "cpu_percent_single_core", "fd_count", "thread_count", "soc_temperature_c", "evidence_bytes"))
                writer.writerow((minute, len(rows), statistics.fmean(rss) if rss else "", max(rss, default=""), statistics.fmean(cpu) if cpu else "", max(fd, default=""), max(threads, default=""), max(temp, default=""), max(evidence, default="")))
        with (derived / "register_timeseries.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.writer(stream)
            writer.writerow(("received_monotonic_ms", "topic", "slave_id", "sequence", "quality", "value", "unit"))
            for row in mqtt:
                payload = row.get("payload") if isinstance(row.get("payload"), dict) else {}
                writer.writerow((row.get("received_monotonic_ms"), row.get("topic"), payload.get("slave_id"), payload.get("sequence"), payload.get("quality"), payload.get("value"), payload.get("unit")))
        anomalies = [
            row for row in gateway
            if row.get("severity") in {"warning", "error", "critical"}
            or (row.get("event") == "request_completed" and row.get("result") != "success")
        ]
        write_json_atomic(derived / "anomaly_timeline.json", anomalies)
        write_json_atomic(
            derived / "visualization_manifest.json",
            {
                "schema_version": RUN_SCHEMA,
                "generated_at_utc": utc_now(),
                "timezone": "UTC",
                "datasets": [
                    {"path": "request_metrics_1m.csv", "unit": "mixed", "purpose": "request success and latency"},
                    {"path": "mqtt_metrics_1m.csv", "unit": "messages", "purpose": "MQTT quality trends"},
                    {"path": "resource_metrics_1m.csv", "unit": "mixed", "purpose": "host and process trends"},
                    {"path": "register_timeseries.csv", "unit": "payload-defined", "purpose": "measurement visualization"},
                    {"path": "anomaly_timeline.json", "unit": "events", "purpose": "incident review"},
                ],
            },
        )

    def write_checksums(self) -> None:
        lines = []
        for path in sorted(self.run_dir.rglob("*")):
            if not path.is_file() or path.name in {"SHA256SUMS", "PASS", "FAIL", "RUNNING"}:
                continue
            lines.append(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(self.run_dir).as_posix()}")
        (self.run_dir / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="utf-8")

    def verify_checksums(self) -> bool:
        path = self.run_dir / "SHA256SUMS"
        if not path.is_file():
            return False
        for line in path.read_text(encoding="utf-8").splitlines():
            digest, relative = line.split("  ", 1)
            target = self.run_dir / relative
            if not target.is_file() or hashlib.sha256(target.read_bytes()).hexdigest() != digest:
                return False
        return True


def _command(argv: list[str], *, timeout: float = 10.0) -> subprocess.CompletedProcess[str]:
    return subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace", timeout=timeout, check=False, shell=False)


def _systemd_properties(unit: str) -> dict[str, Any]:
    result = _command(["systemctl", "show", unit, "--property=ActiveState,MainPID,NRestarts,InvocationID", "--no-pager"])
    if result.returncode != 0:
        raise RuntimeError(f"systemctl show failed for {unit}: {result.stderr.strip()}")
    values = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
    return {
        "active": values.get("ActiveState") == "active",
        "main_pid": int(values.get("MainPID", "0")),
        "nrestarts": int(values.get("NRestarts", "0")),
        "invocation_id": values.get("InvocationID", ""),
    }


def _snapshot(profile: dict[str, Any]) -> dict[str, Any]:
    serial = pathlib.Path(profile["serial_path"])
    return {
        "timestamp_utc": utc_now(),
        "monotonic_ms": time.monotonic_ns() // 1_000_000,
        "gateway": _systemd_properties(profile["systemd_unit"]),
        "mosquitto": _systemd_properties(profile["mosquitto_unit"]),
        "serial_path": str(serial),
        "serial_exists": serial.exists(),
        "serial_resolved": str(serial.resolve()) if serial.exists() else None,
        "boot_id": pathlib.Path("/proc/sys/kernel/random/boot_id").read_text(encoding="utf-8").strip(),
        "disk": {
            "total_bytes": shutil.disk_usage("/").total,
            "used_bytes": shutil.disk_usage("/").used,
            "free_bytes": shutil.disk_usage("/").free,
        },
    }


def _stream_reader(
    stream: TextIO,
    writer: RotatingTextWriter,
    parser: Any,
    failures: list[dict[str, Any]],
    role: str,
) -> None:
    for line_number, line in enumerate(stream, 1):
        try:
            value = parser(line)
            writer.write(json.dumps(value, ensure_ascii=False, sort_keys=True) + "\n")
        except (ValueError, json.JSONDecodeError) as error:
            failures.append({"stream": role, "line": line_number, "reason": str(error)})


def _text_reader(
    stream: TextIO, writer: RotatingTextWriter, role: str, lock: threading.Lock
) -> None:
    for line in stream:
        with lock:
            writer.write(f"{utc_now()} [{role}] {line}")


def _read_observation(store: EvidenceStore, source_revision: str, run_id: str, started_ms: int, ended_ms: int, shutdown_ms: int, collector_failures: list[dict[str, Any]]) -> dict[str, Any]:
    gateway = _json_lines(store._segments("gateway_*.jsonl"))
    mqtt = _json_lines(store._segments("mqtt_messages_*.jsonl"))
    resources = _json_lines(store._segments("resource_samples_*.jsonl"))
    summaries = [row for row in gateway if row.get("event") == "gateway_summary"]
    reopen_events = sum(1 for row in gateway if row.get("event") in {"serial_opened", "serial_reopened"} and isinstance(row.get("monotonic_ms"), int) and row["monotonic_ms"] >= started_ms)
    final_evidence_bytes = sum(
        path.stat().st_size for path in store.run_dir.rglob("*") if path.is_file()
    )
    return {
        "source_revision": source_revision,
        "run_id": run_id,
        "gateway_run_id": _gateway_run_id(mqtt),
        "started_monotonic_ms": started_ms,
        "ended_monotonic_ms": ended_ms,
        "gateway_events": gateway,
        "mqtt_messages": mqtt,
        "resource_samples": resources,
        "gateway_summary": summaries[-1] if summaries else {},
        "shutdown_duration_ms": shutdown_ms,
        "collector_failures": collector_failures,
        "collection_gaps": json.loads((store.run_dir / "collection_gaps.json").read_text(encoding="utf-8")),
        "serial_reopen_events_after_warmup": max(0, reopen_events - 1),
        "unexpected_exits": sum(
            1
            for failure in collector_failures
            if failure.get("reason") == "required_unit_inactive"
        ),
        "final_evidence_bytes": final_evidence_bytes,
    }


def run(profile: dict[str, Any], source_revision: str, artifact_root: pathlib.Path, run_id: str) -> int:
    validate_source_revision(source_revision)
    if platform.machine().lower() not in {"aarch64", "arm64"}:
        raise RuntimeError("G6-T04 formal hardware soak must run on ARM64")
    for executable in ("journalctl", "mosquitto_sub", "systemctl", "sudo", "vcgencmd"):
        if shutil.which(executable) is None:
            raise RuntimeError(f"required executable is missing: {executable}")
    if shutil.disk_usage(artifact_root.parent if artifact_root.parent.exists() else pathlib.Path("/")).free < int(profile["thresholds"]["minimum_disk_free_bytes"]):
        raise RuntimeError("insufficient free disk before G6-T04")
    store = EvidenceStore.create(artifact_root, source_revision=source_revision, run_id=run_id, profile=profile)
    failures: list[dict[str, Any]] = []
    processes: list[subprocess.Popen[str]] = []
    threads: list[threading.Thread] = []
    started_ms = time.monotonic_ns() // 1_000_000
    shutdown_ms = 0
    try:
        before = _snapshot(profile)
        write_json_atomic(store.run_dir / "systemd_before.json", before)
        store.append_jsonl("system_snapshots.jsonl", {"phase": "before", **before})
        result = _command(["sudo", "-n", "systemctl", "restart", profile["systemd_unit"]], timeout=30)
        store.append_jsonl("commands.jsonl", {"role": "gateway_restart", "argv": ["sudo", "-n", "systemctl", "restart", profile["systemd_unit"]], "returncode": result.returncode})
        if result.returncode != 0:
            raise RuntimeError(f"gateway restart failed: {result.stderr.strip()}")
        time.sleep(2)
        identity = _systemd_properties(profile["systemd_unit"])
        if not identity["active"] or identity["main_pid"] <= 0 or not identity["invocation_id"]:
            raise RuntimeError("gateway did not become active after restart")
        started_ms = time.monotonic_ns() // 1_000_000
        host = Arm64HostSampler(maximum_temperature_c=float(profile["thresholds"]["temperature_max_c"]))
        proc = ProcSampler(identity["main_pid"], store.run_dir)
        write_json_atomic(store.run_dir / "environment.json", {"schema_version": RUN_SCHEMA, **host.environment(), "gateway_identity": identity, "machine": platform.machine(), "platform": platform.platform(), "python": platform.python_version()})
        journal_argv = journal_collector_argv(identity["invocation_id"])
        mqtt_argv = ["mosquitto_sub", "-q", "1", "-t", profile["mqtt_topic_filter"], "-F", "%r\t%q\t%t\t%p"]
        store.append_jsonl("commands.jsonl", {"role": "journal_collector", "argv": journal_argv})
        store.append_jsonl("commands.jsonl", {"role": "mqtt_collector", "argv": mqtt_argv})
        journal = subprocess.Popen(journal_argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
        mqtt = subprocess.Popen(mqtt_argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
        processes.extend((journal, mqtt))
        assert journal.stdout is not None and journal.stderr is not None
        assert mqtt.stdout is not None and mqtt.stderr is not None
        collector_lock = threading.Lock()
        threads.extend(
            (
                threading.Thread(target=_stream_reader, args=(journal.stdout, store.gateway_writer, parse_journal_line, failures, "journal"), daemon=True),
                threading.Thread(target=_stream_reader, args=(mqtt.stdout, store.mqtt_writer, lambda line: parse_mqtt_line(line, received_monotonic_ms=time.monotonic_ns() // 1_000_000), failures, "mqtt"), daemon=True),
                threading.Thread(target=_text_reader, args=(journal.stderr, store.collector_writer, "journal-stderr", collector_lock), daemon=True),
                threading.Thread(target=_text_reader, args=(mqtt.stderr, store.collector_writer, "mqtt-stderr", collector_lock), daemon=True),
            )
        )
        for thread in threads:
            thread.start()
        duration = int(profile["duration_seconds"])
        interval = int(profile["sample_interval_seconds"])
        started = time.monotonic()
        next_sample = started
        warned = False
        risk = False
        while time.monotonic() - started < duration:
            now = time.monotonic()
            if now < next_sample:
                time.sleep(min(0.25, next_sample - now))
                continue
            elapsed = now - started
            gateway_state = _systemd_properties(profile["systemd_unit"])
            broker_state = _systemd_properties(profile["mosquitto_unit"])
            sample = {
                "schema_version": RUN_SCHEMA,
                "event_id": f"{run_id}:resource:{int(elapsed):08d}",
                "timestamp_utc": utc_now(),
                "monotonic_ms": time.monotonic_ns() // 1_000_000,
                **proc.sample(elapsed),
                **host.sample(),
                "gateway_active": gateway_state["active"],
                "mosquitto_active": broker_state["active"],
                "gateway_nrestarts": gateway_state["nrestarts"] - identity["nrestarts"],
                "mosquitto_nrestarts": broker_state["nrestarts"] - before["mosquitto"]["nrestarts"],
                "main_pid": gateway_state["main_pid"],
                "invocation_id": gateway_state["invocation_id"],
            }
            store.resource_writer.write(json.dumps(sample, ensure_ascii=False, sort_keys=True) + "\n")
            store.append_jsonl("heartbeats.jsonl", {"timestamp_utc": utc_now(), "elapsed_seconds": elapsed, "monotonic_ms": sample["monotonic_ms"], "evidence_bytes": sample["evidence_bytes"]})
            evidence_bytes = int(sample["evidence_bytes"])
            if evidence_bytes >= int(profile["thresholds"]["evidence_warning_bytes"]) and not warned:
                store.append_jsonl("events.jsonl", {"event_type": "evidence_warning", "bytes": evidence_bytes, "timestamp_utc": utc_now()})
                warned = True
            if evidence_bytes >= int(profile["thresholds"]["evidence_risk_bytes"]) and not risk:
                store.append_jsonl("events.jsonl", {"event_type": "evidence_risk", "bytes": evidence_bytes, "timestamp_utc": utc_now()})
                risk = True
            if evidence_bytes > int(profile["thresholds"]["evidence_max_bytes"]):
                failures.append({"stream": "evidence", "reason": "evidence_max_bytes_exceeded", "actual": evidence_bytes})
                break
            if not gateway_state["active"] or not broker_state["active"]:
                failures.append({"stream": "systemd", "reason": "required_unit_inactive", "gateway": gateway_state, "mosquitto": broker_state})
                break
            next_sample += interval
        stop_started = time.monotonic()
        result = _command(["sudo", "-n", "systemctl", "stop", profile["systemd_unit"]], timeout=10)
        shutdown_ms = int((time.monotonic() - stop_started) * 1000)
        store.append_jsonl("commands.jsonl", {"role": "gateway_stop", "argv": ["sudo", "-n", "systemctl", "stop", profile["systemd_unit"]], "returncode": result.returncode, "duration_ms": shutdown_ms})
        if result.returncode != 0:
            failures.append({"stream": "systemd", "reason": "gateway_stop_failed", "stderr": result.stderr.strip()})
        time.sleep(2)
    except Exception as error:
        failures.append({"stream": "runner", "reason": f"{type(error).__name__}: {error}"})
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
        for process in processes:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        for thread in threads:
            thread.join(timeout=5)
        ended_ms = time.monotonic_ns() // 1_000_000
        store.close_writers()
        try:
            after = _snapshot(profile)
        except Exception as error:
            after = {"timestamp_utc": utc_now(), "snapshot_error": str(error)}
            failures.append({"stream": "snapshot", "reason": str(error)})
        write_json_atomic(store.run_dir / "systemd_after.json", after)
        store.append_jsonl("system_snapshots.jsonl", {"phase": "after", **after})
        store.write_traceability_outputs()
        observation = _read_observation(store, source_revision, run_id, started_ms, ended_ms, shutdown_ms, failures)
        summary = evaluate_run(profile, observation)
        write_json_atomic(store.run_dir / "request_summary.json", {"slaves": summary["slaves"], "task_coverage": summary["task_coverage"], "throughput": summary["request_throughput_per_second"]})
        write_json_atomic(store.run_dir / "mqtt_summary.json", summary["mqtt"])
        write_json_atomic(store.run_dir / "resource_summary.json", {"oracles": [row for row in summary["oracles"] if row["oracle_id"].startswith(("resources.", "arm64.", "host.", "evidence."))]})
        write_json_atomic(store.run_dir / "summary.json", summary)
        failed = [
            row
            for row in summary["oracles"]
            if row["enforced"] and not row["passed"]
        ]
        write_json_atomic(store.run_dir / "failures.json", failed)
        manifest = json.loads((store.run_dir / "manifest.json").read_text(encoding="utf-8"))
        manifest.update({"ended_at_utc": utc_now(), "monotonic_duration_seconds": (ended_ms - started_ms) / 1000.0, "status": summary["status"], "hardware_long_soak_pass": summary["hardware_long_soak_pass"], "gateway_run_id": summary["gateway_run_id"]})
        write_json_atomic(store.run_dir / "manifest.json", manifest)
        (store.run_dir / "RUNNING").unlink(missing_ok=True)
        store.write_checksums()
        marker = summary["status"]
        (store.run_dir / marker).write_text(marker + "\n", encoding="utf-8")
        return 0 if marker == "PASS" else 4


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run P3-S7-G6-T04 real-hardware soak evidence collection")
    parser.add_argument("--repository-root", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--profile", type=pathlib.Path, required=True)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--artifact-root", type=pathlib.Path, required=True)
    parser.add_argument("--run-id", required=True)
    arguments = parser.parse_args(argv)
    root = arguments.repository_root.resolve()
    profile = load_profile(arguments.profile, root)
    output = safe_output_root(arguments.artifact_root, root)
    if output.name != arguments.source_revision:
        raise ContractError("artifact root must end with the full source revision")
    return run(profile, arguments.source_revision, output, arguments.run_id)


if __name__ == "__main__":
    raise SystemExit(main())

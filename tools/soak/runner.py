from __future__ import annotations

import json
import os
import pathlib
import shutil
import shlex
import signal
import subprocess
import threading
import time
from dataclasses import dataclass, field
from typing import Any, BinaryIO

from .evidence import RotatingTextWriter, SoakEvidence
from .monitor import Arm64HostSampler, ProcSampler, evaluate_arm64_health
from .process import ManagedProcess
from .profile import load_and_validate_profile, validate_execution_request
from .summary import summarize


@dataclass
class DriverState:
    lock: threading.Lock = field(default_factory=threading.Lock)
    latest_heartbeat_received: float = field(default_factory=time.monotonic)
    parse_errors: int = 0


def _read_driver(stream: BinaryIO, writer: RotatingTextWriter, state: DriverState) -> None:
    for raw in iter(stream.readline, b""):
        line = raw.decode("utf-8", errors="replace")
        writer.write(line)
        try:
            event = json.loads(line)
            if event.get("event") == "soak_heartbeat":
                with state.lock:
                    state.latest_heartbeat_received = time.monotonic()
        except (json.JSONDecodeError, AttributeError):
            with state.lock:
                state.parse_errors += 1
    writer.close()


def _read_gateway(
    stream: BinaryIO,
    writer: RotatingTextWriter,
    pty_writer: RotatingTextWriter,
    state: DriverState,
) -> None:
    for raw in iter(stream.readline, b""):
        line = raw.decode("utf-8", errors="replace")
        if line.startswith("event="):
            pty_writer.write(line)
            continue
        writer.write(line)
        try:
            json.loads(line)
        except json.JSONDecodeError:
            with state.lock:
                state.parse_errors += 1
    writer.close()
    pty_writer.close()


def _read_mqtt(stream: BinaryIO, writer: RotatingTextWriter, started: float) -> None:
    for raw in iter(stream.readline, b""):
        line = raw.decode("utf-8", errors="replace").rstrip("\n")
        topic, separator, encoded_payload = line.partition(" ")
        if not separator:
            payload: Any = None
        else:
            try:
                payload = json.loads(encoded_payload)
            except json.JSONDecodeError:
                payload = None
        writer.write(
            json.dumps(
                {
                    "elapsed_seconds": time.monotonic() - started,
                    "topic": topic,
                    "payload": payload,
                    "raw_payload": encoded_payload,
                },
                ensure_ascii=False,
                sort_keys=True,
            )
            + "\n"
        )
    writer.close()


def _read_text(stream: BinaryIO, writer: RotatingTextWriter) -> None:
    for raw in iter(stream.readline, b""):
        writer.write(raw.decode("utf-8", errors="replace"))
    writer.close()


def _start_broker(
    executable: str, port: int, writer: RotatingTextWriter
) -> tuple[ManagedProcess, threading.Thread]:
    process = ManagedProcess([executable, "-p", str(port)], stderr=subprocess.STDOUT)
    assert process.process.stdout is not None
    thread = threading.Thread(
        target=_read_text,
        args=(process.process.stdout, writer),
        name="soak-broker-reader",
    )
    thread.start()
    time.sleep(0.25)
    if process.poll() is not None:
        thread.join(timeout=2.0)
        raise RuntimeError("Mosquitto exited during startup")
    return process, thread


def _start_subscriber(
    executable: str,
    port: int,
    topic: str,
    log_writer: RotatingTextWriter,
    writer: RotatingTextWriter,
    started: float,
) -> tuple[ManagedProcess, threading.Thread, threading.Thread]:
    process = ManagedProcess(
        [executable, "-h", "127.0.0.1", "-p", str(port), "-t", topic, "-v"],
    )
    assert process.process.stdout is not None and process.process.stderr is not None
    mqtt_thread = threading.Thread(
        target=_read_mqtt,
        args=(process.process.stdout, writer, started),
        name="soak-mqtt-reader",
    )
    log_thread = threading.Thread(
        target=_read_text,
        args=(process.process.stderr, log_writer),
        name="soak-subscriber-log-reader",
    )
    mqtt_thread.start()
    log_thread.start()
    return process, mqtt_thread, log_thread


def _run_id(profile: dict[str, Any], revision: str) -> str:
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    safe_revision = revision[:7] if len(revision) >= 7 else "explore"
    return f"{stamp}_soak_{profile['profile_kind']}_{safe_revision}_001"


def run(
    *,
    repository_root: pathlib.Path,
    profile_path: pathlib.Path,
    source_revision: str,
    output_root: pathlib.Path,
    driver_executable: pathlib.Path,
    mosquitto: str,
    mosquitto_sub: str,
    stop_event: threading.Event | None = None,
) -> tuple[int, pathlib.Path]:
    profile = load_and_validate_profile(profile_path, repository_root)
    validate_execution_request(profile, source_revision, output_root, repository_root)
    if not driver_executable.is_file():
        raise ValueError(f"soak driver does not exist: {driver_executable}")
    for executable in (mosquitto, mosquitto_sub):
        if shutil.which(executable) is None and not pathlib.Path(executable).is_file():
            raise ValueError(f"required executable not found: {executable}")
    arm64_sampler: Arm64HostSampler | None = None
    evidence_policy = profile["evidence"]
    if evidence_policy.get("temperature_required") is True:
        arm64_sampler = Arm64HostSampler(
            maximum_temperature_c=float(evidence_policy["temperature_max_c"])
        )
    run_id = _run_id(profile, source_revision)
    output_directory = output_root / run_id
    evidence = SoakEvidence(
        output_directory,
        run_id=run_id,
        profile=profile,
        source_revision=source_revision,
        environment_details=arm64_sampler.environment() if arm64_sampler else None,
    )
    stop_event = stop_event or threading.Event()
    port = int(profile["load"]["mqtt"]["broker_port"])
    segment_bytes = int(profile["thresholds"]["disk"]["log_segment_bytes_max"])
    broker: ManagedProcess | None = None
    subscriber: ManagedProcess | None = None
    subscriber_thread: threading.Thread | None = None
    driver: ManagedProcess | None = None
    driver_threads: list[threading.Thread] = []
    auxiliary_threads: list[threading.Thread] = []
    state = DriverState()
    environment_aborted = False
    internal_failure = False
    started = time.monotonic()
    broker_faults = [fault for fault in profile["faults"] if fault["actor"] == "runner"]
    broker_fault_cycle = -1
    broker_restart_at: float | None = None
    broker_active_fault_id: str | None = None
    broker_active_cycle: int | None = None
    broker_generation = 0
    subscriber_generation = 0
    try:
        disk = shutil.disk_usage(output_directory)
        minimum_start = float(profile["thresholds"]["disk"]["start_free_gib_min"]) * 1024**3
        if disk.free < minimum_start:
            raise RuntimeError("insufficient free disk before soak run")
        broker_writer = RotatingTextWriter(
            output_directory, f"broker_{broker_generation:02d}", "log", segment_bytes
        )
        broker, broker_thread = _start_broker(mosquitto, port, broker_writer)
        auxiliary_threads.append(broker_thread)
        broker_generation += 1
        evidence.command("broker", broker.argv)
        mqtt_writer = RotatingTextWriter(output_directory, "mqtt_messages", "jsonl", segment_bytes)
        subscriber_log_writer = RotatingTextWriter(
            output_directory,
            f"mqtt_subscriber_{subscriber_generation:02d}",
            "log",
            segment_bytes,
        )
        subscriber, subscriber_thread, subscriber_log_thread = _start_subscriber(
            mosquitto_sub,
            port,
            profile["load"]["mqtt"]["subscribe_topic"],
            subscriber_log_writer,
            mqtt_writer,
            started,
        )
        auxiliary_threads.append(subscriber_log_thread)
        subscriber_generation += 1
        evidence.command("mqtt_subscriber", subscriber.argv)
        driver_executor = shlex.split(os.environ.get("GATEWAY_SOAK_DRIVER_EXECUTOR", ""))
        driver_argv = [
            *driver_executor,
            str(driver_executable),
            "--profile",
            str(output_directory / "profile.json"),
            "--run-id",
            run_id,
            "--gateway-events",
            "-",
        ]
        driver = ManagedProcess(driver_argv)
        evidence.command("gateway_soak_driver", driver_argv)
        assert driver.process.stdout is not None and driver.process.stderr is not None
        driver_writer = RotatingTextWriter(output_directory, "driver", "jsonl", segment_bytes)
        gateway_writer = RotatingTextWriter(output_directory, "gateway", "jsonl", segment_bytes)
        pty_writer = RotatingTextWriter(output_directory, "pty", "log", segment_bytes)
        driver_threads = [
            threading.Thread(target=_read_driver, args=(driver.process.stdout, driver_writer, state), name="soak-driver-reader"),
            threading.Thread(target=_read_gateway, args=(driver.process.stderr, gateway_writer, pty_writer, state), name="soak-gateway-reader"),
        ]
        for thread in driver_threads:
            thread.start()
        sampler = ProcSampler(driver.pid, output_directory)
        next_sample = started
        duration = float(profile["duration_seconds"])
        cycle_seconds = float(profile["fault_cycle_seconds"])
        while driver.poll() is None:
            now = time.monotonic()
            elapsed = now - started
            if stop_event.is_set():
                evidence.event("runner_stop_requested")
                break
            with state.lock:
                heartbeat_age = now - state.latest_heartbeat_received
            if heartbeat_age > float(profile["heartbeat_timeout_seconds"]):
                evidence.event("heartbeat_timeout", age_seconds=heartbeat_age)
                internal_failure = True
                break
            if now >= next_sample:
                try:
                    sample = sampler.sample(elapsed)
                    arm64_failures: list[str] = []
                    if arm64_sampler is not None:
                        sample.update(arm64_sampler.sample())
                        arm64_failures = evaluate_arm64_health(
                            sample,
                            maximum_temperature_c=arm64_sampler.maximum_temperature_c,
                        )
                    evidence.append_jsonl("resource_samples.jsonl", sample)
                    disk_threshold = float(profile["thresholds"]["disk"]["runtime_free_gib_min"]) * 1024**3
                    evidence_limit = int(profile["thresholds"]["disk"]["evidence_bytes_max"])
                    if sample["disk_free_bytes"] < disk_threshold or sample["evidence_bytes"] > evidence_limit:
                        evidence.event("resource_stop", sample=sample)
                        internal_failure = True
                        break
                    if arm64_failures:
                        evidence.event(
                            "arm64_host_stop", failures=arm64_failures, sample=sample
                        )
                        internal_failure = True
                        break
                except (FileNotFoundError, ProcessLookupError):
                    break
                next_sample = now + float(profile["sample_interval_seconds"])

            cycle = int(elapsed // cycle_seconds)
            cycle_offset = elapsed % cycle_seconds
            for fault in broker_faults:
                if cycle != broker_fault_cycle and cycle_offset >= float(fault["offset_seconds"]):
                    broker_fault_cycle = cycle
                    evidence.event("broker_fault_started", fault_id=fault["fault_id"], cycle=cycle)
                    broker_active_fault_id = str(fault["fault_id"])
                    broker_active_cycle = cycle
                    if subscriber is not None:
                        subscriber.stop(2.0)
                        subscriber = None
                    if broker is not None:
                        broker.stop(2.0)
                        broker = None
                    broker_restart_at = now + float(fault["duration_seconds"])
            if broker_restart_at is not None and now >= broker_restart_at:
                broker_writer = RotatingTextWriter(
                    output_directory, f"broker_{broker_generation:02d}", "log", segment_bytes
                )
                broker, broker_thread = _start_broker(mosquitto, port, broker_writer)
                auxiliary_threads.append(broker_thread)
                broker_generation += 1
                evidence.command("broker_restarted", broker.argv)
                mqtt_writer = RotatingTextWriter(output_directory, "mqtt_messages_recovery", "jsonl", segment_bytes)
                subscriber_log_writer = RotatingTextWriter(
                    output_directory,
                    f"mqtt_subscriber_{subscriber_generation:02d}",
                    "log",
                    segment_bytes,
                )
                subscriber, subscriber_thread, subscriber_log_thread = _start_subscriber(
                    mosquitto_sub,
                    port,
                    profile["load"]["mqtt"]["subscribe_topic"],
                    subscriber_log_writer,
                    mqtt_writer,
                    started,
                )
                auxiliary_threads.append(subscriber_log_thread)
                subscriber_generation += 1
                evidence.event(
                    "broker_fault_recovered",
                    fault_id=broker_active_fault_id,
                    cycle=broker_active_cycle,
                )
                broker_restart_at = None
                broker_active_fault_id = None
                broker_active_cycle = None
            if elapsed > duration + float(profile["thresholds"]["lifecycle"]["shutdown_timeout_seconds"]):
                evidence.event("driver_duration_overrun", elapsed_seconds=elapsed)
                internal_failure = True
                break
            time.sleep(0.05)

        if driver.poll() is None:
            driver_returncode = driver.stop(float(profile["thresholds"]["lifecycle"]["shutdown_timeout_seconds"]))
        else:
            driver_returncode = int(driver.process.returncode)
        if driver_returncode != 0:
            evidence.event("driver_nonzero_exit", returncode=driver_returncode)
            internal_failure = True
        for thread in driver_threads:
            thread.join(timeout=2.0)
        with state.lock:
            if state.parse_errors:
                evidence.event("json_parse_errors", count=state.parse_errors)
                internal_failure = True
    except KeyboardInterrupt:
        environment_aborted = True
        evidence.event("runner_keyboard_interrupt")
    except Exception as error:
        internal_failure = True
        evidence.event("runner_exception", error=type(error).__name__, detail=str(error))
    finally:
        if driver is not None and driver.poll() is None:
            driver.stop(5.0)
        if subscriber is not None:
            subscriber.stop(2.0)
        if broker is not None:
            broker.stop(2.0)
        if subscriber_thread is not None:
            subscriber_thread.join(timeout=2.0)
        for thread in auxiliary_threads:
            thread.join(timeout=2.0)

    provisional = "ABORTED_ENVIRONMENT" if environment_aborted else ("FAIL" if internal_failure else "PASS")
    evidence.finish(provisional)
    summary = summarize(output_directory)
    if internal_failure and summary["status"] == "PASS":
        summary["status"] = "FAIL"
    return (0 if summary["status"] == "PASS" else 1), output_directory

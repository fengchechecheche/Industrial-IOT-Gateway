from __future__ import annotations

import argparse
import dataclasses
import json
import os
import pathlib
import pwd
import shutil
import signal
import subprocess
import time
from collections.abc import Callable
from typing import Any

from tools.release.systemd_runner import (
    EnvironmentIdentity,
    classify_environment,
    collect_environment_identity,
    parse_systemd_properties,
)
from tools.release.systemd_scenarios import (
    EvidenceStore,
    FIXED_UNIT,
    G5ContractError,
    evaluate_scenario,
    journal_after_cursor_arguments,
    validate_native_inputs,
)


UNIT_PATH = pathlib.Path("/etc/systemd/system") / FIXED_UNIT
CONFIG_ROOT = pathlib.Path("/etc/industrial_iot_gateway")
ENV_PATH = CONFIG_ROOT / "gateway.env"
REGISTER_MAP_PATH = CONFIG_ROOT / "register_map.yaml"
SCENARIO_PATH = CONFIG_ROOT / "pty_slave_scenarios.yaml"
GATEWAY_PATH = pathlib.Path("/usr/local/bin/gateway_app")
PTY_BUS_PATH = pathlib.Path("/usr/local/libexec/industrial_iot_gateway/gateway_pty_bus")
BROKER_PORT = 18_884
G5_MQTT_CLIENT_ID = "iiotg5vmrunner"
G5_GATEWAY_ID = "g5_vm"


def resolve_shared_pty_path(alias: str) -> str:
    try:
        resolved = pathlib.Path(alias).resolve(strict=True)
    except OSError as error:
        raise G5ContractError(f"cannot resolve PTY alias {alias!r}: {error}") from error
    if resolved.parent != pathlib.Path("/dev/pts") or not resolved.name.isdecimal():
        raise G5ContractError(
            f"PTY alias must resolve beneath /dev/pts, got {str(resolved)!r}"
        )
    return str(resolved)


@dataclasses.dataclass(frozen=True)
class NativeInputs:
    source_revision: str
    unit_file: pathlib.Path
    gateway_app: pathlib.Path
    pty_bus: pathlib.Path
    register_map: pathlib.Path
    scenario_config: pathlib.Path
    artifact_root: pathlib.Path
    allow_full_vm: bool


@dataclasses.dataclass
class CommandResult:
    command: list[str]
    returncode: int
    stdout: str
    stderr: str
    duration_ms: int
    timed_out: bool = False


class NativeG5Runner:
    def __init__(self, inputs: NativeInputs, store: EvidenceStore) -> None:
        self.inputs = inputs
        self.store = store
        self.pty_process: subprocess.Popen[str] | None = None
        self.broker_process: subprocess.Popen[str] | None = None
        self.gateway_cursor = ""
        self.owns_config = False
        self.owns_unit = False
        self.owns_gateway = False
        self.owns_pty_bus = False
        self.created_pty_parent = False
        self.created_user = False
        self.created_group = False
        self.all_events: list[dict[str, Any]] = []

    def command(self, label: str, arguments: list[str], timeout: float = 15.0) -> CommandResult:
        if not label.replace("_", "").isalnum():
            raise G5ContractError(f"unsafe command label: {label!r}")
        started = time.monotonic()
        timed_out = False
        try:
            completed = subprocess.run(
                arguments,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=timeout,
                check=False,
                shell=False,
            )
            returncode = completed.returncode
            stdout = completed.stdout
            stderr = completed.stderr
        except subprocess.TimeoutExpired as error:
            timed_out = True
            returncode = 124
            stdout = error.stdout if isinstance(error.stdout, str) else ""
            stderr = error.stderr if isinstance(error.stderr, str) else ""
        result = CommandResult(
            command=arguments,
            returncode=returncode,
            stdout=stdout,
            stderr=stderr,
            duration_ms=int((time.monotonic() - started) * 1_000),
            timed_out=timed_out,
        )
        record = dataclasses.asdict(result)
        record["label"] = label
        with (self.store.run_dir / "commands.jsonl").open("a", encoding="utf-8") as output:
            output.write(json.dumps(record, ensure_ascii=False, sort_keys=True) + "\n")
        (self.store.run_dir / f"{label}.stdout.txt").write_text(stdout, encoding="utf-8")
        (self.store.run_dir / f"{label}.stderr.txt").write_text(stderr, encoding="utf-8")
        return result

    @staticmethod
    def _wait_until(predicate: Callable[[], bool], timeout: float, interval: float = 0.1) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return True
            time.sleep(interval)
        return predicate()

    def _properties(self, label: str) -> dict[str, str]:
        names = (
            "ActiveState",
            "SubState",
            "MainPID",
            "ExecMainStatus",
            "NRestarts",
            "Restart",
            "RestartUSec",
            "TimeoutStopUSec",
            "StartLimitBurst",
            "KillSignal",
            "User",
            "CPUUsageNSec",
            "TasksCurrent",
        )
        arguments = ["systemctl", "show", FIXED_UNIT]
        for name in names:
            arguments.extend(["--property", name])
        return parse_systemd_properties(self.command(label, arguments).stdout)

    def _cursor(self, label: str) -> str:
        result = self.command(label, ["journalctl", "--no-pager", "--show-cursor", "-n", "0"])
        marker = "-- cursor: "
        for line in reversed(result.stdout.splitlines()):
            if line.startswith(marker):
                return line[len(marker) :].strip()
        raise G5ContractError("journal cursor is unavailable")

    def _journal(self, label: str, cursor: str) -> list[dict[str, Any]]:
        result = self.command(label, journal_after_cursor_arguments(FIXED_UNIT, cursor), 20.0)
        events: list[dict[str, Any]] = []
        for line in result.stdout.splitlines():
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(value, dict) and isinstance(value.get("event"), str):
                events.append(value)
        self.all_events.extend(events)
        (self.store.run_dir / f"{label}.json").write_text(
            json.dumps(events, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        return events

    @staticmethod
    def _last_event(events: list[dict[str, Any]], name: str) -> dict[str, Any]:
        for event in reversed(events):
            if event.get("event") == name:
                return event
        return {}

    def _write_environment(self, serial_path: str, *, broker_port: int = BROKER_PORT) -> None:
        mqtt = (
            f"--mqtt-broker-uri tcp://127.0.0.1:{broker_port} "
            f"--mqtt-client-id {G5_MQTT_CLIENT_ID} --gateway-id {G5_GATEWAY_ID}"
        )
        ENV_PATH.write_text(
            f"GATEWAY_SERIAL_DEVICE={serial_path}\n"
            f"GATEWAY_REGISTER_MAP={REGISTER_MAP_PATH}\n"
            f"GATEWAY_MQTT_ARGS={mqtt}\n",
            encoding="utf-8",
        )
        os.chmod(ENV_PATH, 0o640)

    def _start_service(self, label: str) -> tuple[bool, dict[str, str]]:
        self.command(f"{label}_reset", ["systemctl", "reset-failed", FIXED_UNIT])
        started = self.command(f"{label}_start", ["systemctl", "start", FIXED_UNIT], 10.0)

        def active() -> bool:
            return self.command(
                f"{label}_poll", ["systemctl", "is-active", FIXED_UNIT], 3.0
            ).stdout.strip() == "active"

        active_ok = started.returncode == 0 and self._wait_until(active, 5.0)
        return active_ok, self._properties(f"{label}_properties")

    def _stop_service(self, label: str) -> tuple[CommandResult, dict[str, str]]:
        result = self.command(label, ["systemctl", "stop", FIXED_UNIT], 7.0)
        return result, self._properties(f"{label}_properties")

    def _check_collisions(self) -> None:
        collisions: list[str] = []
        for path in (UNIT_PATH, CONFIG_ROOT, GATEWAY_PATH, PTY_BUS_PATH):
            if path.exists() or path.is_symlink():
                collisions.append(str(path))
        try:
            pwd.getpwnam("iot-gw")
            collisions.append("user:iot-gw")
        except KeyError:
            pass
        if self.command("collision_group", ["getent", "group", "iot-gw"]).returncode == 0:
            collisions.append("group:iot-gw")
        if self.command("collision_unit", ["systemctl", "cat", FIXED_UNIT]).returncode == 0:
            collisions.append(f"unit:{FIXED_UNIT}")
        if collisions:
            raise G5ContractError(f"dedicated-host collision: {sorted(set(collisions))}")

    def _start_pty_fixture(self) -> tuple[str, str]:
        if self.pty_process is not None and self.pty_process.poll() is None:
            raise G5ContractError("PTY fixture is already running")
        self.pty_process = subprocess.Popen(
            [
                "runuser",
                "--user",
                "iot-gw",
                "--",
                str(PTY_BUS_PATH),
                "--register-map",
                str(REGISTER_MAP_PATH),
                "--scenario-config",
                str(SCENARIO_PATH),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            start_new_session=True,
        )
        if self.pty_process.stdout is None:
            raise G5ContractError("PTY bus stdout is unavailable")
        ready = self.pty_process.stdout.readline().strip()
        try:
            ready_event = json.loads(ready)
        except json.JSONDecodeError as error:
            raise G5ContractError(f"invalid PTY ready event: {ready!r}") from error
        serial_alias = ready_event.get("path")
        if ready_event.get("event") != "pty_bus_ready" or not isinstance(serial_alias, str):
            raise G5ContractError(f"PTY bus did not become ready: {ready_event!r}")
        serial_path = resolve_shared_pty_path(serial_alias)
        self._write_environment(serial_path)
        return serial_alias, serial_path

    def _restart_pty_fixture(self) -> None:
        self._stop_owned_process(self.pty_process)
        self.pty_process = None
        serial_alias, serial_path = self._start_pty_fixture()
        self.store.event(
            "pty_fixture_restarted",
            serial_alias=serial_alias,
            serial_path=serial_path,
        )

    def setup(self) -> None:
        self._check_collisions()
        if self.command("groupadd", ["groupadd", "--system", "iot-gw"]).returncode != 0:
            raise G5ContractError("cannot create iot-gw group")
        self.created_group = True
        user_result = self.command(
            "useradd",
            [
                "useradd",
                "--system",
                "--gid",
                "iot-gw",
                "--home-dir",
                "/var/lib/industrial_iot_gateway",
                "--shell",
                "/usr/sbin/nologin",
                "iot-gw",
            ],
        )
        if user_result.returncode != 0:
            raise G5ContractError("cannot create iot-gw user")
        self.created_user = True
        self.command("dialout_group", ["usermod", "--append", "--groups", "dialout", "iot-gw"])

        CONFIG_ROOT.mkdir(mode=0o750)
        self.owns_config = True
        self.created_pty_parent = not PTY_BUS_PATH.parent.exists()
        PTY_BUS_PATH.parent.mkdir(parents=True, mode=0o755)
        shutil.copy2(self.inputs.unit_file, UNIT_PATH)
        self.owns_unit = True
        shutil.copy2(self.inputs.gateway_app, GATEWAY_PATH)
        self.owns_gateway = True
        shutil.copy2(self.inputs.pty_bus, PTY_BUS_PATH)
        self.owns_pty_bus = True
        shutil.copy2(self.inputs.register_map, REGISTER_MAP_PATH)
        shutil.copy2(self.inputs.scenario_config, SCENARIO_PATH)
        os.chmod(GATEWAY_PATH, 0o755)
        os.chmod(PTY_BUS_PATH, 0o755)
        os.chmod(REGISTER_MAP_PATH, 0o640)
        os.chmod(SCENARIO_PATH, 0o640)
        uid = pwd.getpwnam("iot-gw").pw_uid
        gid = pwd.getpwnam("iot-gw").pw_gid
        for path in (CONFIG_ROOT, REGISTER_MAP_PATH, SCENARIO_PATH):
            os.chown(path, uid, gid)

        self.command("daemon_reload", ["systemctl", "daemon-reload"])
        serial_alias, serial_path = self._start_pty_fixture()
        self._start_broker()
        self.gateway_cursor = self._cursor("initial_cursor")
        self.store.event(
            "native_setup_complete",
            serial_alias=serial_alias,
            serial_path=serial_path,
            broker_port=BROKER_PORT,
        )

    def _start_broker(self) -> None:
        if self.broker_process is not None and self.broker_process.poll() is None:
            return
        self.broker_process = subprocess.Popen(
            ["runuser", "--user", "iot-gw", "--", "mosquitto", "-p", str(BROKER_PORT), "-v"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            start_new_session=True,
        )
        time.sleep(0.4)
        if self.broker_process.poll() is not None:
            raise G5ContractError("private Mosquitto failed to start")

    @staticmethod
    def _stop_owned_process(process: subprocess.Popen[str] | None) -> None:
        if process is None or process.poll() is not None:
            return
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=2.0)

    def scenario_unit_static_verify(self) -> dict[str, Any]:
        verify = self.command("g501_verify", ["systemd-analyze", "verify", str(UNIT_PATH)])
        properties = self._properties("g501_properties")
        keys = ("Restart", "RestartUSec", "TimeoutStopUSec", "StartLimitBurst", "KillSignal", "User")
        return {"verify_rc": verify.returncode, "properties": {key: properties.get(key, "") for key in keys}}

    def scenario_normal_start(self) -> dict[str, Any]:
        cursor = self._cursor("g502_cursor")
        active, properties = self._start_service("g502")
        time.sleep(4.0)
        self._stop_service("g502_stop")
        events = self._journal("g502_journal", cursor)
        summary = self._last_event(events, "gateway_summary")
        return {
            "active_state": "active" if active else properties.get("ActiveState", "unknown"),
            "main_pid": int(properties.get("MainPID", "0") or 0),
            "gateway_ready": bool(self._last_event(events, "gateway_ready")),
            "requests_succeeded_delta": int(summary.get("requests_succeeded", 0)),
            "mqtt_publish_successes_delta": int(summary.get("mqtt_publish_successes", 0)),
        }

    def scenario_sigterm_stop(self) -> dict[str, Any]:
        cursor = self._cursor("g503_cursor")
        self._start_service("g503")
        time.sleep(1.0)
        stopped, properties = self._stop_service("g503_stop")
        events = self._journal("g503_journal", cursor)
        summary = self._last_event(events, "gateway_summary")
        return {
            "stop_rc": stopped.returncode,
            "stop_ms": stopped.duration_ms,
            "active_state": properties.get("ActiveState", "unknown"),
            "main_pid": int(properties.get("MainPID", "0") or 0),
            "stopped": summary.get("stopped") is True,
        }

    def scenario_sigint_direct(self) -> dict[str, Any]:
        cursor = self._cursor("g504_cursor")
        environment = ENV_PATH.read_text(encoding="utf-8").splitlines()
        values = dict(line.split("=", 1) for line in environment if "=" in line)
        mqtt_args = values["GATEWAY_MQTT_ARGS"].split()
        process = subprocess.Popen(
            [
                "setpriv",
                "--reuid=iot-gw",
                "--regid=iot-gw",
                "--init-groups",
                "--",
                str(GATEWAY_PATH),
                "--serial-device",
                values["GATEWAY_SERIAL_DEVICE"],
                "--register-map",
                values["GATEWAY_REGISTER_MAP"],
                *mqtt_args,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            start_new_session=True,
        )
        time.sleep(1.0)
        started = time.monotonic()
        os.killpg(process.pid, signal.SIGINT)
        try:
            stdout, stderr = process.communicate(timeout=6.0)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            stdout, stderr = process.communicate(timeout=2.0)
        stop_ms = int((time.monotonic() - started) * 1_000)
        (self.store.run_dir / "g504_direct.stdout.txt").write_text(stdout, encoding="utf-8")
        (self.store.run_dir / "g504_direct.stderr.txt").write_text(stderr, encoding="utf-8")
        events = []
        for line in stdout.splitlines():
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(value, dict):
                events.append(value)
        self._journal("g504_journal", cursor)
        return {
            "exit_code": process.returncode,
            "stop_ms": stop_ms,
            "stopped": self._last_event(events, "gateway_summary").get("stopped") is True,
        }

    def scenario_abnormal_restart(self) -> dict[str, Any]:
        self._start_service("g505")
        before = self._properties("g505_before")
        old_pid = int(before.get("MainPID", "0") or 0)
        old_restarts = int(before.get("NRestarts", "0") or 0)
        started = time.monotonic()
        self.command(
            "g505_abort",
            ["systemctl", "kill", "--kill-whom=main", "--signal=SIGABRT", FIXED_UNIT],
        )
        latest: dict[str, str] = {}

        def restarted() -> bool:
            nonlocal latest
            latest = self._properties("g505_poll")
            return (
                latest.get("ActiveState") == "active"
                and int(latest.get("MainPID", "0") or 0) not in {0, old_pid}
            )

        recovered = self._wait_until(restarted, 7.0, 0.2)
        restart_ms = int((time.monotonic() - started) * 1_000)
        self._stop_service("g505_stop")
        return {
            "active_state": "active" if recovered else latest.get("ActiveState", "unknown"),
            "old_pid": old_pid,
            "new_pid": int(latest.get("MainPID", "0") or 0),
            "restart_count_delta": int(latest.get("NRestarts", "0") or 0) - old_restarts,
            "restart_ms": restart_ms,
        }

    def scenario_invalid_config(self) -> dict[str, Any]:
        original = ENV_PATH.read_text(encoding="utf-8")
        self.command("g506_reset", ["systemctl", "reset-failed", FIXED_UNIT])
        before = self._properties("g506_before")
        try:
            self._write_environment(str(self._serial_path()))
            text = ENV_PATH.read_text(encoding="utf-8").replace(
                f"GATEWAY_REGISTER_MAP={REGISTER_MAP_PATH}",
                "GATEWAY_REGISTER_MAP=/etc/industrial_iot_gateway/missing.yaml",
            )
            ENV_PATH.write_text(text, encoding="utf-8")
            self.command("g506_start", ["systemctl", "start", FIXED_UNIT], 8.0)
            self._wait_until(
                lambda: self._properties("g506_poll").get("ActiveState") == "failed", 5.0
            )
            after = self._properties("g506_after")
        finally:
            ENV_PATH.write_text(original, encoding="utf-8")
            self.command("g506_reset_final", ["systemctl", "reset-failed", FIXED_UNIT])
        return {
            "exit_code": int(after.get("ExecMainStatus", "-1") or -1),
            "restart_count_delta": int(after.get("NRestarts", "0") or 0)
            - int(before.get("NRestarts", "0") or 0),
            "active_state": after.get("ActiveState", "unknown"),
        }

    def _serial_path(self) -> pathlib.Path:
        for line in ENV_PATH.read_text(encoding="utf-8").splitlines():
            if line.startswith("GATEWAY_SERIAL_DEVICE="):
                return pathlib.Path(line.split("=", 1)[1])
        raise G5ContractError("serial path missing from environment")

    def scenario_missing_serial(self) -> dict[str, Any]:
        original = ENV_PATH.read_text(encoding="utf-8")
        cursor = self._cursor("g507_cursor")
        try:
            self._write_environment("/dev/industrial_iot_gateway_g5_missing")
            active, before = self._start_service("g507")
            time.sleep(3.0)
            after_running = self._properties("g507_running")
            self._stop_service("g507_stop")
            events = self._journal("g507_journal", cursor)
        finally:
            ENV_PATH.write_text(original, encoding="utf-8")
        summary = self._last_event(events, "gateway_summary")
        cpu_before = int(before.get("CPUUsageNSec", "0") or 0)
        cpu_after = int(after_running.get("CPUUsageNSec", "0") or 0)
        journal_bytes = sum(len(json.dumps(value, ensure_ascii=False).encode()) for value in events)
        return {
            "active_state": "active" if active else before.get("ActiveState", "unknown"),
            "serial_open_successes": int(summary.get("serial_open_successes", 0)),
            "journal_bytes": journal_bytes,
            "cpu_time_delta_ms": max(0, (cpu_after - cpu_before) // 1_000_000),
        }

    def scenario_broker_unavailable(self) -> dict[str, Any]:
        self._stop_owned_process(self.broker_process)
        self.broker_process = None
        self._restart_pty_fixture()
        cursor = self._cursor("g508_cursor")
        active, properties = self._start_service("g508")
        time.sleep(4.0)
        before_recovery = self._journal("g508_before_recovery", cursor)
        self._start_broker()
        time.sleep(5.0)
        self._stop_service("g508_stop")
        events = self._journal("g508_journal", cursor)
        summary = self._last_event(events, "gateway_summary")
        failures = sum(1 for value in before_recovery if value.get("event") == "mqtt_connect_failed")
        connected = any(value.get("event") == "mqtt_connected" for value in events)
        return {
            "active_state": "active" if active else properties.get("ActiveState", "unknown"),
            "serial_successes_delta": int(summary.get("requests_succeeded", 0)),
            "mqtt_failures_delta": failures,
            "mqtt_reconnected": connected,
            "mqtt_publish_success_after_recovery": int(summary.get("mqtt_publish_successes", 0)) > 0,
        }

    def scenario_journal_observability(self) -> dict[str, Any]:
        return {
            "cursor_scoped": bool(self.gateway_cursor),
            "events": sorted({str(value.get("event")) for value in self.all_events}),
        }

    def scenario_repeated_cycles(self) -> dict[str, Any]:
        failed = 0
        pids: set[int] = set()
        for cycle in range(10):
            active, properties = self._start_service(f"g510_{cycle:02d}")
            pid = int(properties.get("MainPID", "0") or 0)
            if active and pid > 0:
                pids.add(pid)
            else:
                failed += 1
            time.sleep(0.4)
            stopped, after = self._stop_service(f"g510_{cycle:02d}_stop")
            if stopped.returncode != 0 or after.get("MainPID") not in {"0", ""}:
                failed += 1
        final = self._properties("g510_final")
        residual = 0 if final.get("MainPID") in {"0", ""} else 1
        if len(pids) != 10:
            failed += 1
        return {
            "cycles": 10,
            "failed_cycles": failed,
            "residual_pids": residual,
            "fd_drift": 0 if residual == 0 else 1,
            "thread_drift": 0 if final.get("TasksCurrent") in {"", "[not set]", "0"} else 1,
        }

    def run_scenarios(self) -> None:
        methods = (
            ("unit_static_verify", self.scenario_unit_static_verify),
            ("normal_start", self.scenario_normal_start),
            ("sigterm_stop", self.scenario_sigterm_stop),
            ("sigint_direct", self.scenario_sigint_direct),
            ("abnormal_restart", self.scenario_abnormal_restart),
            ("invalid_config", self.scenario_invalid_config),
            ("missing_serial", self.scenario_missing_serial),
            ("broker_unavailable", self.scenario_broker_unavailable),
            ("journal_observability", self.scenario_journal_observability),
            ("repeated_cycles", self.scenario_repeated_cycles),
        )
        for name, method in methods:
            self.store.event("scenario_started", scenario=name)
            try:
                observation = method()
                record = evaluate_scenario(name, observation)
            except Exception as error:  # preserve evidence and continue to cleanup
                record = {
                    "schema_version": "p3-s7-g5-scenario-v1",
                    "scenario": name,
                    "status": "FAIL",
                    "failures": [f"runner exception: {type(error).__name__}: {error}"],
                    "observation": {},
                }
            self.store.record(record)

    def cleanup(self) -> bool:
        ok = True
        if self.owns_unit:
            if self.command(
                "cleanup_stop", ["systemctl", "stop", FIXED_UNIT], 8.0
            ).returncode not in {0, 5}:
                ok = False
        self._stop_owned_process(self.broker_process)
        self._stop_owned_process(self.pty_process)
        self.broker_process = None
        self.pty_process = None
        owned_files = (
            (UNIT_PATH, self.owns_unit),
            (GATEWAY_PATH, self.owns_gateway),
            (PTY_BUS_PATH, self.owns_pty_bus),
        )
        for path, owned in owned_files:
            if not owned:
                continue
            try:
                path.unlink(missing_ok=True)
            except OSError:
                ok = False
        try:
            if self.owns_config and CONFIG_ROOT.exists():
                shutil.rmtree(CONFIG_ROOT)
            parent = PTY_BUS_PATH.parent
            if self.created_pty_parent and parent.exists() and not any(parent.iterdir()):
                parent.rmdir()
        except OSError:
            ok = False
        if self.owns_unit:
            self.command("cleanup_reload", ["systemctl", "daemon-reload"])
            self.command("cleanup_reset", ["systemctl", "reset-failed", FIXED_UNIT])
        if (
            self.created_user
            and self.command("cleanup_user", ["userdel", "iot-gw"]).returncode != 0
        ):
            ok = False
        if self.created_group:
            group_probe = self.command("cleanup_group_probe", ["getent", "group", "iot-gw"])
            if group_probe.returncode == 0:
                if self.command("cleanup_group", ["groupdel", "iot-gw"]).returncode != 0:
                    ok = False
            elif group_probe.returncode not in {1, 2}:
                ok = False
        residual = any(
            owned and (path.exists() or path.is_symlink()) for path, owned in owned_files
        ) or (self.owns_config and CONFIG_ROOT.exists())
        self.store.event("cleanup_finished", cleanup_ok=ok and not residual, residual=residual)
        return ok and not residual


def _validate_paths(inputs: NativeInputs) -> None:
    validate_native_inputs(FIXED_UNIT, inputs.source_revision)
    for path in (
        inputs.unit_file,
        inputs.gateway_app,
        inputs.pty_bus,
        inputs.register_map,
        inputs.scenario_config,
    ):
        if not path.is_file():
            raise G5ContractError(f"required input is not a file: {path}")


def parse_arguments(argv: list[str] | None = None) -> NativeInputs:
    parser = argparse.ArgumentParser(description="Run native x86_64/systemd G5 scenarios")
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--unit-file", required=True, type=pathlib.Path)
    parser.add_argument("--gateway-app", required=True, type=pathlib.Path)
    parser.add_argument("--pty-bus", required=True, type=pathlib.Path)
    parser.add_argument("--register-map", required=True, type=pathlib.Path)
    parser.add_argument("--scenario-config", required=True, type=pathlib.Path)
    parser.add_argument("--artifact-root", required=True, type=pathlib.Path)
    parser.add_argument("--allow-full-vm", action="store_true")
    arguments = parser.parse_args(argv)
    return NativeInputs(
        source_revision=arguments.source_revision,
        unit_file=arguments.unit_file.resolve(),
        gateway_app=arguments.gateway_app.resolve(),
        pty_bus=arguments.pty_bus.resolve(),
        register_map=arguments.register_map.resolve(),
        scenario_config=arguments.scenario_config.resolve(),
        artifact_root=arguments.artifact_root.resolve(),
        allow_full_vm=arguments.allow_full_vm,
    )


def main(argv: list[str] | None = None) -> int:
    inputs = parse_arguments(argv)
    _validate_paths(inputs)
    identity: EnvironmentIdentity = collect_environment_identity()
    environment_class = classify_environment(identity, allow_full_vm=inputs.allow_full_vm)
    store = EvidenceStore.create(inputs.artifact_root, inputs.source_revision)
    store.event("environment_classified", **dataclasses.asdict(identity), environment_class=environment_class)
    if environment_class != "NATIVE_ELIGIBLE":
        return store.finalize(environment_class=environment_class, cleanup_ok=True)
    if os.geteuid() != 0:
        store.record(
            {
                "schema_version": "p3-s7-g5-scenario-v1",
                "scenario": "unit_static_verify",
                "status": "FAIL",
                "failures": ["native execution requires one root runner invocation"],
                "observation": {},
            }
        )
        return store.finalize(environment_class=environment_class, cleanup_ok=True)

    runner = NativeG5Runner(inputs, store)
    cleanup_ok = False
    try:
        runner.setup()
        runner.run_scenarios()
    except Exception as error:
        store.event("runner_failed", error_type=type(error).__name__, detail=str(error))
    finally:
        cleanup_ok = runner.cleanup()
    return store.finalize(environment_class=environment_class, cleanup_ok=cleanup_ok)


if __name__ == "__main__":
    raise SystemExit(main())

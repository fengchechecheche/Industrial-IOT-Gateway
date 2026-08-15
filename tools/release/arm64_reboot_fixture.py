from __future__ import annotations

import argparse
import json
import os
import pathlib
import selectors
import signal
import socket
import subprocess
import threading
import time
from collections.abc import Sequence

STOP_REQUESTED = threading.Event()


class RebootFixtureError(ValueError):
    """Raised when the standalone reboot fixture cannot become ready."""


def _parse_fixture_ready(line: str) -> str:
    try:
        event = json.loads(line)
    except json.JSONDecodeError as error:
        raise RebootFixtureError("invalid pty_bus_ready JSON") from error
    value = event.get("path") if isinstance(event, dict) else None
    event_name = event.get("event") if isinstance(event, dict) else None
    if event_name != "pty_bus_ready" or not isinstance(value, str):
        raise RebootFixtureError("missing pty_bus_ready event")
    try:
        resolved = pathlib.Path(value).resolve(strict=True)
    except OSError as error:
        raise RebootFixtureError("PTY path must resolve beneath /dev/pts") from error
    if resolved.parent != pathlib.Path("/dev/pts") or not resolved.name.isdecimal():
        raise RebootFixtureError(f"PTY path must resolve beneath /dev/pts: {resolved}")
    return str(resolved)


def _request_stop(_signum: int, _frame: object) -> None:
    STOP_REQUESTED.set()


def _wait_for_port(port: int, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.settimeout(0.2)
            if probe.connect_ex(("127.0.0.1", port)) == 0:
                return True
        time.sleep(0.1)
    return False


def _readline_with_timeout(process: subprocess.Popen[str], timeout: float) -> str:
    if process.stdout is None:
        raise RebootFixtureError("PTY fixture stdout is unavailable")
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    try:
        if not selector.select(timeout):
            raise RebootFixtureError("PTY fixture ready event timed out")
        return process.stdout.readline().strip()
    finally:
        selector.close()


def _stop_process(process: subprocess.Popen[str] | None) -> None:
    if process is None or process.poll() is not None:
        return
    os.killpg(process.pid, signal.SIGTERM)
    try:
        process.wait(timeout=3.0)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=2.0)


def _relay_subscriber(process: subprocess.Popen[str]) -> None:
    if process.stdout is None:
        return
    for line in process.stdout:
        value = line.rstrip("\r\n")
        if value:
            print(
                json.dumps(
                    {"event": "reboot_fixture_mqtt_message", "message": value},
                    ensure_ascii=False,
                    sort_keys=True,
                ),
                flush=True,
            )


def _update_alias(alias: pathlib.Path, actual_path: str) -> None:
    if alias.parent != pathlib.Path("/run/industrial_iot_gateway-reboot"):
        raise RebootFixtureError("stable alias must remain under the test RuntimeDirectory")
    alias.parent.mkdir(parents=True, exist_ok=True)
    pending = alias.with_name(alias.name + ".next")
    pending.unlink(missing_ok=True)
    pending.symlink_to(actual_path)
    os.replace(pending, alias)


def run(arguments: argparse.Namespace) -> int:
    broker: subprocess.Popen[str] | None = None
    subscriber: subprocess.Popen[str] | None = None
    pty: subprocess.Popen[str] | None = None
    alias = pathlib.Path(arguments.serial_alias)
    STOP_REQUESTED.clear()
    signal.signal(signal.SIGTERM, _request_stop)
    signal.signal(signal.SIGINT, _request_stop)
    try:
        broker = subprocess.Popen(
            [arguments.mosquitto, "-p", str(arguments.broker_port), "-v"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            text=True,
            start_new_session=True,
        )
        if not _wait_for_port(arguments.broker_port, 10.0):
            raise RebootFixtureError("private Mosquitto did not become ready")
        subscriber = subprocess.Popen(
            [
                arguments.mosquitto_sub,
                "-h",
                "127.0.0.1",
                "-p",
                str(arguments.broker_port),
                "-t",
                "industrial_iot_gateway/#",
                "-v",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
            start_new_session=True,
        )
        pty = subprocess.Popen(
            [
                arguments.pty_bus,
                "--register-map",
                arguments.register_map,
                "--scenario-config",
                arguments.scenario_config,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
            start_new_session=True,
        )
        actual_path = _parse_fixture_ready(_readline_with_timeout(pty, 15.0))
        _update_alias(alias, actual_path)
        relay = threading.Thread(target=_relay_subscriber, args=(subscriber,), daemon=True)
        relay.start()
        ready = {
            "event": "reboot_fixture_ready",
            "serial_alias": str(alias),
            "serial_path": actual_path,
            "broker_port": arguments.broker_port,
        }
        print(json.dumps(ready, sort_keys=True), flush=True)
        notified = subprocess.run(
            ["systemd-notify", "--ready", "--status=PTY and MQTT fixture ready"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
            timeout=5.0,
        )
        if notified.returncode != 0:
            raise RebootFixtureError("systemd readiness notification failed")
        while not STOP_REQUESTED.wait(0.25):
            if any(child.poll() is not None for child in (broker, subscriber, pty)):
                raise RebootFixtureError("fixture child exited unexpectedly")
        return 0
    except (OSError, RebootFixtureError, subprocess.SubprocessError) as error:
        print(
            json.dumps(
                {
                    "event": "reboot_fixture_failed",
                    "error_type": type(error).__name__,
                    "detail": str(error),
                },
                sort_keys=True,
            ),
            flush=True,
        )
        return 4
    finally:
        alias.unlink(missing_ok=True)
        _stop_process(pty)
        _stop_process(subscriber)
        _stop_process(broker)


def parse_arguments(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the reboot-test PTY and MQTT fixture")
    parser.add_argument("--pty-bus", required=True)
    parser.add_argument("--register-map", required=True)
    parser.add_argument("--scenario-config", required=True)
    parser.add_argument("--serial-alias", required=True)
    parser.add_argument("--broker-port", type=int, default=18_884)
    parser.add_argument("--mosquitto", default="/usr/sbin/mosquitto")
    parser.add_argument("--mosquitto-sub", default="/usr/bin/mosquitto_sub")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    return run(parse_arguments(argv))


if __name__ == "__main__":
    raise SystemExit(main())

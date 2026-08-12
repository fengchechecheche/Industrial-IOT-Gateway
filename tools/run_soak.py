#!/usr/bin/env python3
from __future__ import annotations

import argparse
import pathlib
import signal
import threading

from soak.profile import load_and_validate_profile, validate_execution_request
from soak.runner import run


def main() -> int:
    parser = argparse.ArgumentParser(description="Run Industrial-IOT-Gateway software soak")
    parser.add_argument("--profile", type=pathlib.Path, required=True)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--output-root", type=pathlib.Path, required=True)
    parser.add_argument("--driver", type=pathlib.Path, default=pathlib.Path("build/gateway_soak_driver"))
    parser.add_argument("--mosquitto", default="mosquitto")
    parser.add_argument("--mosquitto-sub", default="mosquitto_sub")
    parser.add_argument("--validate-only", action="store_true")
    arguments = parser.parse_args()
    repository_root = pathlib.Path(__file__).resolve().parents[1]
    profile = load_and_validate_profile(arguments.profile, repository_root)
    validate_execution_request(profile, arguments.source_revision, arguments.output_root, repository_root)
    if arguments.validate_only:
        print(f"PROFILE_VALID={profile['profile_id']}")
        return 0
    stop_event = threading.Event()
    signal.signal(signal.SIGTERM, lambda _signum, _frame: stop_event.set())
    signal.signal(signal.SIGINT, lambda _signum, _frame: stop_event.set())
    code, output = run(
        repository_root=repository_root,
        profile_path=arguments.profile,
        source_revision=arguments.source_revision,
        output_root=arguments.output_root,
        driver_executable=arguments.driver,
        mosquitto=arguments.mosquitto,
        mosquitto_sub=arguments.mosquitto_sub,
        stop_event=stop_event,
    )
    print(f"SOAK_EVIDENCE={output}")
    return code


if __name__ == "__main__":
    raise SystemExit(main())

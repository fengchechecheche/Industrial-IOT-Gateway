#!/usr/bin/env python3
from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import re
import sys

from fault_matrix.models import ScenarioDefinition, ScenarioStatus
from fault_matrix.runner import run_scenarios


def _parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the P3-S4-T03 software fault matrix")
    parser.add_argument("--profile", default="software")
    parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--output-root", type=pathlib.Path, default=pathlib.Path("artifacts/baseline"))
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--exploratory", action="store_true")
    return parser.parse_args()


def _profile_path(repository: pathlib.Path, value: str) -> pathlib.Path:
    candidate = pathlib.Path(value)
    if candidate.is_file():
        return candidate
    return repository / "tests" / "data" / "fault_profiles" / f"{value}.json"


def _next_run_id(output_root: pathlib.Path, revision: str) -> str:
    timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    short = re.sub(r"[^0-9A-Za-z]", "", revision)[:7] or "unknown"
    prefix = f"{timestamp}_g3_{short}_"
    sequence = 1
    while (output_root / f"{prefix}{sequence:03d}").exists():
        sequence += 1
    return f"{prefix}{sequence:03d}"


def main() -> int:
    args = _parse_arguments()
    repository = pathlib.Path(__file__).resolve().parents[1]
    if not args.build_dir.is_dir():
        print(f"build directory does not exist: {args.build_dir}", file=sys.stderr)
        return 3
    if not args.exploratory and not re.fullmatch(r"[0-9a-fA-F]{7,40}", args.source_revision):
        print("formal runs require a 7-40 character hexadecimal source revision", file=sys.stderr)
        return 2
    profile_path = _profile_path(repository, args.profile)
    try:
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
        definitions = []
        for scenario in profile["scenarios"]:
            command = [
                "ctest",
                "--test-dir",
                str(args.build_dir),
                "--output-on-failure",
                "--verbose",
                "--no-tests=error",
                "-R",
                scenario["ctest_regex"],
            ]
            definitions.append(
                ScenarioDefinition(
                    scenario_id=scenario["scenario_id"],
                    title=scenario["title"],
                    command=command,
                    timeout_seconds=float(scenario.get("timeout_seconds", 30.0)),
                    config={
                        "ctest_regex": scenario["ctest_regex"],
                        "expected": scenario["expected"],
                        "requires_recovery_metric": bool(
                            scenario.get("requires_recovery_metric", False)
                        ),
                    },
                )
            )
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        print(f"invalid fault profile: {error}", file=sys.stderr)
        return 2

    args.output_root.mkdir(parents=True, exist_ok=True)
    run_id = _next_run_id(args.output_root, args.source_revision)
    result = run_scenarios(
        definitions,
        args.output_root / run_id,
        run_id=run_id,
        source_revision=args.source_revision,
        exploratory=args.exploratory,
    )
    print(json.dumps({"run_id": run_id, "status": result.status.value, "output": result.output_directory}))
    if result.status == ScenarioStatus.PASS:
        return 0
    if result.status == ScenarioStatus.ERROR:
        return 5
    return 4


if __name__ == "__main__":
    raise SystemExit(main())

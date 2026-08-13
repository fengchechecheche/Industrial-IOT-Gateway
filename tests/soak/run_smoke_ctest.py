from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.soak.runner import run  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    output_root = ROOT / "artifacts/soak/ctest"
    shutil.rmtree(output_root, ignore_errors=True)
    passed = False
    try:
        code, directory = run(
            repository_root=ROOT,
            profile_path=ROOT / "tests/data/soak_profiles/software_smoke.json",
            source_revision="exploratory",
            output_root=output_root,
            driver_executable=arguments.driver,
            mosquitto="mosquitto",
            mosquitto_sub="mosquitto_sub",
        )
        summary = json.loads((directory / "summary.json").read_text(encoding="utf-8"))
        if code != 0 or summary["status"] != "PASS" or summary["long_soak_pass"] is not False:
            print(f"SOAK_SMOKE_RESULT=FAIL EVIDENCE={directory}")
            print((directory / "failures.json").read_text(encoding="utf-8"))
            return 1
        passed = True
        print(f"SOAK_SMOKE_RESULT=PASS EVIDENCE={directory}")
        return 0
    finally:
        if passed:
            shutil.rmtree(output_root, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())

from __future__ import annotations

import json
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.soak.profile import (  # noqa: E402
    load_and_validate_profile,
    validate_execution_request,
    validate_profile_pair,
)


class SoakProfileContractTest(unittest.TestCase):
    def test_frozen_profiles_are_valid(self) -> None:
        paths = [
            ROOT / "config/soak/software_release.json",
            ROOT / "config/soak/software_preflight.json",
            ROOT / "tests/data/soak_profiles/software_smoke.json",
        ]
        for path in paths:
            with self.subTest(path=path.name):
                profile = load_and_validate_profile(path, ROOT)
                self.assertEqual(profile["schema_version"], "1.0.0")

    def test_release_and_preflight_keep_the_same_semantics(self) -> None:
        release = load_and_validate_profile(ROOT / "config/soak/software_release.json", ROOT)
        preflight = load_and_validate_profile(ROOT / "config/soak/software_preflight.json", ROOT)
        validate_profile_pair(release, preflight)

    def test_overlapping_fault_window_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "overlap"):
            load_and_validate_profile(
                ROOT / "tests/data/soak_profiles/invalid_overlapping_faults.json", ROOT
            )

    def test_remote_broker_and_escaping_repository_paths_are_rejected(self) -> None:
        path = ROOT / "tests/data/soak_profiles/software_smoke.json"
        profile = json.loads(path.read_text(encoding="utf-8"))
        profile["load"] = load_and_validate_profile(path, ROOT)["load"]
        profile.pop("extends", None)
        profile["load"]["mqtt"]["broker_host"] = "mqtt.example.com"
        profile["load"]["register_map"] = "../private/registers.yaml"
        temp = ROOT / "tests/data/soak_profiles/.invalid_remote.json"
        try:
            temp.write_text(json.dumps(profile), encoding="utf-8")
            with self.assertRaises(ValueError):
                load_and_validate_profile(temp, ROOT)
        finally:
            temp.unlink(missing_ok=True)

    def test_formal_execution_requires_confirmed_revision_and_raw_output(self) -> None:
        release = load_and_validate_profile(ROOT / "config/soak/software_release.json", ROOT)
        with self.assertRaisesRegex(ValueError, "source_revision"):
            validate_execution_request(
                release, "uncommitted", ROOT / "artifacts/soak/release", ROOT
            )
        with self.assertRaisesRegex(ValueError, "artifacts/soak"):
            validate_execution_request(release, "66a9c95", ROOT / "artifacts/reports", ROOT)
        validate_execution_request(release, "66a9c95", ROOT / "artifacts/soak/release", ROOT)


if __name__ == "__main__":
    unittest.main()

from __future__ import annotations

import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.soak.monitor import (  # noqa: E402
    evaluate_arm64_health,
    parse_vcgencmd_temperature,
    parse_vcgencmd_throttled,
)


class Arm64HostTelemetryUnitTest(unittest.TestCase):
    def test_parses_vcgencmd_outputs(self) -> None:
        self.assertEqual(parse_vcgencmd_temperature("temp=47.8'C\n"), 47.8)
        self.assertEqual(parse_vcgencmd_throttled("throttled=0x50000\n"), 0x50000)
        self.assertIsNone(parse_vcgencmd_temperature("unsupported"))
        self.assertIsNone(parse_vcgencmd_throttled("throttled=invalid"))

    def test_healthy_sample_has_no_failures(self) -> None:
        sample = {
            "soc_temperature_c": 79.9,
            "throttled_current_bits": 0,
            "throttled_history_new_bits": 0,
        }
        self.assertEqual(
            evaluate_arm64_health(sample, maximum_temperature_c=80.0), []
        )

    def test_temperature_is_strictly_below_limit(self) -> None:
        failures = evaluate_arm64_health(
            {
                "soc_temperature_c": 80.0,
                "throttled_current_bits": 0,
                "throttled_history_new_bits": 0,
            },
            maximum_temperature_c=80.0,
        )
        self.assertIn("temperature_at_or_above_limit", failures)

    def test_missing_temperature_and_throttling_bits_fail(self) -> None:
        failures = evaluate_arm64_health(
            {
                "soc_temperature_c": None,
                "throttled_current_bits": 0x1,
                "throttled_history_new_bits": 0x10000,
            },
            maximum_temperature_c=80.0,
        )
        self.assertEqual(
            failures,
            [
                "temperature_missing",
                "current_throttling_or_undervoltage",
                "new_historical_throttling_or_undervoltage",
            ],
        )


if __name__ == "__main__":
    unittest.main()

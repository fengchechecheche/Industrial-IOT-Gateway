from __future__ import annotations

import unittest

from tools.hardware.g6_t03_preflight import (
    evaluate_preflight,
    parse_throttled,
    parse_vcgencmd_temperature,
    systemctl_show_arguments,
)


def passing_observation() -> dict[str, object]:
    return {
        "machine": "aarch64",
        "pid1": "systemd",
        "serial_path": "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0",
        "serial_exists": True,
        "serial_is_character_device": True,
        "serial_group": "dialout",
        "mosquitto_load_state": "loaded",
        "mosquitto_active_state": "active",
        "gateway_load_state": "not-found",
        "disk_free_bytes": 108 * 1024 * 1024 * 1024,
        "temperature_c": 48.7,
        "throttled_current_bits": 0,
        "throttled_history_bits": 0,
    }


class G6T03PreflightTest(unittest.TestCase):
    def test_passing_raspberry_pi_preflight_is_accepted(self) -> None:
        result = evaluate_preflight(passing_observation())
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["failures"], [])

    def test_every_environment_failure_is_reported(self) -> None:
        failed = {
            **passing_observation(),
            "machine": "x86_64",
            "pid1": "bash",
            "serial_path": "/dev/ttyUSB0",
            "serial_exists": False,
            "serial_is_character_device": False,
            "serial_group": "root",
            "mosquitto_load_state": "not-found",
            "mosquitto_active_state": "inactive",
            "gateway_load_state": "masked",
            "disk_free_bytes": 1,
            "temperature_c": 80.0,
            "throttled_current_bits": 1,
            "throttled_history_bits": 65536,
        }
        result = evaluate_preflight(failed)
        self.assertEqual(result["status"], "FAIL")
        combined = "\n".join(result["failures"])
        for marker in (
            "ARM64",
            "PID 1",
            "stable serial",
            "character device",
            "dialout",
            "Mosquitto",
            "gateway unit",
            "disk",
            "80",
            "current throttled",
            "historical throttled",
        ):
            with self.subTest(marker=marker):
                self.assertIn(marker, combined)

    def test_vcgencmd_parsers_reject_malformed_values(self) -> None:
        self.assertEqual(parse_vcgencmd_temperature("temp=48.7'C"), 48.7)
        self.assertEqual(parse_throttled("throttled=0x50000"), 0x50000)
        with self.assertRaises(ValueError):
            parse_vcgencmd_temperature("unknown")
        with self.assertRaises(ValueError):
            parse_throttled("throttled=oops")

    def test_systemctl_arguments_are_fixed_and_shell_free(self) -> None:
        arguments = systemctl_show_arguments("mosquitto.service")
        self.assertEqual(arguments[0:2], ["systemctl", "show"])
        self.assertIn("ActiveState", arguments)
        self.assertNotIn("bash", arguments)
        with self.assertRaises(ValueError):
            systemctl_show_arguments("ssh.service")


if __name__ == "__main__":
    unittest.main()

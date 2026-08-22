from __future__ import annotations

import unittest

from tools.hardware.g6_t03_reboot_controller import evaluate_post_reboot


class G6T03RebootControllerTest(unittest.TestCase):
    def test_complete_real_hardware_recovery_passes(self) -> None:
        checkpoint = {
            "boot_id_before": "boot-a",
            "source_revision": "a" * 40,
            "runner_sha256": "b" * 64,
            "installed_sha256": {"/usr/local/bin/gateway_app": "c" * 64},
        }
        observation = {
            "boot_id_before": "boot-a",
            "boot_id_after": "boot-b",
            "source_revision": "a" * 40,
            "runner_sha256": "b" * 64,
            "installed_sha256": {"/usr/local/bin/gateway_app": "c" * 64},
            "gateway_active": True,
            "mosquitto_active": True,
            "gateway_enabled": True,
            "mosquitto_enabled": True,
            "serial_by_id_ready": True,
            "serial_accessible": True,
            "journal_current_boot": True,
            "journal_required_events": True,
            "ready_within_ms": 90000,
            "slave_successes": {"1": 3, "2": 3, "4": 17},
            "mqtt_publish_successes": 23,
            "nrestarts": 0,
            "failed_units": [],
            "restart_loop": False,
            "temperature_peak_c": 59.2,
            "throttled_current_bits": 0,
            "throttled_history_new_bits": 0,
            "observation_ms": 600000,
        }
        self.assertEqual(evaluate_post_reboot(observation, checkpoint), [])

    def test_all_reboot_failures_are_reported(self) -> None:
        observation = {
            "boot_id_before": "same",
            "boot_id_after": "same",
            "gateway_active": False,
            "mosquitto_active": False,
            "gateway_enabled": False,
            "mosquitto_enabled": False,
            "serial_by_id_ready": False,
            "serial_accessible": False,
            "journal_current_boot": False,
            "journal_required_events": False,
            "ready_within_ms": 120001,
            "slave_successes": {"1": 1, "2": 0},
            "mqtt_publish_successes": 0,
            "nrestarts": 1,
            "failed_units": ["industrial_iot_gateway.service"],
            "restart_loop": True,
            "temperature_peak_c": 80.0,
            "throttled_current_bits": 1,
            "throttled_history_new_bits": 65536,
            "observation_ms": 599999,
        }
        failures = "\n".join(evaluate_post_reboot(observation))
        for marker in (
            "boot ID",
            "gateway",
            "Mosquitto",
            "enabled",
            "USB-RS485",
            "journal",
            "120",
            "slave 2",
            "slave 4",
            "MQTT",
            "NRestarts",
            "failed unit",
            "restart loop",
            "80",
            "current throttled",
            "historical throttled",
            "600",
        ):
            with self.subTest(marker=marker):
                self.assertIn(marker, failures)

    def test_checkpoint_identity_mismatch_is_rejected(self) -> None:
        checkpoint = {
            "boot_id_before": "boot-a",
            "source_revision": "a" * 40,
            "runner_sha256": "b" * 64,
            "installed_sha256": {"/usr/local/bin/gateway_app": "c" * 64},
        }
        observation = {
            "boot_id_before": "wrong-boot",
            "boot_id_after": "boot-b",
            "source_revision": "d" * 40,
            "runner_sha256": "e" * 64,
            "installed_sha256": {"/usr/local/bin/gateway_app": "f" * 64},
        }
        failures = "\n".join(evaluate_post_reboot(observation, checkpoint))
        self.assertIn("baseline does not match checkpoint", failures)
        self.assertIn("source revision", failures)
        self.assertIn("runner SHA-256", failures)
        self.assertIn("installed file SHA-256", failures)


if __name__ == "__main__":
    unittest.main()

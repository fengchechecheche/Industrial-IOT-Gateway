from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
UNIT = ROOT / "packaging/systemd/industrial_iot_gateway.service"
ENVIRONMENT = ROOT / "packaging/systemd/gateway.env.example"
CMAKE = ROOT / "CMakeLists.txt"


def directives(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith(("#", "[")):
            continue
        key, value = line.split("=", 1)
        result[key] = value
    return result


class SystemdUnitContractTest(unittest.TestCase):
    def test_service_lifecycle_and_identity_are_frozen(self) -> None:
        unit = directives(UNIT)

        self.assertEqual(unit["Type"], "simple")
        self.assertEqual(unit["User"], "iot-gw")
        self.assertEqual(unit["Group"], "iot-gw")
        self.assertEqual(unit["SupplementaryGroups"], "dialout")
        self.assertEqual(unit["Restart"], "on-failure")
        self.assertEqual(unit["RestartSec"], "2s")
        self.assertEqual(unit["RestartPreventExitStatus"], "2 3 4 5 6 7")
        self.assertEqual(unit["TimeoutStopSec"], "5s")
        self.assertEqual(unit["KillSignal"], "SIGTERM")
        self.assertEqual(unit["StandardOutput"], "journal")
        self.assertEqual(unit["StandardError"], "journal")

    def test_service_uses_explicit_paths_and_fail_fast_environment(self) -> None:
        unit = directives(UNIT)

        self.assertEqual(unit["WorkingDirectory"], "/var/lib/industrial_iot_gateway")
        self.assertEqual(unit["StateDirectory"], "industrial_iot_gateway")
        self.assertEqual(
            unit["EnvironmentFile"], "/etc/industrial_iot_gateway/gateway.env"
        )
        self.assertTrue(unit["ExecStart"].startswith("/usr/local/bin/gateway_app "))
        self.assertIn("--serial-device ${GATEWAY_SERIAL_DEVICE}", unit["ExecStart"])
        self.assertIn("--register-map ${GATEWAY_REGISTER_MAP}", unit["ExecStart"])

    def test_service_has_baseline_process_hardening(self) -> None:
        unit = directives(UNIT)

        self.assertEqual(unit["NoNewPrivileges"], "true")
        self.assertEqual(unit["PrivateTmp"], "true")
        self.assertEqual(unit["ProtectSystem"], "strict")
        self.assertEqual(unit["ProtectHome"], "true")
        self.assertEqual(unit["ProtectKernelTunables"], "true")
        self.assertEqual(unit["ProtectKernelModules"], "true")
        self.assertEqual(unit["ProtectControlGroups"], "true")
        self.assertEqual(unit["LockPersonality"], "true")
        self.assertEqual(unit["RestrictRealtime"], "true")
        self.assertEqual(unit["RestrictSUIDSGID"], "true")

    def test_environment_example_contains_only_public_placeholders(self) -> None:
        values = directives(ENVIRONMENT)

        self.assertEqual(values["GATEWAY_SERIAL_DEVICE"], "/dev/serial/by-id/REPLACE_ME")
        self.assertEqual(
            values["GATEWAY_REGISTER_MAP"],
            "/etc/industrial_iot_gateway/register_map.yaml",
        )
        self.assertNotIn("PASSWORD", values)
        self.assertNotIn("TOKEN", values)

    def test_release_install_rules_include_runtime_and_systemd_templates(self) -> None:
        cmake = CMAKE.read_text(encoding="utf-8")

        self.assertIn("install(TARGETS gateway_app", cmake)
        self.assertIn("packaging/systemd/industrial_iot_gateway.service", cmake)
        self.assertIn("packaging/systemd/gateway.env.example", cmake)
        self.assertIn("${CMAKE_INSTALL_LIBDIR}/systemd/system", cmake)
        self.assertIn("${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}/systemd", cmake)


if __name__ == "__main__":
    unittest.main()

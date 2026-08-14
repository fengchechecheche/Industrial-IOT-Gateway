from __future__ import annotations

import unittest

from tools.release.systemd_runner import (
    REQUIRED_G5_SCENARIOS,
    EnvironmentIdentity,
    SystemdContractError,
    classify_environment,
    evaluate_g5,
    parse_systemd_properties,
    validate_unit_contract,
)


class SystemdRunnerTest(unittest.TestCase):
    def test_wsl_is_development_only_even_when_pid1_is_systemd(self) -> None:
        identity = EnvironmentIdentity(
            machine="x86_64",
            pid1="systemd",
            kernel_release="6.6.87.2-microsoft-standard-WSL2",
            virtualization="wsl",
        )
        self.assertEqual(classify_environment(identity), "DEVELOPMENT_ONLY")

    def test_container_is_not_native_g5(self) -> None:
        identity = EnvironmentIdentity("x86_64", "systemd", "6.8.0", "docker")
        self.assertEqual(classify_environment(identity), "UNSUPPORTED")

    def test_native_x86_64_systemd_is_eligible(self) -> None:
        identity = EnvironmentIdentity("x86_64", "systemd", "6.8.0-31-generic", "none")
        self.assertEqual(classify_environment(identity), "NATIVE_ELIGIBLE")

    def test_full_vm_requires_explicit_allowance(self) -> None:
        identity = EnvironmentIdentity("x86_64", "systemd", "6.8.0-31-generic", "kvm")
        self.assertEqual(classify_environment(identity), "FULL_VM_REQUIRES_APPROVAL")
        self.assertEqual(classify_environment(identity, allow_full_vm=True), "NATIVE_ELIGIBLE")

    def test_arm64_requires_an_explicit_matching_target_platform(self) -> None:
        self.assertEqual(
            classify_environment(EnvironmentIdentity("aarch64", "systemd", "6.8.0", "none")),
            "UNSUPPORTED",
        )
        self.assertEqual(
            classify_environment(
                EnvironmentIdentity("aarch64", "systemd", "6.8.0", "none"),
                target_platform="linux-arm64",
            ),
            "NATIVE_ELIGIBLE",
        )
        self.assertEqual(
            classify_environment(
                EnvironmentIdentity("arm64", "systemd", "6.8.0", "none"),
                target_platform="linux-arm64",
            ),
            "NATIVE_ELIGIBLE",
        )

    def test_target_platform_mismatch_or_wrong_pid1_is_unsupported(self) -> None:
        self.assertEqual(
            classify_environment(
                EnvironmentIdentity("x86_64", "systemd", "6.8.0", "none"),
                target_platform="linux-arm64",
            ),
            "UNSUPPORTED",
        )
        self.assertEqual(
            classify_environment(
                EnvironmentIdentity("aarch64", "systemd", "6.8.0", "none"),
                target_platform="linux-x86_64",
            ),
            "UNSUPPORTED",
        )
        self.assertEqual(
            classify_environment(EnvironmentIdentity("x86_64", "init", "6.8.0", "none")),
            "UNSUPPORTED",
        )

    def test_arm64_full_vm_still_requires_explicit_allowance(self) -> None:
        identity = EnvironmentIdentity("aarch64", "systemd", "6.8.0", "kvm")
        self.assertEqual(
            classify_environment(identity, target_platform="linux-arm64"),
            "FULL_VM_REQUIRES_APPROVAL",
        )
        self.assertEqual(
            classify_environment(
                identity,
                allow_full_vm=True,
                target_platform="linux-arm64",
            ),
            "NATIVE_ELIGIBLE",
        )

    def test_unknown_target_platform_is_rejected(self) -> None:
        identity = EnvironmentIdentity("riscv64", "systemd", "6.8.0", "none")
        with self.assertRaisesRegex(SystemdContractError, "target platform"):
            classify_environment(identity, target_platform="linux-riscv64")

    def test_systemctl_properties_are_parsed_and_validated(self) -> None:
        properties = parse_systemd_properties(
            "Restart=on-failure\nRestartUSec=2s\nTimeoutStopUSec=5s\n"
            "StartLimitBurst=5\nKillSignal=15\nUser=iot-gw\n"
        )
        validate_unit_contract(properties)
        self.assertEqual(properties["User"], "iot-gw")

    def test_invalid_unit_contract_fails(self) -> None:
        properties = {
            "Restart": "always",
            "RestartUSec": "2s",
            "TimeoutStopUSec": "5s",
            "StartLimitBurst": "5",
            "KillSignal": "15",
            "User": "iot-gw",
        }
        with self.assertRaisesRegex(SystemdContractError, "Restart"):
            validate_unit_contract(properties)

    def test_all_required_scenarios_are_needed_for_pass(self) -> None:
        records = {name: {"status": "PASS"} for name in REQUIRED_G5_SCENARIOS}
        summary = evaluate_g5(records, environment_class="NATIVE_ELIGIBLE")
        self.assertEqual(summary["status"], "PASS")
        del records[REQUIRED_G5_SCENARIOS[0]]
        summary = evaluate_g5(records, environment_class="NATIVE_ELIGIBLE")
        self.assertEqual(summary["status"], "FAIL")
        self.assertIn(REQUIRED_G5_SCENARIOS[0], summary["missing_scenarios"])

    def test_development_environment_can_never_produce_g5_pass(self) -> None:
        records = {name: {"status": "PASS"} for name in REQUIRED_G5_SCENARIOS}
        summary = evaluate_g5(records, environment_class="DEVELOPMENT_ONLY")
        self.assertEqual(summary["status"], "DEVELOPMENT_ONLY")

    def test_any_failed_scenario_fails_g5(self) -> None:
        records = {name: {"status": "PASS"} for name in REQUIRED_G5_SCENARIOS}
        records["sigterm_stop"] = {"status": "FAIL", "stop_ms": 5100}
        summary = evaluate_g5(records, environment_class="NATIVE_ELIGIBLE")
        self.assertEqual(summary["status"], "FAIL")
        self.assertEqual(summary["failed_scenarios"], ["sigterm_stop"])


if __name__ == "__main__":
    unittest.main()

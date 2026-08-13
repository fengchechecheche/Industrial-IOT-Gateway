from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class ReleaseContractTest(unittest.TestCase):
    def test_mit_license_is_frozen(self) -> None:
        license_path = ROOT / "LICENSE"
        self.assertTrue(license_path.is_file(), "LICENSE is required for v0.1.0")
        text = license_path.read_text(encoding="utf-8")
        self.assertIn("MIT License", text)
        self.assertIn("Copyright (c) 2026 fengchechecheche", text)
        self.assertIn("Permission is hereby granted, free of charge", text)
        self.assertIn('THE SOFTWARE IS PROVIDED "AS IS"', text)

    def test_readme_freezes_scope_license_and_demo(self) -> None:
        text = (ROOT / "README.md").read_text(encoding="utf-8")
        self.assertNotIn("项目许可证尚未最终确定", text)
        self.assertIn("MIT License", text)
        self.assertIn("(LICENSE)", text)
        self.assertIn("python3 tools/demo.py --profile pty-mqtt", text)
        self.assertIn("Linux x86_64", text)
        self.assertIn("不代表", text)
        self.assertIn("真实 USB-RS485", text)

    def test_third_party_notices_cover_delivery_boundaries(self) -> None:
        path = ROOT / "THIRD_PARTY_NOTICES.md"
        self.assertTrue(path.is_file(), "THIRD_PARTY_NOTICES.md is required")
        text = path.read_text(encoding="utf-8")
        for required in (
            "yaml-cpp",
            "0.8.0+dfsg-6build1",
            "X11",
            "Eclipse Paho MQTT C++",
            "1.2.0-2",
            "EPL-2.0",
            "Eclipse Paho MQTT C",
            "1.3.13-1build2",
            "nlohmann/json",
            "3.11.3-1",
            "GoogleTest",
            "Mosquitto",
            "测试专用",
            "不随本项目安装包分发",
        ):
            self.assertIn(required, text)

    def test_inventory_matches_fixed_ubuntu_packages(self) -> None:
        text = (ROOT / "docs/third_party_inventory.md").read_text(encoding="utf-8")
        for required in (
            "0.8.0+dfsg-6build1",
            "X11",
            "1.2.0-2",
            "1.3.13-1build2",
            "EPL-2.0",
            "3.11.3-1",
            "编译进入",
            "动态链接",
            "测试专用",
            "THIRD_PARTY_NOTICES.md",
        ):
            self.assertIn(required, text)

    def test_cmake_installs_release_contract_files(self) -> None:
        text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        for required in (
            "CMAKE_INSTALL_DOCDIR",
            "LICENSE",
            "THIRD_PARTY_NOTICES.md",
            "README.md",
            "docs/runbook.md",
            "config/examples/register_map.yaml",
            "config/examples/pty_slave_scenarios.yaml",
        ):
            self.assertIn(required, text)


if __name__ == "__main__":
    unittest.main()

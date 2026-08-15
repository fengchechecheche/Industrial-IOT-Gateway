from __future__ import annotations

import json
import pathlib
import tempfile
import unittest

from tools.release.manifest import (
    REQUIRED_PACKAGE_PATHS,
    ManifestError,
    build_manifest,
    build_manifest_v2,
    scan_release_tree,
    sha256_file,
    validate_source_revision,
    write_sha256sums,
)


REVISION = "4af261498a6c2561c6085a2f1ca57ef68924ea17"


def create_package(root: pathlib.Path) -> None:
    for index, relative in enumerate(REQUIRED_PACKAGE_PATHS):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"fixture-{index}\n", encoding="utf-8")


class ReleaseManifestTest(unittest.TestCase):
    def test_source_revision_requires_full_lowercase_sha1(self) -> None:
        self.assertEqual(validate_source_revision(REVISION), REVISION)
        for invalid in ("4af2614", REVISION.upper(), "g" * 40, "", "../revision"):
            with self.subTest(invalid=invalid):
                with self.assertRaises(ManifestError):
                    validate_source_revision(invalid)

    def test_manifest_is_sorted_and_contains_release_boundaries(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            package_root = pathlib.Path(directory)
            create_package(package_root)
            manifest = build_manifest(
                package_root,
                release_name="industrial_iot_gateway-0.1.0-linux-x86_64",
                source_revision=REVISION,
                environment={"architecture": "x86_64"},
                gates={"G1": "PASS", "G5": "PASS", "G6": "NOT_RUN_OPTIONAL"},
            )

        paths = [entry["path"] for entry in manifest["files"]]
        self.assertEqual(paths, sorted(paths))
        self.assertEqual(manifest["source_revision"], REVISION)
        self.assertFalse(manifest["hardware_validated"])
        self.assertFalse(manifest["arm64_validated"])
        self.assertEqual(manifest["gates"]["G6"], "NOT_RUN_OPTIONAL")
        self.assertTrue(all(not pathlib.PurePosixPath(path).is_absolute() for path in paths))
        self.assertTrue(all(".." not in pathlib.PurePosixPath(path).parts for path in paths))

    def test_manifest_records_mode_size_and_sha256(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            package_root = pathlib.Path(directory)
            create_package(package_root)
            manifest = build_manifest(
                package_root,
                release_name="industrial_iot_gateway-0.1.0-linux-x86_64",
                source_revision=REVISION,
                environment={},
                gates={},
            )
            entry = manifest["files"][0]
            self.assertRegex(str(entry["mode"]), r"^0[0-7]{3}$")
            self.assertGreater(entry["bytes"], 0)
            self.assertRegex(str(entry["sha256"]), r"^[0-9a-f]{64}$")
            self.assertEqual(entry["type"], "file")

    def test_manifest_v2_records_platform_and_capability_boundaries(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            package_root = pathlib.Path(directory)
            create_package(package_root)
            manifest = build_manifest_v2(
                package_root,
                release_name="industrial_iot_gateway-0.1.0-linux-arm64",
                source_revision=REVISION,
                environment={"os": "Ubuntu 24.04"},
                gates={"G5": "PASS", "G6": "WAITING_FOR_HARDWARE"},
                target_platform="linux-arm64",
                machine="aarch64",
                package_architecture="arm64",
                release_status="candidate",
                capabilities={
                    "x86_64_validated": False,
                    "arm64_native_build_validated": True,
                    "arm64_systemd_validated": True,
                    "arm64_long_soak_validated": False,
                    "arm64_release_bundle_ready": False,
                    "hardware_validated": False,
                },
            )

        self.assertEqual(manifest["schema_version"], "p3-s7-release-manifest-v2")
        self.assertEqual(manifest["target_platform"], "linux-arm64")
        self.assertEqual(manifest["machine"], "aarch64")
        self.assertEqual(manifest["package_architecture"], "arm64")
        self.assertEqual(manifest["release_status"], "candidate")
        self.assertTrue(manifest["capabilities"]["arm64_native_build_validated"])
        self.assertTrue(manifest["capabilities"]["arm64_systemd_validated"])
        self.assertFalse(manifest["capabilities"]["arm64_long_soak_validated"])
        self.assertFalse(manifest["hardware_validated"])
        self.assertFalse(manifest["published"])
        self.assertIsNone(manifest["tag"])

    def test_manifest_v2_rejects_inconsistent_platform_fields_and_capabilities(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            package_root = pathlib.Path(directory)
            create_package(package_root)
            base = {
                "package_root": package_root,
                "release_name": "industrial_iot_gateway-0.1.0-linux-arm64",
                "source_revision": REVISION,
                "environment": {},
                "gates": {},
                "target_platform": "linux-arm64",
                "machine": "aarch64",
                "package_architecture": "arm64",
                "release_status": "candidate",
                "capabilities": {
                    "x86_64_validated": False,
                    "arm64_native_build_validated": True,
                    "arm64_systemd_validated": True,
                    "arm64_long_soak_validated": False,
                    "arm64_release_bundle_ready": False,
                    "hardware_validated": False,
                },
            }
            with self.assertRaisesRegex(ManifestError, "package architecture"):
                build_manifest_v2(**{**base, "package_architecture": "x86_64"})
            with self.assertRaisesRegex(ManifestError, "release status"):
                build_manifest_v2(**{**base, "release_status": "published"})
            incomplete = dict(base["capabilities"])
            incomplete.pop("hardware_validated")
            with self.assertRaisesRegex(ManifestError, "capability fields"):
                build_manifest_v2(**{**base, "capabilities": incomplete})

    def test_missing_required_package_file_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            package_root = pathlib.Path(directory)
            create_package(package_root)
            (package_root / REQUIRED_PACKAGE_PATHS[0]).unlink()
            with self.assertRaisesRegex(ManifestError, "missing required package files"):
                build_manifest(
                    package_root,
                    release_name="industrial_iot_gateway-0.1.0-linux-x86_64",
                    source_revision=REVISION,
                    environment={},
                    gates={},
                )

    def test_symlink_escaping_package_root_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            package_root = pathlib.Path(directory)
            create_package(package_root)
            (package_root / "escape").symlink_to("../../outside")
            with self.assertRaisesRegex(ManifestError, "symlink escapes package root"):
                build_manifest(
                    package_root,
                    release_name="industrial_iot_gateway-0.1.0-linux-x86_64",
                    source_revision=REVISION,
                    environment={},
                    gates={},
                )

    def test_sha256sums_is_sorted_and_self_verifiable(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            first = root / "z.txt"
            second = root / "a.txt"
            first.write_text("z", encoding="utf-8")
            second.write_text("a", encoding="utf-8")
            output = root / "SHA256SUMS"
            write_sha256sums([first, second], root=root, output=output)
            lines = output.read_text(encoding="utf-8").splitlines()
            self.assertTrue(lines[0].endswith("  a.txt"))
            self.assertTrue(lines[1].endswith("  z.txt"))
            expected = {line.split("  ", 1)[1]: line.split("  ", 1)[0] for line in lines}
            self.assertEqual(expected["a.txt"], sha256_file(second))
            self.assertEqual(expected["z.txt"], sha256_file(first))

    def test_manifest_is_json_serializable_without_private_absolute_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            package_root = pathlib.Path(directory)
            create_package(package_root)
            manifest = build_manifest(
                package_root,
                release_name="industrial_iot_gateway-0.1.0-linux-x86_64",
                source_revision=REVISION,
                environment={"os": "Ubuntu 24.04"},
                gates={},
            )
            encoded = json.dumps(manifest, ensure_ascii=False, sort_keys=True)
            self.assertNotIn(str(package_root), encoded)

    def test_secret_and_private_path_scan_reports_release_blockers(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "safe.txt").write_text("broker=tcp://127.0.0.1:1883\n", encoding="utf-8")
            (root / "unsafe.env").write_text(
                "password=hunter2\npath=/home/private-user/project\n",
                encoding="utf-8",
            )
            findings = scan_release_tree(root)
            categories = {finding["category"] for finding in findings}
            self.assertEqual(categories, {"credential", "private_path"})
            self.assertTrue(all("hunter2" not in finding["excerpt"] for finding in findings))


if __name__ == "__main__":
    unittest.main()

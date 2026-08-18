from __future__ import annotations

import hashlib
import json
import pathlib
import tempfile
import unittest

from tools.release.local_dual_arch_release import (
    LocalReleaseInputError,
    assemble_local_release,
    parse_arguments,
)


REVISION = "2a0c961bb9677fb3cc54b1e57be88a96cd9db82b"
BUNDLE_MARKER = "PASS_P3_S7_ARM64_T03_REPRODUCIBLE_BUNDLE"
T04_MARKER = "PASS_P3_S7_ARM64_T04_LONG_SOAK_AND_REBOOT"


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_sums(
    root: pathlib.Path, paths: list[pathlib.Path], *, output: pathlib.Path | None = None
) -> None:
    (output or root / "SHA256SUMS").write_text(
        "\n".join(f"{sha256(path)}  {path.relative_to(root).as_posix()}" for path in paths)
        + "\n",
        encoding="utf-8",
    )


def bundle_evidence(root: pathlib.Path, platform: str) -> pathlib.Path:
    run = root / f"run-{platform}"
    bundle = run / "bundle"
    bundle.mkdir(parents=True)
    name = f"industrial_iot_gateway-0.1.0-{platform}.tar.gz"
    package = bundle / name
    package.write_bytes(platform.encode())
    manifest = bundle / "install_manifest.json"
    manifest.write_text(
        json.dumps(
            {
                "schema_version": "p3-s7-release-manifest-v2",
                "release_status": "final",
                "source_revision": REVISION,
                "target_platform": platform,
                "published": False,
                "tag": None,
                "hardware_validated": False,
                "capabilities": {"hardware_validated": False},
                "files": list(range(9)),
            }
        ),
        encoding="utf-8",
    )
    (run / "summary.json").write_text(
        json.dumps(
            {
                "status": "PASS",
                "pass_marker": BUNDLE_MARKER,
                "target_platform": platform,
                "package_name": name,
                "package_sha256": sha256(package),
                "install_file_count": 9,
                "reproducible_archive": True,
                "hardware_validated": False,
            }
        ),
        encoding="utf-8",
    )
    (run / BUNDLE_MARKER).write_text("pass\n", encoding="utf-8")
    write_sums(run, [package, manifest], output=bundle / "SHA256SUMS")
    return run


def t04_evidence(root: pathlib.Path) -> pathlib.Path:
    run = root / "run-t04"
    run.mkdir(parents=True)
    payloads: dict[str, object] = {
        "summary.json": {
            "status": "PASS",
            "pass_marker": T04_MARKER,
            "source_revision": REVISION,
            "cleanup_ok": True,
            "hardware_validated": False,
        },
        "observation.json": {
            "observation_seconds": 637.4,
            "cleanup": {"reset_failed_ok": True, "owned_paths_absent": True},
        },
        "failures.json": [],
    }
    for name, payload in payloads.items():
        (run / name).write_text(json.dumps(payload), encoding="utf-8")
    (run / T04_MARKER).write_text("pass\n", encoding="utf-8")
    write_sums(run, [run / name for name in payloads])
    return run


class LocalDualArchReleaseContractTest(unittest.TestCase):
    def test_success_has_exact_files_and_safe_capabilities(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            output = root / "local_release" / "v0.1.0"
            assemble_local_release(
                source_revision=REVISION,
                version="0.1.0",
                x86_bundle_evidence=bundle_evidence(root / "x86", "linux-x86_64"),
                arm64_bundle_evidence=bundle_evidence(root / "arm64", "linux-arm64"),
                arm64_t04_evidence=t04_evidence(root / "t04"),
                output_root=output,
            )
            self.assertEqual(
                {path.name for path in output.iterdir()},
                {
                    "industrial_iot_gateway-0.1.0-linux-x86_64.tar.gz",
                    "industrial_iot_gateway-0.1.0-linux-arm64.tar.gz",
                    "SHA256SUMS",
                    "release_index.json",
                    "RELEASE_NOTES.md",
                },
            )
            index = json.loads((output / "release_index.json").read_text(encoding="utf-8"))
            self.assertEqual(index["release_status"], "LOCAL_RELEASE_READY")
            self.assertFalse(index["published"])
            self.assertIsNone(index["tag"])
            self.assertFalse(index["hardware_validated"])
            self.assertTrue(index["x86_64_validated"])
            self.assertTrue(index["arm64_long_soak_validated"])
            self.assertTrue(index["arm64_release_bundle_ready"])
            self.assertEqual(len(index["assets"]), 2)
            notes = (output / "RELEASE_NOTES.md").read_text(encoding="utf-8")
            self.assertIn("PUBLISHED=false", notes)
            self.assertIn("HARDWARE_VALIDATED=false", notes)
            for line in (output / "SHA256SUMS").read_text().splitlines():
                digest, name = line.split("  ", 1)
                self.assertEqual(digest, sha256(output / name))

    def test_rejects_nonfinal_or_revision_drift(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for field, value, message in (
                ("release_status", "candidate", "final"),
                ("source_revision", "0" * 40, "source revision"),
            ):
                with self.subTest(field=field):
                    case = root / field
                    x86 = bundle_evidence(case / "x86", "linux-x86_64")
                    arm64 = bundle_evidence(case / "arm64", "linux-arm64")
                    t04 = t04_evidence(case / "t04")
                    manifest = arm64 / "bundle" / "install_manifest.json"
                    payload = json.loads(manifest.read_text())
                    payload[field] = value
                    manifest.write_text(json.dumps(payload), encoding="utf-8")
                    write_sums(
                        arm64,
                        [arm64 / "bundle" / "industrial_iot_gateway-0.1.0-linux-arm64.tar.gz", manifest],
                        output=arm64 / "bundle" / "SHA256SUMS",
                    )
                    with self.assertRaisesRegex(LocalReleaseInputError, message):
                        assemble_local_release(
                            source_revision=REVISION,
                            version="0.1.0",
                            x86_bundle_evidence=x86,
                            arm64_bundle_evidence=arm64,
                            arm64_t04_evidence=t04,
                            output_root=case / "output",
                        )

    def test_rejects_checksum_failure_t04_failure_and_existing_output(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            x86 = bundle_evidence(root / "x86", "linux-x86_64")
            arm64 = bundle_evidence(root / "arm64", "linux-arm64")
            t04 = t04_evidence(root / "t04")
            package = x86 / "bundle" / "industrial_iot_gateway-0.1.0-linux-x86_64.tar.gz"
            package.write_bytes(b"tampered")
            with self.assertRaisesRegex(LocalReleaseInputError, "checksum"):
                assemble_local_release(
                    source_revision=REVISION,
                    version="0.1.0",
                    x86_bundle_evidence=x86,
                    arm64_bundle_evidence=arm64,
                    arm64_t04_evidence=t04,
                    output_root=root / "checksum-output",
                )
            x86 = bundle_evidence(root / "x86-clean", "linux-x86_64")
            summary = t04 / "summary.json"
            payload = json.loads(summary.read_text())
            payload["status"] = "FAIL"
            summary.write_text(json.dumps(payload), encoding="utf-8")
            write_sums(t04, [summary, t04 / "observation.json", t04 / "failures.json"])
            with self.assertRaisesRegex(LocalReleaseInputError, "T04"):
                assemble_local_release(
                    source_revision=REVISION,
                    version="0.1.0",
                    x86_bundle_evidence=x86,
                    arm64_bundle_evidence=arm64,
                    arm64_t04_evidence=t04,
                    output_root=root / "t04-output",
                )
            existing = root / "existing"
            existing.mkdir()
            with self.assertRaisesRegex(LocalReleaseInputError, "already exists"):
                assemble_local_release(
                    source_revision=REVISION,
                    version="0.1.0",
                    x86_bundle_evidence=x86,
                    arm64_bundle_evidence=arm64,
                    arm64_t04_evidence=t04,
                    output_root=existing,
                )

    def test_parser_freezes_cli(self) -> None:
        arguments = parse_arguments(
            [
                "--source-revision",
                REVISION,
                "--x86-bundle-evidence",
                "/x86",
                "--arm64-bundle-evidence",
                "/arm64",
                "--arm64-t04-evidence",
                "/t04",
                "--output-root",
                "/output",
            ]
        )
        self.assertEqual(arguments.version, "0.1.0")
        self.assertEqual(arguments.source_revision, REVISION)


if __name__ == "__main__":
    unittest.main()

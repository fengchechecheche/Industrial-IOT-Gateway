from __future__ import annotations

import gzip
import json
import pathlib
import platform
import tarfile
import tempfile
import unittest
from unittest import mock

from tools.release.manifest import REQUIRED_PACKAGE_PATHS, sha256_file
from tools.fault_matrix.models import ProcessResult
from tools.release.native_bundle_runner import (
    NativeBundleInputError,
    build_native_bundle_steps,
    create_deterministic_archive,
    package_filename,
    parse_arguments,
    platform_contract,
    resolve_pty_alias,
    validate_native_platform,
    validate_repository_state,
)


REVISION = "4af261498a6c2561c6085a2f1ca57ef68924ea17"


def create_package(root: pathlib.Path) -> None:
    for index, relative in enumerate(REQUIRED_PACKAGE_PATHS):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"fixture-{index}\n", encoding="utf-8")
    (root / REQUIRED_PACKAGE_PATHS[0]).chmod(0o755)


class NativeBundleRunnerTest(unittest.TestCase):
    def test_formal_runner_requires_exact_head_and_clean_worktree(self) -> None:
        def response(stdout: str, returncode: int = 0) -> ProcessResult:
            return ProcessResult(
                command=["git"],
                returncode=returncode,
                stdout=stdout,
                stderr="",
                duration_ms=1,
                timed_out=False,
                termination_signal=None,
            )

        clean = iter((response(REVISION + "\n"), response("")))
        validate_repository_state(
            pathlib.Path("/tmp/repository"),
            REVISION,
            command_runner=lambda *_args, **_kwargs: next(clean),
        )
        dirty = iter((response(REVISION + "\n"), response(" M tools/release/manifest.py\n")))
        with self.assertRaisesRegex(NativeBundleInputError, "clean committed worktree"):
            validate_repository_state(
                pathlib.Path("/tmp/repository"),
                REVISION,
                command_runner=lambda *_args, **_kwargs: next(dirty),
            )
        wrong = iter((response("0" * 40 + "\n"),))
        with self.assertRaisesRegex(NativeBundleInputError, "committed HEAD"):
            validate_repository_state(
                pathlib.Path("/tmp/repository"),
                REVISION,
                command_runner=lambda *_args, **_kwargs: next(wrong),
            )

    def test_platform_contract_accepts_linux_native_aliases(self) -> None:
        self.assertEqual(platform_contract("linux-x86_64", "x86_64").package_architecture, "x86_64")
        self.assertEqual(platform_contract("linux-x86_64", "amd64").package_architecture, "x86_64")
        self.assertEqual(platform_contract("linux-arm64", "aarch64").package_architecture, "arm64")
        self.assertEqual(platform_contract("linux-arm64", "arm64").package_architecture, "arm64")

    def test_wrong_or_unknown_architecture_is_rejected(self) -> None:
        with self.assertRaisesRegex(NativeBundleInputError, "does not match"):
            validate_native_platform("linux-arm64", machine="x86_64", system="Linux")
        with self.assertRaisesRegex(NativeBundleInputError, "unsupported target"):
            platform_contract("linux-riscv64", "riscv64")
        with self.assertRaisesRegex(NativeBundleInputError, "requires Linux"):
            validate_native_platform("linux-x86_64", machine="x86_64", system="Windows")

    def test_pty_alias_must_resolve_to_numeric_entry_below_allowed_parent(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            pts = root / "pts"
            pts.mkdir()
            target = pts / "7"
            target.write_text("fixture", encoding="utf-8")
            alias = root / "gateway-pty"
            alias.symlink_to(target)
            self.assertEqual(
                resolve_pty_alias(str(alias), allowed_parent=pts),
                str(target.resolve()),
            )
            invalid = pts / "not-numeric"
            invalid.write_text("fixture", encoding="utf-8")
            invalid_alias = root / "invalid-pty"
            invalid_alias.symlink_to(invalid)
            with self.assertRaisesRegex(NativeBundleInputError, "must resolve beneath"):
                resolve_pty_alias(str(invalid_alias), allowed_parent=pts)

    def test_package_names_are_frozen_by_target_platform(self) -> None:
        self.assertEqual(
            package_filename("0.1.0", "linux-x86_64"),
            "industrial_iot_gateway-0.1.0-linux-x86_64.tar.gz",
        )
        self.assertEqual(
            package_filename("0.1.0", "linux-arm64"),
            "industrial_iot_gateway-0.1.0-linux-arm64.tar.gz",
        )
        with self.assertRaises(NativeBundleInputError):
            package_filename("0.2.0", "linux-arm64")

    def test_deterministic_archive_is_identical_across_independent_roots(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            first_root = root / "stage-a"
            second_root = root / "stage-b"
            create_package(first_root)
            create_package(second_root)
            for path in second_root.rglob("*"):
                if path.is_file():
                    path.touch()
            first = root / "a.tar.gz"
            second = root / "b.tar.gz"
            create_deterministic_archive(
                first_root,
                first,
                archive_root="industrial_iot_gateway-0.1.0-linux-arm64",
            )
            create_deterministic_archive(
                second_root,
                second,
                archive_root="industrial_iot_gateway-0.1.0-linux-arm64",
            )
            self.assertEqual(sha256_file(first), sha256_file(second))
            with gzip.open(first, "rb") as stream:
                with tarfile.open(fileobj=stream, mode="r:") as archive:
                    names = archive.getnames()
                    self.assertEqual(names, sorted(names))
                    self.assertEqual(len([name for name in names if name.endswith("gateway_app")]), 1)
                    self.assertTrue(all(member.uid == 0 and member.gid == 0 for member in archive))
                    self.assertTrue(all(member.mtime == 0 for member in archive))

    def test_archive_rejects_output_inside_tree_and_escaping_symlink(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            create_package(root)
            with self.assertRaisesRegex(NativeBundleInputError, "outside package root"):
                create_deterministic_archive(root, root / "bundle.tar.gz", archive_root="bundle")
            (root / "escape").symlink_to("../../outside")
            with self.assertRaisesRegex(NativeBundleInputError, "symlink escapes"):
                create_deterministic_archive(root, root.parent / "bundle.tar.gz", archive_root="bundle")

    def test_parser_freezes_cli_contract_and_arm64_jobs_four(self) -> None:
        arguments = parse_arguments(
            [
                "--source-revision",
                REVISION,
                "--target-platform",
                "linux-arm64",
                "--version",
                "0.1.0",
                "--jobs",
                "4",
                "--artifact-root",
                "artifacts/releases/v0.1.0/arm64",
                "--work-root",
                "build/native-bundle-arm64",
                "--release-status",
                "candidate",
            ]
        )
        self.assertEqual(arguments.jobs, 4)
        self.assertEqual(arguments.target_platform, "linux-arm64")
        self.assertEqual(arguments.release_status, "candidate")

    def test_build_plan_has_release_ctest_two_installs_and_static_unit_checks(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            work = root / "build" / "native"
            steps = build_native_bundle_steps(root, work, jobs=4)
        labels = [step.label for step in steps]
        self.assertEqual(
            labels,
            [
                "release_configure",
                "release_build",
                "release_test",
                "install_a",
                "install_b",
                "systemd_verify_a",
                "systemd_verify_b",
            ],
        )
        encoded = json.dumps([step.command for step in steps])
        self.assertIn("--parallel", encoded)
        self.assertIn('"4"', encoded)
        self.assertIn("GATEWAY_ENABLE_MQTT=ON", encoded)
        self.assertIn("GATEWAY_BUILD_PTY_SLAVE=ON", encoded)
        self.assertIn("--no-tests=error", encoded)

    def test_runtime_platform_probe_uses_current_machine(self) -> None:
        target = "linux-arm64" if platform.machine().lower() in {"aarch64", "arm64"} else "linux-x86_64"
        with mock.patch("tools.release.native_bundle_runner.platform_module.system", return_value="Linux"):
            contract = validate_native_platform(target, machine=platform.machine())
        self.assertEqual(contract.target_platform, target)

    def test_source_uses_no_shell_or_global_kill(self) -> None:
        source = pathlib.Path(
            __import__("tools.release.native_bundle_runner", fromlist=["__file__"]).__file__
        ).read_text(encoding="utf-8")
        self.assertNotIn("shell=True", source)
        self.assertNotIn("pkill", source)
        self.assertNotIn("killall", source)


if __name__ == "__main__":
    unittest.main()

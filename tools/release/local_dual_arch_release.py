#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import shutil
import sys
import tempfile
from collections.abc import Sequence
from typing import Any


REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from tools.release.manifest import validate_source_revision  # noqa: E402


FROZEN_VERSION = "0.1.0"
BUNDLE_PASS_MARKER = "PASS_P3_S7_ARM64_T03_REPRODUCIBLE_BUNDLE"
T04_PASS_MARKER = "PASS_P3_S7_ARM64_T04_LONG_SOAK_AND_REBOOT"
T06_PASS_MARKER = "PASS_P3_S7_T06_LOCAL_DUAL_ARCH_RELEASE_READY"
PLATFORMS = ("linux-x86_64", "linux-arm64")


class LocalReleaseInputError(ValueError):
    """Raised when the local dual-architecture release contract is violated."""


def _sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _load_json(path: pathlib.Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise LocalReleaseInputError(f"could not read JSON evidence: {path}") from error


def _require_directory(path: pathlib.Path, label: str) -> pathlib.Path:
    resolved = path.resolve()
    if not resolved.is_dir():
        raise LocalReleaseInputError(f"{label} is not a directory: {path}")
    return resolved


def _verify_checksums(
    root: pathlib.Path, *, checksum_file: pathlib.Path | None = None
) -> None:
    checksum_file = checksum_file or root / "SHA256SUMS"
    if not checksum_file.is_file():
        raise LocalReleaseInputError(f"checksum file is missing: {checksum_file}")
    try:
        lines = checksum_file.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise LocalReleaseInputError(f"could not read checksum file: {checksum_file}") from error
    if not lines:
        raise LocalReleaseInputError(f"checksum file is empty: {checksum_file}")
    for line in lines:
        try:
            expected, relative = line.split("  ", 1)
        except ValueError as error:
            raise LocalReleaseInputError(f"invalid checksum line: {line!r}") from error
        if len(expected) != 64 or any(character not in "0123456789abcdef" for character in expected):
            raise LocalReleaseInputError(f"invalid checksum digest: {expected!r}")
        candidate = (root / relative).resolve()
        try:
            candidate.relative_to(root.resolve())
        except ValueError as error:
            raise LocalReleaseInputError(f"checksum path escapes evidence root: {relative}") from error
        if not candidate.is_file() or _sha256(candidate) != expected:
            raise LocalReleaseInputError(f"checksum mismatch: {relative}")


def _package_name(version: str, platform: str) -> str:
    return f"industrial_iot_gateway-{version}-{platform}.tar.gz"


def _validate_bundle_evidence(
    evidence: pathlib.Path,
    *,
    platform: str,
    version: str,
    source_revision: str,
) -> dict[str, Any]:
    evidence = _require_directory(evidence, f"{platform} bundle evidence")
    bundle = _require_directory(evidence / "bundle", f"{platform} bundle directory")
    _verify_checksums(evidence, checksum_file=bundle / "SHA256SUMS")
    summary = _load_json(evidence / "summary.json")
    manifest = _load_json(bundle / "install_manifest.json")
    package_name = _package_name(version, platform)
    package = bundle / package_name
    if summary.get("status") != "PASS" or summary.get("pass_marker") != BUNDLE_PASS_MARKER:
        raise LocalReleaseInputError(f"{platform} bundle summary is not PASS")
    if not (evidence / BUNDLE_PASS_MARKER).is_file():
        raise LocalReleaseInputError(f"{platform} bundle PASS marker is missing")
    if manifest.get("schema_version") != "p3-s7-release-manifest-v2":
        raise LocalReleaseInputError(f"{platform} manifest schema is not v2")
    if manifest.get("release_status") != "final":
        raise LocalReleaseInputError(f"{platform} manifest must have final release status")
    if manifest.get("source_revision") != source_revision:
        raise LocalReleaseInputError(f"{platform} source revision does not match")
    if manifest.get("target_platform") != platform:
        raise LocalReleaseInputError(f"{platform} manifest target platform does not match")
    if manifest.get("published") is not False or manifest.get("tag") is not None:
        raise LocalReleaseInputError(f"{platform} manifest violates unpublished release boundary")
    if manifest.get("hardware_validated") is not False:
        raise LocalReleaseInputError(f"{platform} manifest violates hardware validation boundary")
    if manifest.get("capabilities", {}).get("hardware_validated") is not False:
        raise LocalReleaseInputError(f"{platform} capability violates hardware validation boundary")
    if len(manifest.get("files", [])) != 9 or summary.get("install_file_count") != 9:
        raise LocalReleaseInputError(f"{platform} package must contain exactly 9 product files")
    if summary.get("reproducible_archive") is not True:
        raise LocalReleaseInputError(f"{platform} archive is not reproducible")
    if summary.get("target_platform") != platform or summary.get("package_name") != package_name:
        raise LocalReleaseInputError(f"{platform} summary package identity does not match")
    if summary.get("hardware_validated") is not False:
        raise LocalReleaseInputError(f"{platform} summary violates hardware validation boundary")
    if not package.is_file() or summary.get("package_sha256") != _sha256(package):
        raise LocalReleaseInputError(f"{platform} package checksum does not match summary")
    return {
        "evidence": evidence,
        "run_id": evidence.name,
        "package": package,
        "package_name": package_name,
        "package_bytes": package.stat().st_size,
        "package_sha256": _sha256(package),
    }


def _validate_t04_evidence(evidence: pathlib.Path, source_revision: str) -> dict[str, Any]:
    evidence = _require_directory(evidence, "ARM64 T04 evidence")
    _verify_checksums(evidence)
    summary = _load_json(evidence / "summary.json")
    observation = _load_json(evidence / "observation.json")
    failures = _load_json(evidence / "failures.json")
    if summary.get("status") != "PASS" or summary.get("pass_marker") != T04_PASS_MARKER:
        raise LocalReleaseInputError("ARM64 T04 summary is not PASS")
    if not (evidence / T04_PASS_MARKER).is_file():
        raise LocalReleaseInputError("ARM64 T04 PASS marker is missing")
    if summary.get("source_revision") != source_revision:
        raise LocalReleaseInputError("ARM64 T04 source revision does not match")
    if summary.get("cleanup_ok") is not True or failures != []:
        raise LocalReleaseInputError("ARM64 T04 cleanup or failures are not closed")
    if summary.get("hardware_validated") is not False:
        raise LocalReleaseInputError("ARM64 T04 violates hardware validation boundary")
    if float(observation.get("observation_seconds", 0.0)) < 600.0:
        raise LocalReleaseInputError("ARM64 T04 observation is shorter than 600 seconds")
    cleanup = observation.get("cleanup", {})
    if cleanup and (not all(cleanup.values()) or cleanup.get("reset_failed_ok") is not True):
        raise LocalReleaseInputError("ARM64 T04 cleanup oracle is not fully PASS")
    return {"run_id": evidence.name, "evidence": evidence}


def _release_notes(source_revision: str) -> str:
    return f"""# Industrial-IOT-Gateway v0.1.0 本地双架构 Release

本地 Release 集绑定候选提交 `{source_revision}`，包含 Linux x86_64 与 Linux ARM64 两个软件包。

- 软件范围：PTY 串口仿真、本地 Mosquitto、Modbus RTU 轮询与 MQTT 上行；
- 平台范围：Ubuntu 24.04 原生 Linux x86_64 与 Raspberry Pi 4B Ubuntu 24.04 ARM64；
- 项目自有代码许可证：MIT；
- 未创建 Git tag，未上传 GitHub Release；
- 未完成真实 USB-RS485、商用 Modbus 从站、STM32 或 CAN 硬件 G6。

```text
PUBLISHED=false
HARDWARE_VALIDATED=false
TAG=null
```
"""


def assemble_local_release(
    *,
    source_revision: str,
    version: str,
    x86_bundle_evidence: pathlib.Path,
    arm64_bundle_evidence: pathlib.Path,
    arm64_t04_evidence: pathlib.Path,
    output_root: pathlib.Path,
) -> pathlib.Path:
    try:
        validate_source_revision(source_revision)
    except ValueError as error:
        raise LocalReleaseInputError(str(error)) from error
    if version != FROZEN_VERSION:
        raise LocalReleaseInputError(f"only frozen version {FROZEN_VERSION} is accepted")
    output_root = output_root.resolve()
    if output_root.exists():
        raise LocalReleaseInputError(f"output root already exists: {output_root}")
    x86 = _validate_bundle_evidence(
        x86_bundle_evidence,
        platform="linux-x86_64",
        version=version,
        source_revision=source_revision,
    )
    arm64 = _validate_bundle_evidence(
        arm64_bundle_evidence,
        platform="linux-arm64",
        version=version,
        source_revision=source_revision,
    )
    t04 = _validate_t04_evidence(arm64_t04_evidence, source_revision)
    output_root.parent.mkdir(parents=True, exist_ok=True)
    temporary = pathlib.Path(tempfile.mkdtemp(prefix=".local-release-", dir=output_root.parent))
    try:
        assets = []
        for record in (x86, arm64):
            target = temporary / record["package_name"]
            shutil.copy2(record["package"], target)
            assets.append(
                {
                    "name": target.name,
                    "bytes": target.stat().st_size,
                    "sha256": _sha256(target),
                    "target_platform": (
                        "linux-x86_64" if "x86_64" in target.name else "linux-arm64"
                    ),
                    "bundle_run": record["run_id"],
                }
            )
        release_index = {
            "schema_version": "p3-s7-local-release-index-v1",
            "version": version,
            "source_revision": source_revision,
            "release_status": "LOCAL_RELEASE_READY",
            "published": False,
            "tag": None,
            "hardware_validated": False,
            "x86_64_validated": True,
            "arm64_native_build_validated": True,
            "arm64_systemd_validated": True,
            "arm64_long_soak_validated": True,
            "arm64_release_bundle_ready": True,
            "license": {
                "project_code": "MIT",
                "copyright": "Copyright (c) 2026 fengchechecheche",
                "third_party": "See THIRD_PARTY_NOTICES.md inside each package",
            },
            "evidence": {"arm64_t04_run": t04["run_id"]},
            "assets": assets,
        }
        index_path = temporary / "release_index.json"
        index_path.write_text(
            json.dumps(release_index, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        notes_path = temporary / "RELEASE_NOTES.md"
        notes_path.write_text(_release_notes(source_revision), encoding="utf-8")
        checksum_inputs = [temporary / asset["name"] for asset in assets] + [index_path, notes_path]
        checksum_lines = [f"{_sha256(path)}  {path.name}" for path in checksum_inputs]
        (temporary / "SHA256SUMS").write_text(
            "\n".join(checksum_lines) + "\n", encoding="utf-8"
        )
        if {path.name for path in temporary.iterdir()} != {
            asset["name"] for asset in assets
        } | {"release_index.json", "RELEASE_NOTES.md", "SHA256SUMS"}:
            raise LocalReleaseInputError("local release contains unexpected files")
        temporary.replace(output_root)
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise
    return output_root


def parse_arguments(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Assemble the local dual-architecture release")
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--version", default=FROZEN_VERSION)
    parser.add_argument("--x86-bundle-evidence", type=pathlib.Path, required=True)
    parser.add_argument("--arm64-bundle-evidence", type=pathlib.Path, required=True)
    parser.add_argument("--arm64-t04-evidence", type=pathlib.Path, required=True)
    parser.add_argument("--output-root", type=pathlib.Path, required=True)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parse_arguments(argv)
    try:
        output = assemble_local_release(
            source_revision=arguments.source_revision,
            version=arguments.version,
            x86_bundle_evidence=arguments.x86_bundle_evidence,
            arm64_bundle_evidence=arguments.arm64_bundle_evidence,
            arm64_t04_evidence=arguments.arm64_t04_evidence,
            output_root=arguments.output_root,
        )
    except LocalReleaseInputError as error:
        print(f"LOCAL_DUAL_ARCH_RELEASE_ERROR={error}")
        return 4
    print(f"LOCAL_DUAL_ARCH_RELEASE=PASS OUTPUT={output}")
    print(T06_PASS_MARKER)
    print("PUBLISHED=false")
    print("HARDWARE_VALIDATED=false")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

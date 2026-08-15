from __future__ import annotations

import hashlib
import json
import os
import pathlib
import re
import stat
from collections.abc import Iterable, Mapping
from typing import Any


SOURCE_REVISION_PATTERN = re.compile(r"^[0-9a-f]{40}$")
REQUIRED_PACKAGE_PATHS = (
    "usr/local/bin/gateway_app",
    "usr/local/lib/systemd/system/industrial_iot_gateway.service",
    "usr/local/share/industrial_iot_gateway/systemd/gateway.env.example",
    "usr/local/share/industrial_iot_gateway/config/register_map.yaml",
    "usr/local/share/industrial_iot_gateway/config/pty_slave_scenarios.yaml",
    "usr/local/share/doc/industrial_iot_gateway/LICENSE",
    "usr/local/share/doc/industrial_iot_gateway/THIRD_PARTY_NOTICES.md",
    "usr/local/share/doc/industrial_iot_gateway/README.md",
    "usr/local/share/doc/industrial_iot_gateway/runbook.md",
)
PRIVATE_PATH_PATTERNS = (
    re.compile(r"/home/[^/\s]+"),
    re.compile(r"[A-Za-z]:\\\\Users\\\\[^\\\\\s]+"),
)
CREDENTIAL_PATTERN = re.compile(
    r"(?i)\b(password|passwd|token|api[_-]?key|secret)\s*[:=]\s*(\S+)"
)
PRIVATE_KEY_PATTERN = re.compile(r"-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----")
V2_PLATFORM_ARCHITECTURES = {
    "linux-x86_64": "x86_64",
    "linux-arm64": "arm64",
}
V2_RELEASE_STATUSES = frozenset({"candidate", "final"})
V2_REQUIRED_CAPABILITIES = frozenset(
    {
        "x86_64_validated",
        "arm64_native_build_validated",
        "arm64_systemd_validated",
        "arm64_long_soak_validated",
        "arm64_release_bundle_ready",
        "hardware_validated",
    }
)


class ManifestError(ValueError):
    """Raised when a release package cannot satisfy the frozen manifest contract."""


def validate_source_revision(value: str) -> str:
    if SOURCE_REVISION_PATTERN.fullmatch(value) is None:
        raise ManifestError("source revision must be a full lowercase 40-character SHA-1")
    return value


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _relative_path(path: pathlib.Path, root: pathlib.Path) -> str:
    try:
        relative = path.relative_to(root)
    except ValueError as error:
        raise ManifestError(f"path is outside package root: {path}") from error
    if relative.is_absolute() or ".." in relative.parts:
        raise ManifestError(f"unsafe package path: {relative}")
    return relative.as_posix()


def _symlink_entry(path: pathlib.Path, relative: str, root: pathlib.Path) -> dict[str, Any]:
    target = os.readlink(path)
    resolved = (path.parent / target).resolve()
    try:
        resolved.relative_to(root.resolve())
    except ValueError as error:
        raise ManifestError(f"symlink escapes package root: {relative}") from error
    target_bytes = target.encode("utf-8")
    return {
        "path": relative,
        "type": "symlink",
        "mode": f"0{stat.S_IMODE(path.lstat().st_mode):03o}",
        "bytes": len(target_bytes),
        "sha256": hashlib.sha256(target_bytes).hexdigest(),
        "target": target,
    }


def _file_entry(path: pathlib.Path, relative: str) -> dict[str, Any]:
    file_stat = path.stat()
    return {
        "path": relative,
        "type": "file",
        "mode": f"0{stat.S_IMODE(file_stat.st_mode):03o}",
        "bytes": file_stat.st_size,
        "sha256": sha256_file(path),
    }


def _collect_entries(package_root: pathlib.Path) -> list[dict[str, Any]]:
    root = package_root.resolve()
    if not root.is_dir():
        raise ManifestError(f"package root is not a directory: {package_root}")

    entries: list[dict[str, Any]] = []
    for path in sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix()):
        relative = _relative_path(path, root)
        if path.is_symlink():
            entries.append(_symlink_entry(path, relative, root))
        elif path.is_file():
            entries.append(_file_entry(path, relative))

    included = {entry["path"] for entry in entries}
    missing = sorted(set(REQUIRED_PACKAGE_PATHS) - included)
    if missing:
        raise ManifestError("missing required package files: " + ", ".join(missing))
    return entries


def build_manifest(
    package_root: pathlib.Path,
    *,
    release_name: str,
    source_revision: str,
    environment: Mapping[str, Any],
    gates: Mapping[str, Any],
) -> dict[str, Any]:
    validate_source_revision(source_revision)
    entries = _collect_entries(package_root)

    return {
        "schema_version": "p3-s7-release-manifest-v1",
        "release_name": release_name,
        "release_status": "CANDIDATE",
        "source_revision": source_revision,
        "environment": dict(environment),
        "gates": dict(gates),
        "hardware_validated": False,
        "arm64_validated": False,
        "files": entries,
    }


def build_manifest_v2(
    package_root: pathlib.Path,
    *,
    release_name: str,
    source_revision: str,
    environment: Mapping[str, Any],
    gates: Mapping[str, Any],
    target_platform: str,
    machine: str,
    package_architecture: str,
    release_status: str,
    capabilities: Mapping[str, Any],
) -> dict[str, Any]:
    """Build the platform-aware manifest without changing the frozen v1 contract."""

    validate_source_revision(source_revision)
    expected_architecture = V2_PLATFORM_ARCHITECTURES.get(target_platform)
    if expected_architecture is None:
        raise ManifestError(f"unsupported target platform: {target_platform}")
    if package_architecture != expected_architecture:
        raise ManifestError(
            "package architecture does not match target platform: "
            f"expected {expected_architecture}, got {package_architecture}"
        )
    if release_status not in V2_RELEASE_STATUSES:
        raise ManifestError(f"unsupported release status: {release_status}")
    missing_capabilities = sorted(V2_REQUIRED_CAPABILITIES - set(capabilities))
    if missing_capabilities:
        raise ManifestError(
            "missing required capability fields: " + ", ".join(missing_capabilities)
        )
    non_boolean = sorted(
        name for name in V2_REQUIRED_CAPABILITIES if not isinstance(capabilities[name], bool)
    )
    if non_boolean:
        raise ManifestError("capability fields must be boolean: " + ", ".join(non_boolean))
    if capabilities["hardware_validated"]:
        raise ManifestError("hardware_validated must remain false before physical G6")

    entries = _collect_entries(package_root)
    return {
        "schema_version": "p3-s7-release-manifest-v2",
        "release_name": release_name,
        "release_status": release_status,
        "source_revision": source_revision,
        "target_platform": target_platform,
        "machine": machine,
        "package_architecture": package_architecture,
        "environment": dict(environment),
        "gates": dict(gates),
        "capabilities": dict(capabilities),
        "hardware_validated": False,
        "published": False,
        "tag": None,
        "files": entries,
    }


def write_manifest(manifest: Mapping[str, Any], output: pathlib.Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".tmp")
    temporary.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    temporary.replace(output)


def write_sha256sums(
    paths: Iterable[pathlib.Path], *, root: pathlib.Path, output: pathlib.Path
) -> None:
    resolved_root = root.resolve()
    records: list[tuple[str, pathlib.Path]] = []
    for path in paths:
        resolved = path.resolve()
        relative = _relative_path(resolved, resolved_root)
        if not resolved.is_file():
            raise ManifestError(f"checksum input is not a file: {path}")
        records.append((relative, resolved))
    records.sort(key=lambda item: item[0])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        "".join(f"{sha256_file(path)}  {relative}\n" for relative, path in records),
        encoding="utf-8",
    )


def scan_release_tree(root: pathlib.Path) -> list[dict[str, str]]:
    resolved_root = root.resolve()
    if not resolved_root.is_dir():
        raise ManifestError(f"release scan root is not a directory: {root}")
    findings: list[dict[str, str]] = []
    for path in sorted(resolved_root.rglob("*")):
        if path.is_symlink() or not path.is_file():
            continue
        relative = _relative_path(path, resolved_root)
        if path.suffix.lower() in {".key", ".pem", ".p12", ".pfx"}:
            findings.append(
                {"path": relative, "category": "credential", "excerpt": "sensitive file type"}
            )
            continue
        if path.stat().st_size > 2 * 1024 * 1024:
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for line_number, line in enumerate(text.splitlines(), start=1):
            credential = CREDENTIAL_PATTERN.search(line)
            if credential is not None and credential.group(2).upper() not in {
                "REPLACE_ME",
                "CHANGEME",
                "<REDACTED>",
            }:
                findings.append(
                    {
                        "path": relative,
                        "category": "credential",
                        "excerpt": f"line {line_number}: {credential.group(1)}=<redacted>",
                    }
                )
            if PRIVATE_KEY_PATTERN.search(line) is not None:
                findings.append(
                    {
                        "path": relative,
                        "category": "credential",
                        "excerpt": f"line {line_number}: private key marker",
                    }
                )
            if any(pattern.search(line) is not None for pattern in PRIVATE_PATH_PATTERNS):
                findings.append(
                    {
                        "path": relative,
                        "category": "private_path",
                        "excerpt": f"line {line_number}: private path redacted",
                    }
                )
    return findings

#!/usr/bin/env python3
"""Check the extracted contents of the Ubuntu .deb against its sidecar inventory."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from package_notices import verify_notices
from package_sbom import verify_sbom

SIDECAR_STATUS = "oscillate .deb inventory"
DESKTOP_ENTRY = "usr/share/applications/oscillate.desktop"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_qml_runtime_dependencies(control: str, inventory: dict) -> None:
    if not inventory["qml_source_files"] or not inventory["qml_source_imports"]:
        raise ValueError("bundled QML import inventory is empty")
    if not inventory["qml_runtime_packages"]:
        raise ValueError("QML runtime package inventory is empty")
    packages = {dependency.split(" ", 1)[0] for dependency in control.split(", ")}
    for name in inventory["qml_runtime_packages"]:
        if name not in packages:
            raise ValueError(f"QML runtime dependency missing: {name}")


def smoke(package: Path, inventory_path: Path) -> None:
    inventory = json.loads(inventory_path.read_text())
    if inventory.get("status") != SIDECAR_STATUS:
        raise ValueError("not an oscillate .deb inventory")
    if sha256(package) != inventory["package_sha256"]:
        raise ValueError("package hash differs from sidecar")
    control = subprocess.run(
        ["dpkg-deb", "--field", str(package), "Depends"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    if control != inventory["depends"]:
        raise ValueError("package Depends differs from sidecar")
    check_qml_runtime_dependencies(control, inventory)
    with tempfile.TemporaryDirectory(prefix="oscillate-deb-extract-") as temporary:
        root = Path(temporary)
        subprocess.run(["dpkg-deb", "--extract", str(package), str(root)], check=True)
        expected = set()
        for item in inventory["files"]:
            relative = Path(item["path"].lstrip("/"))
            if relative.is_absolute() or ".." in relative.parts:
                raise ValueError(f"unsafe package path: {relative}")
            expected.add(str(relative))
            staged = root / relative
            if not staged.is_file() or sha256(staged) != item["sha256"]:
                raise ValueError(f"package payload mismatch: {relative}")
        actual = {str(path.relative_to(root)) for path in root.rglob("*") if path.is_file()}
        if actual != expected:
            raise ValueError("package file set differs from sidecar")
        notices = verify_notices(root, expected_copied_elfs=set(), scanner_enabled=True)
        if notices["system_dependencies"] != inventory["depends"]:
            raise ValueError("system dependency inventory differs from package sidecar")
        if any(path.startswith("usr/lib/") for path in actual):
            raise ValueError("native package unexpectedly bundles a shared library")
        sbom = verify_sbom(root)
        if sbom["packages"][0]["versionInfo"] != inventory["version"]:
            raise ValueError("SBOM version differs from the package")
        subprocess.run(["desktop-file-validate", str(root / DESKTOP_ENTRY)], check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    args = parser.parse_args()
    smoke(args.package.resolve(strict=True), args.inventory.resolve(strict=True))
    print("Extracted .deb payload, notices, SBOM and desktop entry passed")


if __name__ == "__main__":
    main()

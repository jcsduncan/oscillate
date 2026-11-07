#!/usr/bin/env python3
"""Build a local-only Ubuntu .deb from an already-built Qt Quick executable.

This prototype depends on Ubuntu's shared libraries. It does not vendor Qt,
codecs, plug-ins, or QML modules. An explicitly supplied VST3 scanner helper
is staged beside the app. This is not a release packaging decision.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

from package_notices import stage_notices, verify_notices

PACKAGE = "oscillate-quick-prototype"
VERSION = "0.1.0~prototype1"
PACKAGE_FEATURES = {
    "BUILD_TESTING": "OFF",
    "CMAKE_BUILD_TYPE": "Release",
    "CMAKE_SKIP_BUILD_RPATH": "ON",
    "OSCILLATE_BUILD_QUICK_UI": "ON",
    "OSCILLATE_BUILD_VST3_SCANNER": "ON",
    "OSCILLATE_BUILD_CODEC_ADAPTER": "ON",
    "OSCILLATE_BUILD_EXPORT_RESAMPLER": "ON",
    "OSCILLATE_BUILD_SOURCE_RESAMPLER": "ON",
    "OSCILLATE_BUILD_CONVOLUTION_DSP": "OFF",
}
QML_RUNTIME_PACKAGES = {"QtQuick": "qml6-module-qtquick"}
# Modules the app registers itself (qmlRegister* in apps/daw_quick/src): they
# live in the binary, so no runtime package provides them.
APP_QML_MODULES = frozenset({"Oscillate.Contracts"})


def run(*args: str, cwd: Path | None = None) -> str:
    result = subprocess.run(args, cwd=cwd, check=True, capture_output=True, text=True)
    return result.stdout


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def inspect_elf(path: Path) -> tuple[list[str], str]:
    elf_header = run("readelf", "-h", str(path))
    if "Machine:                           Advanced Micro Devices X86-64" not in elf_header:
        raise SystemExit(f"Input is not an x86-64 ELF executable: {path}")
    dynamic = run("readelf", "-d", str(path))
    if "(RPATH)" in dynamic or "(RUNPATH)" in dynamic:
        raise SystemExit(f"Refusing an executable with RPATH/RUNPATH: {path}")
    needed = re.findall(r"\(NEEDED\).*\[(.+?)\]", dynamic)
    if not needed or "libQt6Test.so.6" in needed:
        raise SystemExit(f"Missing dependencies or linked Qt Test: {path}")
    resolved = run("ldd", str(path))
    if "not found" in resolved:
        raise SystemExit(f"A required shared library is missing on this host: {path}")
    return needed, resolved


def check_package_profile(cache_path: Path, binary: Path, helper: Path | None) -> dict[str, str]:
    """Bind the package inputs to one configured, feature-complete CMake build."""
    cache_path = cache_path.resolve(strict=True)
    cache: dict[str, str] = {}
    for line in cache_path.read_text().splitlines():
        if line.startswith(("#", "//")) or "=" not in line:
            continue
        name_and_type, value = line.split("=", 1)
        name = name_and_type.split(":", 1)[0]
        cache[name] = value
    for name, expected in PACKAGE_FEATURES.items():
        if cache.get(name) != expected:
            raise SystemExit(f"Package profile requires {name}={expected}; got {cache.get(name)!r}")
    source_root = Path(__file__).resolve().parent.parent
    if Path(cache.get("CMAKE_HOME_DIRECTORY", "/nonexistent")).resolve() != source_root:
        raise SystemExit("Package profile CMake source differs from packaging source")
    expected_bin = cache_path.parent / "apps/daw_quick/oscillate"
    expected_helper = cache_path.parent / "apps/daw_quick/oscillate_vst3_scan_helper"
    if (
        binary.resolve() != expected_bin.resolve()
        or helper is None
        or helper.resolve() != expected_helper.resolve()
    ):
        raise SystemExit("Package profile requires app and scanner helper from its own build")
    return {name: cache[name] for name in sorted(PACKAGE_FEATURES)}


def bundled_qml_imports(source_root: Path) -> tuple[list[Path], list[str]]:
    """Read only QML files embedded by the app's resource manifest."""
    manifest = source_root / "apps/daw_quick/qml.qrc"
    files = sorted(
        {
            (manifest.parent / item.text).resolve(strict=True)
            for item in ET.parse(manifest).iter("file")
            if item.text is not None and item.text.endswith(".qml")
        }
    )
    if not files:
        raise SystemExit("Quick package has no bundled QML files")
    imports = sorted(
        {
            match.group(1)
            for qml in files
            for line in qml.read_text().splitlines()
            if (match := re.match(r"^\s*import\s+([A-Za-z][A-Za-z0-9.]*)\b", line))
        }
        - APP_QML_MODULES
    )
    if not imports:
        raise SystemExit("Quick package has no discoverable QML module imports")
    unsupported = sorted(set(imports) - QML_RUNTIME_PACKAGES.keys())
    if unsupported:
        raise SystemExit(f"Unmapped QML runtime imports: {', '.join(unsupported)}")
    return files, imports


def complete_dependencies(shlib_dependencies: str, imports: list[str]) -> str:
    """dpkg-shlibdeps does not see QML modules loaded by the QML engine."""
    packages = sorted({QML_RUNTIME_PACKAGES[name] for name in imports})
    return ", ".join([shlib_dependencies, *packages])


def package(
    binary: Path, output: Path, scanner_helper: Path | None = None, cmake_cache: Path | None = None
) -> tuple[Path, Path]:
    if platform.machine() != "x86_64":
        raise SystemExit("This prototype requires an x86-64 host")
    os_release = Path("/etc/os-release").read_text()
    if 'VERSION_ID="26.04"' not in os_release:
        raise SystemExit("This prototype requires Ubuntu 26.04")
    binary = binary.resolve(strict=True)
    source_root = Path(__file__).resolve().parent.parent
    qml_files, imports = bundled_qml_imports(source_root)
    feature_profile = (
        check_package_profile(cmake_cache, binary, scanner_helper)
        if cmake_cache is not None
        else None
    )
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    needed, resolved = inspect_elf(binary)
    if "libQt6Quick.so.6" not in needed:
        raise SystemExit("Input does not look like the dynamically linked Qt Quick executable")
    helper_needed: list[str] = []
    helper_resolved = ""
    if scanner_helper is not None:
        scanner_helper = scanner_helper.resolve(strict=True)
        if scanner_helper.name != "oscillate_vst3_scan_helper":
            raise SystemExit("VST3 helper must be named oscillate_vst3_scan_helper")
        if scanner_helper.parent != binary.parent:
            raise SystemExit("VST3 helper must be the Quick executable's build sibling")
        helper_needed, helper_resolved = inspect_elf(scanner_helper)

    with tempfile.TemporaryDirectory(prefix="oscillate-deb-") as temporary:
        work = Path(temporary)
        stage = work / "stage"
        executable = stage / "usr/bin/oscillate"
        executable.parent.mkdir(parents=True)
        shutil.copy2(binary, executable)
        executable.chmod(0o755)
        staged_helper = None
        if scanner_helper is not None:
            staged_helper = executable.parent / scanner_helper.name
            shutil.copy2(scanner_helper, staged_helper)
            staged_helper.chmod(0o755)

        desktop = stage / "usr/share/applications/oscillate-quick-prototype.desktop"
        desktop.parent.mkdir(parents=True)
        desktop.write_text(
            "[Desktop Entry]\nType=Application\nName=Oscillate (prototype)\n"
            "Exec=oscillate\nTerminal=false\nCategories=AudioVideo;Audio;\n"
        )
        note = stage / f"usr/share/doc/{PACKAGE}/README.prototype"
        note.parent.mkdir(parents=True)
        note.write_text(
            "Local Ubuntu 26.04 packaging prototype only. Not for distribution.\n"
            "Third-party notice evidence is in the oscillate/third-party directory.\n"
            "Product code and starter-sound grants, complete licence review and LGPL\n"
            "replacement/source obligations are unresolved. Qt, codec and audio\n"
            "libraries come from system packages; no library is bundled here.\n"
        )

        # dpkg-shlibdeps reads debian/control from its working directory and
        # resolves system package shlibs metadata from the actual ELF closure.
        debian = work / "debian"
        debian.mkdir()
        (debian / "control").write_text(
            f"Source: {PACKAGE}\nSection: sound\nPriority: optional\n"
            "Maintainer: Chris Duncan <jcsduncan2011@gmail.com>\n"
            "Standards-Version: 4.7.0\n\n"
            f"Package: {PACKAGE}\nArchitecture: amd64\n"
            "Description: local Oscillate packaging prototype\n"
        )
        elf_inputs = [f"-e{executable}"]
        if staged_helper is not None:
            elf_inputs.append(f"-e{staged_helper}")
        dependency_output = run("dpkg-shlibdeps", "-O", *elf_inputs, cwd=work)
        match = re.search(r"^shlibs:Depends=(.+)$", dependency_output, re.MULTILINE)
        if match is None:
            raise SystemExit("dpkg-shlibdeps did not produce dependency metadata")
        depends = complete_dependencies(match.group(1), imports)
        stage_notices(
            stage,
            Path(__file__).resolve().parent.parent,
            scanner_enabled=staged_helper is not None,
            system_dependencies=depends,
        )
        verify_notices(stage, expected_copied_elfs=set(), scanner_enabled=staged_helper is not None)
        control = stage / "DEBIAN/control"
        control.parent.mkdir()
        control.write_text(
            f"Package: {PACKAGE}\nVersion: {VERSION}\nSection: sound\n"
            "Priority: optional\nArchitecture: amd64\n"
            "Maintainer: Chris Duncan <jcsduncan2011@gmail.com>\n"
            f"Depends: {depends}\n"
            "Description: Oscillate Quick local packaging prototype\n"
            " Local-only package evidence; no distribution licence approval.\n"
        )
        epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
        for path in stage.rglob("*"):
            path.chmod(0o755 if path.is_dir() or path in (executable, staged_helper) else 0o644)
            os.utime(path, (epoch, epoch))
        os.utime(stage, (epoch, epoch))
        os.environ.setdefault("SOURCE_DATE_EPOCH", str(epoch))
        artifact = output / f"{PACKAGE}_{VERSION}_amd64.deb"
        run("dpkg-deb", "--build", "--root-owner-group", str(stage), str(artifact))
        quick_source = source_root / "apps/daw_quick"
        resource_paths = [
            (quick_source / item.text).resolve(strict=True)
            for item in ET.parse(quick_source / "qml.qrc").iter("file")
            if item.text is not None
        ]
        payload = {
            "status": "local prototype; not for distribution",
            "package": artifact.name,
            "package_sha256": digest(artifact),
            "binary_sha256": digest(executable),
            "scanner_helper_sha256": digest(staged_helper) if staged_helper else None,
            "third_party_inventory": "/usr/share/doc/oscillate/third-party/inventory.json",
            "cmake_feature_profile": feature_profile,
            "depends": depends,
            "direct_dt_needed": needed,
            "scanner_helper_dt_needed": helper_needed,
            "files": [
                {"path": "/" + str(path.relative_to(stage)), "sha256": digest(path)}
                for path in sorted((stage / "usr").rglob("*"))
                if path.is_file()
            ],
            "runtime_ldd": [
                re.sub(r"\s+\(0x[0-9a-f]+\)$", "", line) for line in resolved.splitlines()
            ],
            "scanner_helper_ldd": [
                re.sub(r"\s+\(0x[0-9a-f]+\)$", "", line) for line in helper_resolved.splitlines()
            ],
            "qml_source_imports": imports,
            "qml_runtime_packages": sorted({QML_RUNTIME_PACKAGES[name] for name in imports}),
            "qml_source_files": [
                {"path": str(path.relative_to(source_root)), "sha256": digest(path)}
                for path in qml_files
            ],
            "qrc_source_files": [
                {"path": str(path.relative_to(source_root)), "sha256": digest(path)}
                for path in resource_paths
            ],
            "qml_note": "Source inventory only; inspect QML runtime trace for artifact closure",
        }
        inventory = output / f"{artifact.name}.inventory.json"
        inventory.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
        return artifact, inventory


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument(
        "--scanner-helper",
        type=Path,
        help="paired scanner executable from the same VST3-enabled Quick build",
    )
    parser.add_argument(
        "--cmake-cache",
        type=Path,
        help="enforce the complete local package feature profile from this CMake build",
    )
    args = parser.parse_args()
    try:
        artifact, inventory = package(
            args.binary, args.output_dir, args.scanner_helper, args.cmake_cache
        )
    except subprocess.CalledProcessError as error:
        raise SystemExit(f"{error.cmd} failed: {error.stderr}") from error
    print(artifact)
    print(inventory)


if __name__ == "__main__":
    main()

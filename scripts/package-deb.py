#!/usr/bin/env python3
"""Build the Ubuntu 26.04 .deb from a Release build configured with the deb profile.

The package depends on Ubuntu's shared libraries and bundles none: Qt, codecs,
PipeWire and JACK come from system packages, which dpkg-shlibdeps names. It
ships the app, its VST3 scanner helper, a desktop entry and icon, the feature
list, the third-party notices and an SPDX SBOM. A JSON sidecar beside the .deb
records its hash, dependencies and payload for the release and smoke checks.
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
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

# Run as `python3 -I`, which leaves the script's directory off the path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from package_notices import stage_notices, verify_notices
from package_profiles import PROFILES, SOURCE_ROOT, check_build, feature_readme
from package_sbom import installed_version, verify_sbom, write_sbom

PACKAGE = "oscillate"
MAINTAINER = "Chris Duncan <jcsduncan2011@gmail.com>"
HOMEPAGE = "https://github.com/jcsduncan/oscillate"
DOC_DIR = Path(f"usr/share/doc/{PACKAGE}")
SIDECAR_STATUS = "oscillate .deb inventory"
QML_RUNTIME_PACKAGES = {"QtQuick": "qml6-module-qtquick"}
# Modules the app registers itself (qmlRegister* in apps/daw_quick/src): they
# live in the binary, so no runtime package provides them.
APP_QML_MODULES = frozenset({"Oscillate.Contracts", "Oscillate.Format"})
READ_CHUNK_BYTES = 1 << 20
EXECUTABLE_MODE = 0o755
FILE_MODE = 0o644
X86_64_MACHINE = "Machine:                           Advanced Micro Devices X86-64"
COPYRIGHT = """\
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Oscillate
Source: https://github.com/jcsduncan/oscillate

Files: *
Copyright: Oscillate contributors
License: GPL-3+
 On Ubuntu systems, the complete text of the GNU General Public License
 version 3 can be found in /usr/share/common-licenses/GPL-3.
 .
 Third-party code compiled into the executables keeps its own terms; see
 /usr/share/doc/oscillate/third-party/inventory.json.
"""


def run(*args: str, cwd: Path | None = None) -> str:
    result = subprocess.run(args, cwd=cwd, check=True, capture_output=True, text=True)
    return result.stdout


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(READ_CHUNK_BYTES), b""):
            h.update(chunk)
    return h.hexdigest()


def inspect_elf(path: Path) -> tuple[list[str], str]:
    elf_header = run("readelf", "-h", str(path))
    if X86_64_MACHINE not in elf_header:
        raise ValueError(f"Input is not an x86-64 ELF executable: {path}")
    dynamic = run("readelf", "-d", str(path))
    if "(RPATH)" in dynamic or "(RUNPATH)" in dynamic:
        raise ValueError(f"Refusing an executable with RPATH/RUNPATH: {path}")
    needed = re.findall(r"\(NEEDED\).*\[(.+?)\]", dynamic)
    if not needed or "libQt6Test.so.6" in needed:
        raise ValueError(f"Missing dependencies or linked Qt Test: {path}")
    resolved = run("ldd", str(path))
    if "not found" in resolved:
        raise ValueError(f"A required shared library is missing on this host: {path}")
    return needed, resolved


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
        raise ValueError("Quick package has no bundled QML files")
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
        raise ValueError("Quick package has no discoverable QML module imports")
    unsupported = sorted(set(imports) - QML_RUNTIME_PACKAGES.keys())
    if unsupported:
        raise ValueError(f"Unmapped QML runtime imports: {', '.join(unsupported)}")
    return files, imports


def complete_dependencies(shlib_dependencies: str, imports: list[str]) -> str:
    """dpkg-shlibdeps does not see QML modules loaded by the QML engine."""
    packages = sorted({QML_RUNTIME_PACKAGES[name] for name in imports})
    return ", ".join([shlib_dependencies, *packages])


def install_file(source: Path, destination: Path, mode: int) -> Path:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    destination.chmod(mode)
    return destination


def write_file(destination: Path, text: str) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(text)
    destination.chmod(FILE_MODE)


def strip_ldd_addresses(output: str) -> list[str]:
    return [re.sub(r"\s+\(0x[0-9a-f]+\)$", "", line) for line in output.splitlines()]


def package(
    build_dir: Path,
    output: Path,
    *,
    release_tag: str | None = None,
    version_suffix: str | None = None,
) -> tuple[Path, Path]:
    if platform.machine() != "x86_64":
        raise ValueError("The .deb requires an x86-64 host")
    if 'VERSION_ID="26.04"' not in Path("/etc/os-release").read_text():
        raise ValueError("The .deb requires Ubuntu 26.04")
    profile = PROFILES["deb"]
    inputs = check_build(build_dir, profile, release_tag=release_tag, version_suffix=version_suffix)
    qml_files, imports = bundled_qml_imports(SOURCE_ROOT)
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    needed, resolved = inspect_elf(inputs.app)
    if "libQt6Quick.so.6" not in needed:
        raise ValueError("Input does not look like the dynamically linked Qt Quick executable")
    helper_needed, helper_resolved = inspect_elf(inputs.helper)

    with tempfile.TemporaryDirectory(prefix="oscillate-deb-") as temporary:
        work = Path(temporary)
        stage = work / "stage"
        executable = install_file(inputs.app, stage / "usr/bin/oscillate", EXECUTABLE_MODE)
        helper = install_file(
            inputs.helper, stage / "usr/bin" / inputs.helper.name, EXECUTABLE_MODE
        )
        assets = SOURCE_ROOT / "assets/packaging"
        install_file(
            assets / "oscillate.desktop",
            stage / "usr/share/applications/oscillate.desktop",
            FILE_MODE,
        )
        install_file(
            assets / "oscillate.svg",
            stage / "usr/share/icons/hicolor/scalable/apps/oscillate.svg",
            FILE_MODE,
        )
        write_file(stage / DOC_DIR / "README", feature_readme(profile, inputs.version))
        write_file(stage / DOC_DIR / "copyright", COPYRIGHT)

        # dpkg-shlibdeps reads debian/control from its working directory and
        # resolves system package shlibs metadata from the actual ELF closure.
        debian = work / "debian"
        debian.mkdir()
        (debian / "control").write_text(
            f"Source: {PACKAGE}\nSection: sound\nPriority: optional\n"
            f"Maintainer: {MAINTAINER}\nStandards-Version: 4.7.0\n\n"
            f"Package: {PACKAGE}\nArchitecture: amd64\n"
            "Description: Oscillate digital audio workstation\n"
        )
        dependency_output = run("dpkg-shlibdeps", "-O", f"-e{executable}", f"-e{helper}", cwd=work)
        match = re.search(r"^shlibs:Depends=(.+)$", dependency_output, re.MULTILINE)
        if match is None:
            raise ValueError("dpkg-shlibdeps did not produce dependency metadata")
        depends = complete_dependencies(match.group(1), imports)
        notices = stage_notices(
            stage, SOURCE_ROOT, scanner_enabled=True, system_dependencies=depends
        )
        verify_notices(stage, expected_copied_elfs=set(), scanner_enabled=True)
        control = stage / "DEBIAN/control"
        control.parent.mkdir()
        control.write_text(
            f"Package: {PACKAGE}\nVersion: {inputs.version}\nSection: sound\n"
            "Priority: optional\nArchitecture: amd64\n"
            f"Maintainer: {MAINTAINER}\n"
            f"Depends: {depends}\n"
            f"Homepage: {HOMEPAGE}\n"
            "Description: digital audio workstation\n"
            " Arrange, play and mix audio and MIDI with built-in instruments and\n"
            " effects, Linux VST3 plug-ins, PipeWire and JACK output.\n"
        )
        epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
        write_sbom(
            stage,
            package_name=PACKAGE,
            version=inputs.version,
            form="deb",
            notice_inventory=notices,
            nlohmann_version=installed_version("nlohmann-json3-dev"),
            creation_epoch=epoch,
            depends=depends,
        )
        verify_sbom(stage)
        for path in stage.rglob("*"):
            path.chmod(
                EXECUTABLE_MODE if path.is_dir() or path in (executable, helper) else FILE_MODE
            )
            os.utime(path, (epoch, epoch))
        os.utime(stage, (epoch, epoch))
        os.environ.setdefault("SOURCE_DATE_EPOCH", str(epoch))
        artifact = output / f"{PACKAGE}_{inputs.version}_amd64.deb"
        run("dpkg-deb", "--build", "--root-owner-group", str(stage), str(artifact))
        payload = {
            "status": SIDECAR_STATUS,
            "package": artifact.name,
            "version": inputs.version,
            "package_sha256": digest(artifact),
            "binary_sha256": digest(executable),
            "scanner_helper_sha256": digest(helper),
            "third_party_inventory": f"/{DOC_DIR}/third-party/inventory.json",
            "sbom": f"/{DOC_DIR}/sbom.spdx.json",
            "cmake_feature_profile": inputs.switches,
            "features": profile.included_features(),
            "excluded_features": profile.excluded_features(),
            "depends": depends,
            "direct_dt_needed": needed,
            "scanner_helper_dt_needed": helper_needed,
            "files": [
                {"path": "/" + str(path.relative_to(stage)), "sha256": digest(path)}
                for path in sorted((stage / "usr").rglob("*"))
                if path.is_file()
            ],
            "runtime_ldd": strip_ldd_addresses(resolved),
            "scanner_helper_ldd": strip_ldd_addresses(helper_resolved),
            "qml_source_imports": imports,
            "qml_runtime_packages": sorted({QML_RUNTIME_PACKAGES[name] for name in imports}),
            "qml_source_files": [
                {"path": str(path.relative_to(SOURCE_ROOT)), "sha256": digest(path)}
                for path in qml_files
            ],
        }
        inventory = output / f"{artifact.name}.inventory.json"
        inventory.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
        return artifact, inventory


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir", type=Path, required=True, help="Release tree from the package-deb preset"
    )
    parser.add_argument("--output-dir", type=Path, required=True)
    version = parser.add_mutually_exclusive_group(required=True)
    version.add_argument("--release-tag", help="release tag; must be v<project version>")
    version.add_argument("--version-suffix", help="pre-release suffix, as in 0.1.0~<suffix>")
    args = parser.parse_args()
    try:
        artifact, inventory = package(
            args.build_dir,
            args.output_dir,
            release_tag=args.release_tag,
            version_suffix=args.version_suffix,
        )
    except subprocess.CalledProcessError as error:
        parser.exit(1, f"{error.cmd} failed: {error.stderr}\n")
    except (ValueError, OSError) as error:
        parser.exit(1, f"{error}\n")
    print(artifact)
    print(inventory)


if __name__ == "__main__":
    main()

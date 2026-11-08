#!/usr/bin/env python3
"""Stage the AppImage's AppDir from a Release build configured with the appimage profile.

The AppDir carries the app, its VST3 scanner helper, selected Qt platform
plugins and their observed Ubuntu shared-library closure, minus the glibc base
ABI and host-integration libraries listed below. Every copied ELF is mapped to
its Ubuntu package, copyright file and exact source package, the written
source offer links each source, and an SPDX SBOM lists every file. A JSON
sidecar records the payload tree for scripts/build-appimage.py.
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
from pathlib import Path

# Run as `python3 -I`, which leaves the script's directory off the path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from package_notices import stage_notices, verify_notices
from package_profiles import (
    PROFILES,
    SOURCE_ROOT,
    TYPE2_RUNTIME,
    check_build,
    feature_readme,
)
from package_sbom import installed_version, verify_sbom, write_sbom
from package_sources import build_offer, verify_offer, write_offer

INVENTORY_STATUS = "oscillate AppDir inventory"
PLUGINS = (
    "platforms/libqoffscreen.so",
    "platforms/libqxcb.so",
    "platforms/libqwayland.so",
    "platforminputcontexts/libibusplatforminputcontextplugin.so",
)

# Keep the loader and glibc ABI from the target Ubuntu 26.04 system. All other
# observed dependencies are copied.
HOST_BASE_NAMES = {
    "ld-linux-x86-64.so.2",
    "libc.so.6",
    "libdl.so.2",
    "libm.so.6",
    "libmvec.so.1",
    "libpthread.so.0",
    "libresolv.so.2",
    "librt.so.1",
    "libutil.so.1",
}
HOST_INTEGRATION_NAMES = {
    # Ubuntu's libproxy carries an absolute RUNPATH to its backend directory.
    # Copying it would encode a host path in the AppDir and omit that backend.
    "libproxy.so.1",
    "libpxbackend-1.0.so",
}
READ_CHUNK_BYTES = 1 << 20
EXECUTABLE_MODE = 0o755
FILE_MODE = 0o644
X86_64_MACHINE = "Machine:                           Advanced Micro Devices X86-64"
DOC_DIR = Path("usr/share/doc/oscillate")


def command(*args: str) -> str:
    result = subprocess.run(args, check=True, capture_output=True, text=True)
    return result.stdout


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(READ_CHUNK_BYTES), b""):
            digest.update(chunk)
    return digest.hexdigest()


def elf_info(path: Path) -> dict[str, object]:
    header = command("readelf", "-h", str(path))
    if X86_64_MACHINE not in header:
        raise ValueError(f"not an x86-64 ELF: {path}")
    dynamic = command("readelf", "-d", str(path))
    needed = re.findall(r"\(NEEDED\).*\[(.+?)\]", dynamic)
    paths = re.findall(r"\((?:RPATH|RUNPATH)\).*\[(.*?)\]", dynamic)
    if paths:
        raise ValueError(f"RPATH/RUNPATH requires explicit review: {path}: {paths}")
    return {"needed": needed, "runtime_paths": paths}


def resolved_libraries(path: Path) -> dict[str, Path]:
    output = command("ldd", str(path))
    if "not found" in output:
        raise ValueError(f"unresolved shared library for {path}: {output}")
    found: dict[str, Path] = {}
    for line in output.splitlines():
        match = re.match(r"\s*(\S+)\s+=>\s+(/\S+)\s+\(0x[0-9a-f]+\)", line)
        if match:
            found[match.group(1)] = Path(match.group(2))
    return found


def package_for(path: Path) -> dict[str, str | None]:
    result = subprocess.run(
        ["dpkg-query", "-S", str(path)], capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        raise ValueError(f"no Ubuntu package owns observed library: {path}")
    package = result.stdout.splitlines()[0].split(": ", 1)[0]
    version, source_package, source_version = command(
        "dpkg-query", "-W", "-f=${Version}\t${source:Package}\t${source:Version}", package
    ).split("\t")
    copyright_file = Path("/usr/share/doc") / package.split(":", 1)[0] / "copyright"
    return {
        "package": package,
        "version": version,
        "source_package": source_package,
        "source_version": source_version,
        "copyright": str(copyright_file) if copyright_file.is_file() else None,
    }


def write_text(path: Path, value: str, mode: int = FILE_MODE) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value)
    path.chmod(mode)


def copy_file(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    destination.chmod(FILE_MODE)


def stage(
    build_dir: Path,
    output: Path,
    plugin_root: Path,
    *,
    release_tag: str | None = None,
    version_suffix: str | None = None,
) -> tuple[Path, Path]:
    if not output.is_relative_to(SOURCE_ROOT / "build"):
        raise ValueError("AppDir output must stay under this checkout's ignored build/")
    if (
        platform.machine() != "x86_64"
        or 'VERSION_ID="26.04"' not in Path("/etc/os-release").read_text()
    ):
        raise ValueError("the AppDir requires an Ubuntu 26.04 x86-64 host")
    profile = PROFILES["appimage"]
    inputs = check_build(build_dir, profile, release_tag=release_tag, version_suffix=version_suffix)
    if output.exists():
        raise ValueError(f"refusing to overwrite an existing AppDir: {output}")
    if not output.name.endswith(".AppDir"):
        raise ValueError("output directory must end in .AppDir")
    app_info = elf_info(inputs.app)
    elf_info(inputs.helper)
    if "libQt6Quick.so.6" not in app_info["needed"] or "libQt6Test.so.6" in app_info["needed"]:
        raise ValueError("input is not a production Quick build")
    plugins = [(name, (plugin_root / name).resolve(strict=True)) for name in PLUGINS]
    for _, plugin in plugins:
        elf_info(plugin)

    # Resolve every root separately so dlopen-only Qt platform/input plugins
    # contribute their own DT_NEEDED closure. Reject SONAME ambiguity.
    libraries: dict[str, Path] = {}
    for root in [inputs.app, inputs.helper, *(path for _, path in plugins)]:
        for soname, path in resolved_libraries(root).items():
            if soname in libraries and libraries[soname] != path:
                raise ValueError(f"multiple host versions for {soname}")
            libraries[soname] = path

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="oscillate-appdir-", dir=output.parent) as tmp:
        stage_root = Path(tmp) / output.name
        bin_dir = stage_root / "usr/bin"
        lib_dir = stage_root / "usr/lib"
        plugin_dir = stage_root / "usr/plugins"
        bin_dir.mkdir(parents=True)
        lib_dir.mkdir(parents=True)
        records: list[dict[str, object]] = []

        def copy_elf(source: Path, destination: Path, kind: str) -> None:
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
            destination.chmod(EXECUTABLE_MODE)
            records.append(
                {
                    "path": str(destination.relative_to(stage_root)),
                    "source": str(source),
                    "kind": kind,
                    "sha256": sha256(destination),
                    **elf_info(destination),
                    **(package_for(source) if kind != "project" else {}),
                }
            )

        copy_elf(inputs.app, bin_dir / inputs.app.name, "project")
        copy_elf(inputs.helper, bin_dir / inputs.helper.name, "project")
        for name, source in plugins:
            copy_elf(source, plugin_dir / name, "qt-plugin")
        external: list[dict[str, object]] = []
        for soname, source in sorted(libraries.items()):
            if soname in HOST_BASE_NAMES or soname in HOST_INTEGRATION_NAMES:
                external.append(
                    {
                        "soname": soname,
                        "host_path": str(source),
                        "reason": (
                            "base ABI" if soname in HOST_BASE_NAMES else "host proxy integration"
                        ),
                        **package_for(source),
                    }
                )
            else:
                copy_elf(source, lib_dir / soname, "shared-library")

        write_text(
            stage_root / "AppRun",
            "#!/bin/sh\nset -eu\n"
            'APPDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)\n'
            'export LD_LIBRARY_PATH="$APPDIR/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n'
            'export QT_PLUGIN_PATH="$APPDIR/usr/plugins"\n'
            'export QT_QPA_PLATFORM_PLUGIN_PATH="$APPDIR/usr/plugins/platforms"\n'
            'exec "$APPDIR/usr/bin/oscillate" "$@"\n',
            EXECUTABLE_MODE,
        )
        write_text(
            bin_dir / "qt.conf", "[Paths]\nPrefix = ..\nPlugins = plugins\nQmlImports = qml\n"
        )
        assets = SOURCE_ROOT / "assets/packaging"
        copy_file(assets / "oscillate.desktop", stage_root / "oscillate.desktop")
        copy_file(assets / "oscillate.svg", stage_root / "oscillate.svg")
        copy_file(SOURCE_ROOT / "LICENSE", stage_root / DOC_DIR / "LICENSE")
        write_text(stage_root / DOC_DIR / "README", feature_readme(profile, inputs.version))
        notice_inventory = stage_notices(
            stage_root,
            SOURCE_ROOT,
            scanner_enabled=True,
            runtime_enabled=True,
            copied_elfs=[record for record in records if record["kind"] != "project"],
        )
        verify_notices(
            stage_root,
            expected_copied_elfs={
                str(record["path"]) for record in records if record["kind"] != "project"
            },
            scanner_enabled=True,
            runtime_enabled=True,
        )
        copied = [record for record in records if record["kind"] != "project"]
        project_commit = command("git", "-C", str(SOURCE_ROOT), "rev-parse", "HEAD").strip()
        offer = build_offer(copied, version=inputs.version, project_commit=project_commit)
        write_offer(stage_root, offer)
        verify_offer(stage_root, {str(record["package"]).split(":", 1)[0] for record in copied})
        epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
        write_sbom(
            stage_root,
            package_name="oscillate",
            version=inputs.version,
            form="appimage",
            notice_inventory=notice_inventory,
            nlohmann_version=installed_version("nlohmann-json3-dev"),
            creation_epoch=epoch,
            runtime_sha256=TYPE2_RUNTIME["sha256"],
        )
        verify_sbom(stage_root)
        for path in stage_root.rglob("*"):
            os.utime(path, (epoch, epoch))
        os.utime(stage_root, (epoch, epoch))
        os.replace(stage_root, output)

    payload = []
    for path in sorted(output.rglob("*")):
        if path.is_file():
            payload.append(
                {
                    "path": str(path.relative_to(output)),
                    "sha256": sha256(path),
                    "size": path.stat().st_size,
                    "mode": oct(path.stat().st_mode & 0o777),
                }
            )
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    inventory = {
        "status": INVENTORY_STATUS,
        "version": inputs.version,
        "tree_sha256": hashlib.sha256(encoded).hexdigest(),
        "project_commit": project_commit,
        "cmake_feature_profile": inputs.switches,
        "features": profile.included_features(),
        "excluded_features": profile.excluded_features(),
        "qt_plugins": list(PLUGINS),
        "elf_files": sorted(records, key=lambda item: str(item["path"])),
        "host_base_exclusions": external,
        "third_party_inventory": f"{DOC_DIR}/third-party/inventory.json",
        "third_party_notices": notice_inventory["copied_library_packages"],
        "combined_notices": notice_inventory["combined_notices"]["path"],
        "source_offer": offer,
        "sbom": f"{DOC_DIR}/sbom.spdx.json",
        "payload": payload,
    }
    inventory_path = output.parent / f"{output.name}.inventory.json"
    inventory_path.write_text(json.dumps(inventory, indent=2, sort_keys=True) + "\n")
    return output, inventory_path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir",
        type=Path,
        required=True,
        help="Release tree from the package-appimage preset",
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--qt-plugin-root",
        type=Path,
        default=Path("/usr/lib/x86_64-linux-gnu/qt6/plugins"),
    )
    version = parser.add_mutually_exclusive_group(required=True)
    version.add_argument("--release-tag", help="release tag; must be v<project version>")
    version.add_argument("--version-suffix", help="pre-release suffix, as in 0.1.0~<suffix>")
    args = parser.parse_args()
    try:
        appdir, inventory = stage(
            args.build_dir,
            args.output.resolve(),
            args.qt_plugin_root,
            release_tag=args.release_tag,
            version_suffix=args.version_suffix,
        )
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")
    print(appdir)
    print(inventory)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Stage a local-only Qt Quick AppDir with an explicit host ELF inventory.

This is a dependency experiment, not an AppImage builder or release recipe.
It copies the observed Ubuntu runtime closure, so its output must not be
distributed before the file-level licence and portability review closes.
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
from pathlib import Path

from codec_source_gate import stage_ledger, verify_codec_gate, verify_lame_fft_fingerprint
from package_notices import stage_notices, verify_notices

PLUGINS = (
    "platforms/libqoffscreen.so",
    "platforms/libqxcb.so",
    "platforms/libqwayland.so",
    "platforminputcontexts/libibusplatforminputcontextplugin.so",
)

# Keep the loader and glibc ABI from the target Ubuntu 26.04 system. All other
# observed dependencies are copied for this local experiment, including files
# that a later reviewed AppImage exclusion policy might leave on the host.
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


def command(*args: str) -> str:
    result = subprocess.run(args, check=True, capture_output=True, text=True)
    return result.stdout


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def elf_info(path: Path) -> dict[str, object]:
    header = command("readelf", "-h", str(path))
    if "Machine:                           Advanced Micro Devices X86-64" not in header:
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
    version = command("dpkg-query", "-W", "-f=${Version}", package).strip()
    copyright_file = Path("/usr/share/doc") / package.split(":", 1)[0] / "copyright"
    return {
        "package": package,
        "version": version,
        "copyright": str(copyright_file) if copyright_file.is_file() else None,
    }


def write_text(path: Path, value: str, mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value)
    path.chmod(mode)


def stage(binary: Path, helper: Path, output: Path, plugin_root: Path) -> tuple[Path, Path]:
    source_root = Path(__file__).resolve().parent.parent
    if not output.is_relative_to(source_root / "build"):
        raise ValueError("local prototype output must stay under this checkout's ignored build/")
    if (
        platform.machine() != "x86_64"
        or 'VERSION_ID="26.04"' not in Path("/etc/os-release").read_text()
    ):
        raise ValueError("this prototype requires Ubuntu 26.04 x86-64")
    binary = binary.resolve(strict=True)
    helper = helper.resolve(strict=True)
    if binary.name != "oscillate" or helper.name != "oscillate_vst3_scan_helper":
        raise ValueError("expected the Quick executable and its VST3 scan helper")
    if binary.parent != helper.parent:
        raise ValueError("scanner helper must be the Quick build sibling")
    if output.exists():
        raise ValueError(f"refusing to overwrite an existing AppDir: {output}")
    if not output.name.endswith(".AppDir"):
        raise ValueError("output directory must end in .AppDir")
    app_info = elf_info(binary)
    elf_info(helper)
    if "libQt6Quick.so.6" not in app_info["needed"] or "libQt6Test.so.6" in app_info["needed"]:
        raise ValueError("input is not a production-style Quick build")
    plugins = [(name, (plugin_root / name).resolve(strict=True)) for name in PLUGINS]
    for _, plugin in plugins:
        elf_info(plugin)

    # Resolve every root separately so dlopen-only Qt platform/input plugins
    # contribute their own DT_NEEDED closure. Reject SONAME ambiguity.
    libraries: dict[str, Path] = {}
    for root in [binary, helper, *(path for _, path in plugins)]:
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
            destination.chmod(0o755)
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

        copy_elf(binary, bin_dir / binary.name, "project")
        copy_elf(helper, bin_dir / helper.name, "project")
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
            0o755,
        )
        write_text(
            bin_dir / "qt.conf", "[Paths]\nPrefix = ..\nPlugins = plugins\nQmlImports = qml\n"
        )
        write_text(
            stage_root / "oscillate-prototype.desktop",
            "[Desktop Entry]\nType=Application\nName=Oscillate (local AppDir prototype)\n"
            "Exec=oscillate\nIcon=oscillate-prototype\nTerminal=false\n"
            "Categories=AudioVideo;Audio;\n",
        )
        write_text(
            stage_root / "oscillate-prototype.svg",
            '<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" '
            'viewBox="0 0 64 64"><rect width="64" height="64" rx="12" '
            'fill="#202638"/><path d="M8 34h9l5-15 8 29 7-21 5 7h14" fill="none" '
            'stroke="#78d5c5" stroke-width="4" stroke-linecap="round" '
            'stroke-linejoin="round"/></svg>\n',
        )
        write_text(
            stage_root / "usr/share/doc/oscillate/README.prototype",
            "Local AppDir dependency experiment only; do not distribute.\n"
            "Third-party notice evidence is in third-party/inventory.json; product\n"
            "code/content grants, source/replacement duties and X11/Wayland remain open.\n",
        )
        notice_inventory = stage_notices(
            stage_root,
            source_root,
            scanner_enabled=True,
            runtime_enabled=True,
            copied_elfs=[record for record in records if record["kind"] != "project"],
        )
        codec_ledger = stage_ledger(stage_root, source_root)
        verify_codec_gate({"elf_files": records}, notice_inventory, codec_ledger)
        verify_lame_fft_fingerprint(stage_root, codec_ledger)
        verify_notices(
            stage_root,
            expected_copied_elfs={
                str(record["path"]) for record in records if record["kind"] != "project"
            },
            scanner_enabled=True,
            runtime_enabled=True,
        )
        epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
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
        "status": "local AppDir prototype; not for distribution",
        "tree_sha256": hashlib.sha256(encoded).hexdigest(),
        "project_commit": command("git", "-C", str(source_root), "rev-parse", "HEAD").strip(),
        "sources": {"quick": str(binary), "helper": str(helper), "plugin_root": str(plugin_root)},
        "qt_plugins": list(PLUGINS),
        "elf_files": sorted(records, key=lambda item: str(item["path"])),
        "host_base_exclusions": external,
        "third_party_inventory": "usr/share/doc/oscillate/third-party/inventory.json",
        "payload": payload,
        "limits": [
            "No embedded AppImage runtime yet; its pinned notice is staged, but "
            "static-dependency source and licence review is open",
            "Host-specific dependency copies; not a portable or approved release",
        ],
    }
    inventory_path = output.parent / f"{output.name}.inventory.json"
    inventory_path.write_text(json.dumps(inventory, indent=2, sort_keys=True) + "\n")
    return output, inventory_path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--scanner-helper", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--qt-plugin-root",
        type=Path,
        default=Path("/usr/lib/x86_64-linux-gnu/qt6/plugins"),
    )
    args = parser.parse_args()
    try:
        appdir, inventory = stage(
            args.binary, args.scanner_helper, args.output.resolve(), args.qt_plugin_root
        )
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")
    print(appdir)
    print(inventory)


if __name__ == "__main__":
    main()

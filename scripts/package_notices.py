"""Stage third-party notices and their machine-checkable inventory.

The inventory records provenance, not a per-binary licence conclusion or a
source/replacement offer. Packaging fails if evidence for a copied ELF is absent.
"""

from __future__ import annotations

import hashlib
import json
import re
import shutil
from pathlib import Path

INVENTORY_STATUS = "third-party notice inventory; not a licence conclusion"
# Notices for third-party code compiled into the project's own executables.
COMPILED_IN_NOTICES = {
    "nlohmann-json3-dev": Path("third_party/notices/nlohmann-json3-dev/copyright"),
    "pffft": Path("third_party/pffft/LICENSE.txt"),
}
RUNTIME_NOTICE_SHA256 = "aa154fc9070614bbe7921f89db11efd1dba7a1f3a41685958110e2230f9c0ca1"
# Full upstream notice files for the runtime's named static libraries and
# observed mimalloc code. The Alpine package revisions in the released runtime
# are not attested by its floating Docker build; see the dependency audit.
RUNTIME_STATIC_NOTICES = {
    "libfuse-lgpl2": (
        "libfuse-3.15.0-LGPL2.txt",
        "dc626520dcd53a22f727af3ee42c770e56c97a64fe3adb063799d8ab032fe551",
    ),
    "libfuse-license": (
        "libfuse-3.15.0-LICENSE",
        "b8832d9caaa075bbbd2aef24efa09f8b7ab66a832812d88c602da0c7b4397fad",
    ),
    "mimalloc": (
        "mimalloc-2.1.7-LICENSE",
        "19c99805e7a44a34b297a75d1edea9985e300066dfc024d5c99d4236d4573b5d",
    ),
    "musl": (
        "musl-1.2.5-COPYRIGHT",
        "f9bc4423732350eb0b3f7ed7e91d530298476f8fec0c6c427a1c04ade22655af",
    ),
    "squashfuse": (
        "squashfuse-0.5.2-LICENSE",
        "9e909cc8a8ba27b1a649c964cf9eda37de911f3138c2b64e6d2f01703904ac13",
    ),
    "zlib": (
        "zlib-1.3.1-LICENSE",
        "845efc77857d485d91fb3e0b884aaa929368c717ae8186b66fe1ed2495753243",
    ),
    "zstd": (
        "zstd-1.5.6-LICENSE",
        "7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8",
    ),
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _copy_notice(source: Path, destination: Path) -> dict[str, str]:
    if not source.is_file() or source.is_symlink():
        raise ValueError(f"missing regular notice file: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    destination.chmod(0o644)
    return {"path": str(destination), "sha256": sha256(destination)}


def stage_notices(
    stage_root: Path,
    source_root: Path,
    *,
    scanner_enabled: bool,
    runtime_enabled: bool = False,
    copied_elfs: list[dict[str, object]] | None = None,
    system_dependencies: str | None = None,
) -> dict[str, object]:
    """Stage notices and a machine-checkable file-to-notice mapping.

    ``copied_elfs`` is the AppDir's actual non-project ELF list. The native
    package supplies an empty list because Ubuntu supplies its shared libs.
    """
    doc_root = stage_root / "usr/share/doc/oscillate/third-party"
    project_sources = {
        name: source_root / relative for name, relative in COMPILED_IN_NOTICES.items()
    }
    if scanner_enabled:
        project_sources.update(
            {
                "vst3sdk": source_root / "third_party/notices/vst3sdk/LICENSE.txt",
                "vst3-pluginterfaces": source_root
                / "third_party/notices/vst3_pluginterfaces/LICENSE.txt",
            }
        )
    if runtime_enabled:
        runtime_notice = source_root / "docs/dependencies/notices/type2-runtime-20251108.LICENSE"
        if sha256(runtime_notice) != RUNTIME_NOTICE_SHA256:
            raise ValueError("pinned type-2 runtime notice changed")
        project_sources["type2-runtime"] = runtime_notice
    project_notices = []
    for name, source in sorted(project_sources.items()):
        destination = doc_root / "project" / f"{name}.txt"
        copied = _copy_notice(source, destination)
        project_notices.append(
            {
                "component": name,
                "path": str(destination.relative_to(stage_root)),
                "sha256": copied["sha256"],
                "source": str(source.relative_to(source_root)),
            }
        )

    runtime_static_notices = []
    if runtime_enabled:
        for component, (filename, expected_sha) in sorted(RUNTIME_STATIC_NOTICES.items()):
            source = source_root / "docs/dependencies/notices/runtime-static" / filename
            if sha256(source) != expected_sha:
                raise ValueError(f"pinned runtime static notice changed: {component}")
            destination = doc_root / "runtime-static" / f"{component}.txt"
            copied = _copy_notice(source, destination)
            runtime_static_notices.append(
                {
                    "component": component,
                    "path": str(destination.relative_to(stage_root)),
                    "sha256": copied["sha256"],
                    "source": str(source.relative_to(source_root)),
                }
            )

    copied_files = []
    packages: dict[str, dict[str, str]] = {}
    copied_paths: set[str] = set()
    for record in sorted(copied_elfs or [], key=lambda item: str(item["path"])):
        if record.get("kind") not in ("shared-library", "qt-plugin"):
            raise ValueError(f"unexpected copied ELF kind: {record.get('kind')}")
        relative = Path(str(record["path"]))
        if relative.is_absolute() or ".." in relative.parts or str(relative) in copied_paths:
            raise ValueError(f"unsafe or duplicate copied ELF path: {relative}")
        copied_paths.add(str(relative))
        package = str(record.get("package", ""))
        version = str(record.get("version", ""))
        copyright_path = Path(str(record.get("copyright", "")))
        if (
            not re.fullmatch(r"[a-z0-9][a-z0-9+.-]*(?::[a-z0-9]+)?", package)
            or not version
            or copyright_path != (Path("/usr/share/doc") / package.split(":", 1)[0] / "copyright")
        ):
            raise ValueError(f"missing package notice evidence: {record['path']}")
        staged_path = f"usr/share/doc/oscillate/third-party/ubuntu/{package}.copyright"
        if package not in packages:
            copied = _copy_notice(copyright_path, stage_root / staged_path)
            packages[package] = {
                "version": version,
                "notice_path": staged_path,
                "notice_sha256": copied["sha256"],
                "installed_notice_sha256": sha256(copyright_path),
            }
        elif packages[package]["version"] != version:
            raise ValueError(f"package version changed within payload: {package}")
        copied_files.append(
            {
                "path": str(record["path"]),
                "sha256": str(record["sha256"]),
                "binary_package": package,
                "binary_version": version,
                "notice_path": staged_path,
                "notice_sha256": packages[package]["notice_sha256"],
            }
        )

    inventory: dict[str, object] = {
        "schema": 1,
        "status": INVENTORY_STATUS,
        "project_notices": project_notices,
        "runtime_static_notices": runtime_static_notices,
        "copied_elfs": copied_files,
        "copied_library_packages": dict(sorted(packages.items())),
        "system_dependencies": system_dependencies,
        "open_gates": [
            "Starter-sound content grant is unset",
            "Per-binary licence/SPDX and matching source/offer review is incomplete",
            "LGPL library replacement and clean-host testing are incomplete",
        ]
        + (
            [
                "AppImage runtime dependency versions/build, corresponding source, and LGPL "
                "replacement remain unproven"
            ]
            if runtime_enabled
            else []
        ),
    }
    inventory_path = doc_root / "inventory.json"
    inventory_path.write_text(json.dumps(inventory, indent=2, sort_keys=True) + "\n")
    inventory_path.chmod(0o644)
    return inventory


def verify_notices(
    stage_root: Path,
    *,
    expected_copied_elfs: set[str],
    scanner_enabled: bool,
    runtime_enabled: bool = False,
) -> dict[str, object]:
    inventory_path = stage_root / "usr/share/doc/oscillate/third-party/inventory.json"
    inventory = json.loads(inventory_path.read_text())
    if inventory.get("schema") != 1 or inventory.get("status") != INVENTORY_STATUS:
        raise ValueError("invalid third-party notice inventory")
    copied_elfs = inventory["copied_elfs"]
    if (
        len(copied_elfs) != len(expected_copied_elfs)
        or {item["path"] for item in copied_elfs} != expected_copied_elfs
    ):
        raise ValueError("copied ELF notice mapping is incomplete")
    expected_components = set(COMPILED_IN_NOTICES)
    if scanner_enabled:
        expected_components.update({"vst3sdk", "vst3-pluginterfaces"})
    if runtime_enabled:
        expected_components.add("type2-runtime")
    project_notices = inventory["project_notices"]
    if (
        len(project_notices) != len(expected_components)
        or {item["component"] for item in project_notices} != expected_components
    ):
        raise ValueError("project third-party notices are incomplete")
    for item in project_notices:
        relative = Path(item["path"])
        expected_path = Path("usr/share/doc/oscillate/third-party/project") / (
            f"{item['component']}.txt"
        )
        if relative != expected_path:
            raise ValueError(f"unsafe notice path: {relative}")
        notice = stage_root / relative
        if not notice.is_file() or notice.is_symlink() or sha256(notice) != item["sha256"]:
            raise ValueError(f"missing or changed staged notice: {relative}")
        if item["component"] == "type2-runtime" and item["sha256"] != RUNTIME_NOTICE_SHA256:
            raise ValueError("pinned type-2 runtime notice changed")
    runtime_static_notices = inventory["runtime_static_notices"]
    expected_static = set(RUNTIME_STATIC_NOTICES) if runtime_enabled else set()
    if (
        len(runtime_static_notices) != len(expected_static)
        or {item["component"] for item in runtime_static_notices} != expected_static
    ):
        raise ValueError("runtime static notices are incomplete")
    for item in runtime_static_notices:
        component = item["component"]
        filename, expected_sha = RUNTIME_STATIC_NOTICES[component]
        relative = Path(item["path"])
        expected_path = Path("usr/share/doc/oscillate/third-party/runtime-static") / (
            f"{component}.txt"
        )
        expected_source = f"docs/dependencies/notices/runtime-static/{filename}"
        if (
            relative != expected_path
            or item["source"] != expected_source
            or item["sha256"] != expected_sha
        ):
            raise ValueError(f"runtime static notice mapping changed: {component}")
        notice = stage_root / relative
        if not notice.is_file() or notice.is_symlink() or sha256(notice) != expected_sha:
            raise ValueError(f"missing or changed runtime static notice: {component}")
    packages = inventory["copied_library_packages"]
    if set(packages) != {item["binary_package"] for item in copied_elfs}:
        raise ValueError("copied ELF package mapping is incomplete")
    for item in copied_elfs:
        package = item["binary_package"]
        if not re.fullmatch(r"[a-z0-9][a-z0-9+.-]*(?::[a-z0-9]+)?", package):
            raise ValueError(f"unsafe copied ELF package: {package}")
        evidence = packages[package]
        relative = Path(item["notice_path"])
        expected_path = Path("usr/share/doc/oscillate/third-party/ubuntu") / (
            f"{package}.copyright"
        )
        if (
            relative != expected_path
            or evidence["notice_path"] != str(relative)
            or evidence["notice_sha256"] != item["notice_sha256"]
            or evidence["installed_notice_sha256"] != item["notice_sha256"]
            or evidence["version"] != item["binary_version"]
        ):
            raise ValueError(f"unsafe notice path: {relative}")
        notice = stage_root / relative
        if not notice.is_file() or notice.is_symlink() or sha256(notice) != item["notice_sha256"]:
            raise ValueError(f"missing or changed staged notice: {relative}")
    for item in copied_elfs:
        relative = Path(item["path"])
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError(f"unsafe copied ELF path: {relative}")
        elf = stage_root / relative
        if not elf.is_file() or elf.is_symlink() or sha256(elf) != item["sha256"]:
            raise ValueError(f"missing or changed copied ELF: {relative}")
    return inventory

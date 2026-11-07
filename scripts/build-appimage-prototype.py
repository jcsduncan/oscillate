#!/usr/bin/env python3
"""Build and inspect a local, pinned type-2 AppImage from a reviewed AppDir.

This experiment does not prepare notices or approve redistribution.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import tempfile
from pathlib import Path

from codec_source_gate import (
    verify_codec_gate,
    verify_lame_fft_fingerprint,
    verify_staged_ledger,
)
from package_notices import verify_notices

TOOL_SHA256 = "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"
RUNTIME_SHA256 = "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d"
TOOL_RELEASE = "https://github.com/AppImage/appimagetool/releases/tag/1.9.1"
RUNTIME_RELEASE = "https://github.com/AppImage/type2-runtime/releases/tag/20251108"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def digest(path: Path) -> str:
    sha = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            sha.update(block)
    return sha.hexdigest()


def payload_files(root: Path, *, extracted: bool = False) -> list[dict[str, object]]:
    result = []
    icon = root / ".DirIcon"
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            require(
                extracted and path == icon and path.readlink() == Path("oscillate-prototype.svg"),
                f"unexpected symlink: {path}",
            )
            continue
        if path.is_dir():
            continue
        require(path.is_file(), f"unexpected non-file in AppDir: {path}")
        result.append(
            {
                "path": str(path.relative_to(root)),
                "sha256": digest(path),
                "size": path.stat().st_size,
                "mode": oct(path.stat().st_mode & 0o777),
            }
        )
    return result


def verify_tree(root: Path, inventory: dict[str, object], *, extracted: bool = False) -> None:
    payload = payload_files(root, extracted=extracted)
    if extracted:
        require((root / ".DirIcon").is_symlink(), "appimagetool icon symlink missing")
    require(payload == inventory["payload"], f"payload differs from inventory: {root}")
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    require(hashlib.sha256(encoded).hexdigest() == inventory["tree_sha256"], "bad tree hash")


def run(
    command: list[str], *, cwd: Path | None = None, env: dict[str, str] | None = None
) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(command, cwd=cwd, env=env, capture_output=True, check=True)


def build(
    appdir: Path, inventory_path: Path, tool: Path, runtime: Path, output: Path
) -> dict[str, object]:
    source_root = Path(__file__).resolve().parent.parent
    build_root = source_root / "build"
    require(platform.machine() == "x86_64", "x86-64 host required")
    require(
        'VERSION_ID="26.04"' in Path("/etc/os-release").read_text(),
        "Ubuntu 26.04 host required",
    )
    require(output.is_relative_to(build_root), "output must be in this checkout's ignored build/")
    require(output.suffix == ".AppImage", "output must end in .AppImage")
    require(not output.exists(), "refusing to overwrite an AppImage")
    require(digest(tool) == TOOL_SHA256, "appimagetool SHA-256 mismatch")
    require(digest(runtime) == RUNTIME_SHA256, "type-2 runtime SHA-256 mismatch")
    inventory = json.loads(inventory_path.read_text())
    require(inventory["status"].startswith("local AppDir prototype"), "wrong inventory kind")
    verify_tree(appdir, inventory)
    env = os.environ.copy()
    env.update({"APPIMAGE_EXTRACT_AND_RUN": "1", "ARCH": "x86_64", "SOURCE_DATE_EPOCH": "0"})
    # The explicit runtime prevents appimagetool from fetching a moving "latest" binary.
    with tempfile.TemporaryDirectory(prefix="oscillate-appimage-", dir=build_root) as temporary:
        temporary_root = Path(temporary)
        candidate = temporary_root / output.name
        tool_input = temporary_root / appdir.name
        # appimagetool adds .DirIcon; keep the inventoried source AppDir unchanged.
        shutil.copytree(appdir, tool_input, symlinks=True)
        result = run(
            [str(tool), "--runtime-file", str(runtime), str(tool_input), str(candidate)],
            env=env,
        )
        require(
            candidate.is_file(),
            f"appimagetool produced no image: {result.stderr.decode(errors='replace')}",
        )
        with candidate.open("rb") as image:
            require(image.read(16)[8:11] == b"AI\x02", "missing type-2 AppImage marker")
        offset = int(run([str(candidate), "--appimage-offset"]).stdout.strip())
        require(offset == runtime.stat().st_size, "embedded runtime length differs")
        with candidate.open("rb") as image, runtime.open("rb") as source:
            embedded = image.read(offset)
            pinned = source.read()
        changes = [
            index
            for index, (left, right) in enumerate(zip(embedded, pinned, strict=False))
            if left != right
        ]
        # appimagetool 1.9.1 writes the image MD5 into this 16-byte runtime slot.
        require(
            changes and all(932096 <= index < 932112 for index in changes),
            "embedded runtime differs outside MD5 slot",
        )
        run([str(candidate), "--appimage-extract"], cwd=temporary_root)
        extracted = temporary_root / "squashfs-root"
        verify_tree(extracted, inventory, extracted=True)
        notice_inventory = verify_notices(
            extracted,
            expected_copied_elfs={
                str(item["path"]) for item in inventory["elf_files"] if item["kind"] != "project"
            },
            scanner_enabled=True,
            runtime_enabled=True,
        )
        codec_ledger = verify_staged_ledger(extracted, source_root)
        verify_codec_gate(inventory, notice_inventory, codec_ledger)
        verify_lame_fft_fingerprint(extracted, codec_ledger)
        image_sha = digest(candidate)
        output.parent.mkdir(parents=True, exist_ok=True)
        os.replace(candidate, output)
    record = {
        "status": "local AppImage prototype; not for distribution",
        "appimage_sha256": image_sha,
        "appimage_bytes": output.stat().st_size,
        "appdir_tree_sha256": inventory["tree_sha256"],
        "appdir_project_commit": inventory["project_commit"],
        "appimagetool": {"release": TOOL_RELEASE, "sha256": TOOL_SHA256},
        "type2_runtime": {
            "release": RUNTIME_RELEASE,
            "sha256": RUNTIME_SHA256,
            "bytes": offset,
            "appimagetool_md5_slot": [932096, 932112],
        },
        "source_date_epoch": 0,
        "limits": [
            "Offscreen host smoke only; X11, Wayland, clean machine and physical audio unqualified",
            "Bundled licences, notices, source and LGPL replacement route not approved",
        ],
    }
    record_path = output.with_suffix(".AppImage.inventory.json")
    record_path.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    return record


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--appdir", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--appimagetool", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        record = build(
            args.appdir.resolve(strict=True),
            args.inventory.resolve(strict=True),
            args.appimagetool.resolve(strict=True),
            args.runtime.resolve(strict=True),
            args.output.resolve(),
        )
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")
    print(args.output, record["appimage_sha256"])


if __name__ == "__main__":
    main()

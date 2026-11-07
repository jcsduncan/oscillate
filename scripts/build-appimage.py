#!/usr/bin/env python3
"""Build and verify the type-2 AppImage from an AppDir staged by stage-appdir.py.

appimagetool and the runtime are pinned by SHA-256. The image is extracted
again and its payload, notices and SBOM are checked against the AppDir
inventory before the image is published.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

# Run as `python3 -I`, which leaves the script's directory off the path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from package_notices import verify_notices
from package_profiles import APPIMAGETOOL, SOURCE_ROOT, TYPE2_RUNTIME
from package_sbom import verify_sbom

APPDIR_STATUS = "oscillate AppDir inventory"
RECORD_STATUS = "oscillate AppImage inventory"
ICON = Path("oscillate.svg")
READ_CHUNK_BYTES = 1 << 20
IMAGE_HEADER_BYTES = 16
TYPE2_MARKER = slice(8, 11)
TYPE2_MAGIC = b"AI\x02"
# appimagetool 1.9.1 writes the image MD5 into this 16-byte runtime slot.
RUNTIME_MD5_SLOT = (932096, 932112)
EXECUTABLE_MODE = 0o755
DOWNLOAD_TIMEOUT_SECONDS = 120
INPUTS_DIR = SOURCE_ROOT / "build/appimage-inputs"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def digest(path: Path) -> str:
    sha = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(READ_CHUNK_BYTES), b""):
            sha.update(block)
    return sha.hexdigest()


def payload_files(root: Path, *, extracted: bool = False) -> list[dict[str, object]]:
    result = []
    icon = root / ".DirIcon"
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            require(
                extracted and path == icon and path.readlink() == ICON,
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


def fetch_input(pinned: dict[str, str], destination: Path) -> Path:
    """Download one pinned build input unless a verified copy is already there."""
    if not destination.is_file() or digest(destination) != pinned["sha256"]:
        destination.parent.mkdir(parents=True, exist_ok=True)
        partial = destination.with_name(f"{destination.name}.part")
        try:
            with (
                urllib.request.urlopen(pinned["url"], timeout=DOWNLOAD_TIMEOUT_SECONDS) as response,
                partial.open("wb") as target,
            ):
                shutil.copyfileobj(response, target)
            require(
                digest(partial) == pinned["sha256"], f"download SHA-256 mismatch: {pinned['url']}"
            )
            os.replace(partial, destination)
        finally:
            partial.unlink(missing_ok=True)
    destination.chmod(EXECUTABLE_MODE)
    return destination


def image_name(version: str) -> str:
    return f"Oscillate-{version}-x86_64.AppImage"


def run(
    command: list[str], *, cwd: Path | None = None, env: dict[str, str] | None = None
) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(command, cwd=cwd, env=env, capture_output=True, check=True)


def build(
    appdir: Path, inventory_path: Path, tool: Path, runtime: Path, output_dir: Path
) -> tuple[Path, dict[str, object]]:
    build_root = SOURCE_ROOT / "build"
    inventory = json.loads(inventory_path.read_text())
    require(inventory["status"] == APPDIR_STATUS, "wrong inventory kind")
    output = output_dir / image_name(inventory["version"])
    require(platform.machine() == "x86_64", "x86-64 host required")
    require(
        'VERSION_ID="26.04"' in Path("/etc/os-release").read_text(),
        "Ubuntu 26.04 host required",
    )
    require(output.is_relative_to(build_root), "output must be in this checkout's ignored build/")
    require(not output.exists(), "refusing to overwrite an AppImage")
    require(digest(tool) == APPIMAGETOOL["sha256"], "appimagetool SHA-256 mismatch")
    require(digest(runtime) == TYPE2_RUNTIME["sha256"], "type-2 runtime SHA-256 mismatch")
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
            require(
                image.read(IMAGE_HEADER_BYTES)[TYPE2_MARKER] == TYPE2_MAGIC,
                "missing type-2 AppImage marker",
            )
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
        slot_start, slot_end = RUNTIME_MD5_SLOT
        require(
            changes and all(slot_start <= index < slot_end for index in changes),
            "embedded runtime differs outside MD5 slot",
        )
        run([str(candidate), "--appimage-extract"], cwd=temporary_root)
        extracted = temporary_root / "squashfs-root"
        verify_tree(extracted, inventory, extracted=True)
        verify_notices(
            extracted,
            expected_copied_elfs={
                str(item["path"]) for item in inventory["elf_files"] if item["kind"] != "project"
            },
            scanner_enabled=True,
            runtime_enabled=True,
        )
        verify_sbom(extracted)
        image_sha = digest(candidate)
        output.parent.mkdir(parents=True, exist_ok=True)
        os.replace(candidate, output)
    record = {
        "status": RECORD_STATUS,
        "version": inventory["version"],
        "image": output.name,
        "appimage_sha256": image_sha,
        "appimage_bytes": output.stat().st_size,
        "appdir_tree_sha256": inventory["tree_sha256"],
        "appdir_project_commit": inventory["project_commit"],
        "features": inventory["features"],
        "excluded_features": inventory["excluded_features"],
        "appimagetool": {"release": APPIMAGETOOL["release"], "sha256": APPIMAGETOOL["sha256"]},
        "type2_runtime": {
            "release": TYPE2_RUNTIME["release"],
            "sha256": TYPE2_RUNTIME["sha256"],
            "bytes": offset,
            "appimagetool_md5_slot": list(RUNTIME_MD5_SLOT),
        },
        "source_date_epoch": 0,
    }
    record_path = output.with_name(f"{output.name}.inventory.json")
    record_path.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    return output, record


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--appdir", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument(
        "--appimagetool", type=Path, help="pinned appimagetool; downloaded when omitted"
    )
    parser.add_argument(
        "--runtime", type=Path, help="pinned type-2 runtime; downloaded when omitted"
    )
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    try:
        tool = args.appimagetool or fetch_input(APPIMAGETOOL, INPUTS_DIR / "appimagetool")
        runtime = args.runtime or fetch_input(TYPE2_RUNTIME, INPUTS_DIR / "runtime")
        output, record = build(
            args.appdir.resolve(strict=True),
            args.inventory.resolve(strict=True),
            tool.resolve(strict=True),
            runtime.resolve(strict=True),
            args.output_dir.resolve(),
        )
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")
    print(output, record["appimage_sha256"])


if __name__ == "__main__":
    main()

"""Bind the AppDir MP3 codec chain to reviewed local package evidence.

This is a drift gate, not an SPDX conclusion or distribution approval. The
native Ubuntu package uses system libraries and is outside this byte lock.
"""

from __future__ import annotations

import hashlib
import json
import shutil
from pathlib import Path

LEDGER_PATH = Path("docs/dependencies/mp3-ubuntu-codec-ledger.json")
STAGED_LEDGER_PATH = Path("usr/share/doc/oscillate/third-party/codec-source-ledger.json")


def load_ledger(source_root: Path) -> dict[str, object]:
    return json.loads((source_root / LEDGER_PATH).read_text())


def stage_ledger(stage_root: Path, source_root: Path) -> dict[str, object]:
    source = source_root / LEDGER_PATH
    destination = stage_root / STAGED_LEDGER_PATH
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    destination.chmod(0o644)
    return verify_staged_ledger(stage_root, source_root)


def verify_staged_ledger(stage_root: Path, source_root: Path) -> dict[str, object]:
    staged = stage_root / STAGED_LEDGER_PATH
    source = source_root / LEDGER_PATH
    if not staged.is_file() or staged.is_symlink():
        raise ValueError("MP3 codec source ledger is missing")
    if hashlib.sha256(staged.read_bytes()).digest() != hashlib.sha256(source.read_bytes()).digest():
        raise ValueError("MP3 codec source ledger differs from reviewed copy")
    return json.loads(staged.read_text())


def verify_codec_gate(
    appdir_inventory: dict[str, object],
    notice_inventory: dict[str, object],
    ledger: dict[str, object],
) -> None:
    """Fail if the copied MP3 encoder or its immediate dependency path drifts."""
    if ledger.get("schema") != 1 or set(ledger["components"]) != {
        "libsndfile",
        "libmp3lame",
        "libmpg123",
    }:
        raise ValueError("invalid MP3 codec source ledger")
    if ledger["lame_source_evidence"]["declared_fft_c_license"] != "GPL-1+":
        raise ValueError("LAME fft.c licence classification changed; review required")
    records = {item["path"]: item for item in appdir_inventory["elf_files"]}
    notices = {item["path"]: item for item in notice_inventory["copied_elfs"]}
    for name, expected in ledger["components"].items():
        path = expected["path"]
        record = records.get(path)
        notice = notices.get(path)
        if record is None or notice is None:
            raise ValueError(f"MP3 codec component is missing: {name}")
        if (
            record.get("kind") != "shared-library"
            or record.get("package") != expected["binary_package"]
            or record.get("version") != expected["binary_version"]
            or record.get("sha256") != expected["binary_sha256"]
            or notice.get("binary_package") != expected["binary_package"]
            or notice.get("binary_version") != expected["binary_version"]
            or notice.get("sha256") != expected["binary_sha256"]
            or notice.get("notice_sha256") != expected["notice_sha256"]
        ):
            raise ValueError(f"MP3 codec package or notice drift requires review: {name}")
    dependencies = (
        ("usr/bin/oscillate", "libsndfile.so.1"),
        (ledger["components"]["libsndfile"]["path"], "libmp3lame.so.0"),
        (ledger["components"]["libmp3lame"]["path"], "libmpg123.so.0"),
    )
    for path, soname in dependencies:
        if soname not in records.get(path, {}).get("needed", []):
            raise ValueError(f"MP3 codec dependency path changed: {path} -> {soname}")


def verify_lame_fft_fingerprint(stage_root: Path, ledger: dict[str, object]) -> None:
    """Check the known fht instruction segment from the matching fft.o."""
    record = ledger["components"]["libmp3lame"]
    match = ledger["lame_source_evidence"]["fft_text_match"]
    path = stage_root / record["path"]
    if not path.is_file() or path.is_symlink():
        raise ValueError("staged LAME encoder is missing")
    with path.open("rb") as source:
        source.seek(match["shared_elf_file_offset"])
        code = source.read(match["length"])
    if len(code) != match["length"] or hashlib.sha256(code).hexdigest() != match["sha256"]:
        raise ValueError("staged LAME fft.c code fingerprint changed")

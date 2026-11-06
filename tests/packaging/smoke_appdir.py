#!/usr/bin/env python3
"""Exercise one already-staged local Quick AppDir and its scanner helper."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from codec_source_gate import (
    verify_codec_gate,
    verify_lame_fft_fingerprint,
    verify_staged_ledger,
)
from package_notices import verify_notices


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args: list[str], env: dict[str, str]) -> subprocess.CompletedProcess[bytes]:
    result = subprocess.run(args, env=env, capture_output=True, check=True)
    return result


def launch_window(args: list[str], env: dict[str, str]) -> bytes:
    """The production app stays open; bound its offscreen launch for smoke."""
    try:
        completed = subprocess.run(args, env=env, capture_output=True, timeout=3, check=False)
    except subprocess.TimeoutExpired as error:
        return error.stderr or b""
    raise RuntimeError(
        f"production Quick app exited during launch: {completed.returncode}: "
        f"{completed.stderr.decode(errors='replace')}"
    )


def smoke(appdir: Path, inventory_path: Path, fixture: Path) -> None:
    appdir = appdir.resolve(strict=True)
    fixture = fixture.resolve(strict=True)
    inventory = json.loads(inventory_path.read_text())
    require(inventory["status"].startswith("local AppDir prototype"), "wrong inventory kind")
    payload = inventory["payload"]
    require(
        {item["path"] for item in payload}
        == {str(path.relative_to(appdir)) for path in appdir.rglob("*") if path.is_file()},
        "payload inventory omits a staged file",
    )
    for item in payload:
        staged = appdir / item["path"]
        require(digest(staged) == item["sha256"], "payload hash mismatch")
        require(staged.stat().st_size == item["size"], "payload size mismatch")
        require(oct(staged.stat().st_mode & 0o777) == item["mode"], "payload mode mismatch")
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    require(hashlib.sha256(encoded).hexdigest() == inventory["tree_sha256"], "tree hash mismatch")
    notices = verify_notices(
        appdir,
        expected_copied_elfs={
            str(item["path"]) for item in inventory["elf_files"] if item["kind"] != "project"
        },
        scanner_enabled=True,
        runtime_enabled=True,
    )
    codec_ledger = verify_staged_ledger(appdir, Path(__file__).resolve().parents[2])
    verify_codec_gate(inventory, notices, codec_ledger)
    verify_lame_fft_fingerprint(appdir, codec_ledger)
    require(
        all(not item["runtime_paths"] for item in inventory["elf_files"]),
        "staged ELF has RPATH/RUNPATH",
    )

    with tempfile.TemporaryDirectory(prefix="oscillate-appdir-smoke-") as temporary:
        project = Path(temporary) / "project"
        env = os.environ.copy()
        env.pop("LD_LIBRARY_PATH", None)
        env.update(
            {
                "QT_QPA_PLATFORM": "offscreen",
                "QT_QUICK_BACKEND": "software",
                "QT_DEBUG_PLUGINS": "1",
                "QML_IMPORT_TRACE": "1",
                "XDG_CONFIG_HOME": str(Path(temporary) / "config"),
            }
        )
        launch = str(appdir / "AppRun")
        created_trace = launch_window([launch, "--create", str(project)], env)
        require((project / "state/CURRENT").is_file(), "project was not created")
        reopened_trace = launch_window([launch, "--open", str(project)], env)
        for data in (created_trace, reopened_trace):
            trace = data.decode(errors="replace")
            for plugin in (
                "platforms/libqoffscreen.so",
                "platforminputcontexts/libibusplatforminputcontextplugin.so",
            ):
                require(
                    f'{appdir}/usr/plugins/{plugin}" loaded library' in trace,
                    f"Qt loaded {plugin} from outside the AppDir",
                )
            require(":/qt-project.org/imports/QtQuick/qmldir" in trace, "QtQuick import missing")
        ld_env = env | {"LD_LIBRARY_PATH": str(appdir / "usr/lib")}
        helper = run([str(appdir / "usr/bin/oscillate_vst3_scan_helper"), str(fixture)], ld_env)
        require(helper.stdout.startswith(b"LDV3\x01\x00"), "scanner did not accept fixture")
        dynamic = run(["ldd", str(appdir / "usr/bin/oscillate")], ld_env).stdout.decode()
        for soname in (
            "libQt6Quick.so.6",
            "libpipewire-0.3.so.0",
            "libsndfile.so.1",
            "libsamplerate.so.0",
            "libcrypto.so.3",
        ):
            require(
                f"{soname} => {appdir}/usr/lib/{soname}" in dynamic,
                f"staged {soname} was not selected by the loader",
            )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--appdir", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    args = parser.parse_args()
    smoke(args.appdir, args.inventory, args.fixture)
    print("AppDir payload, Qt load, create/reopen and VST3 helper smoke passed")


if __name__ == "__main__":
    main()

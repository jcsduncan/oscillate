#!/usr/bin/env python3
"""Exercise a staged AppDir: payload, notices, SBOM, app startup and scanner helper."""

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
from package_notices import verify_notices
from package_sbom import verify_sbom

INVENTORY_STATUS = "oscillate AppDir inventory"
STARTUP_TIMEOUT_SECONDS = 60
HELPER_TIMEOUT_SECONDS = 30
# Scanner reply header: magic, protocol version, then a status byte.
SCAN_REPLY_PREFIX = b"LDV3\x01"
SCAN_STATUS_OFFSET = len(SCAN_REPLY_PREFIX)
SCAN_STATUS_OK = 0
# Libraries the loader must take from the AppDir rather than the host.
BUNDLED_SONAMES = (
    "libQt6Quick.so.6",
    "libpipewire-0.3.so.0",
    "libjack.so.0",
    "libsamplerate.so.0",
    "libcrypto.so.3",
)
IN_APPDIR_PLUGINS = (
    "platforms/libqoffscreen.so",
    "platforminputcontexts/libibusplatforminputcontextplugin.so",
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_startup(args: list[str], env: dict[str, str], expected: str) -> str:
    """Run the app's own startup check, which quits once the window is up."""
    completed = subprocess.run(
        args, env=env, capture_output=True, timeout=STARTUP_TIMEOUT_SECONDS, check=False
    )
    stdout = completed.stdout.decode(errors="replace")
    stderr = completed.stderr.decode(errors="replace")
    require(
        completed.returncode == 0 and expected in stdout,
        f"startup check failed ({completed.returncode}): {stdout}{stderr}",
    )
    return stderr


def smoke(appdir: Path, inventory_path: Path) -> None:
    appdir = appdir.resolve(strict=True)
    inventory = json.loads(inventory_path.read_text())
    require(inventory["status"] == INVENTORY_STATUS, "wrong inventory kind")
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
    verify_notices(
        appdir,
        expected_copied_elfs={
            str(item["path"]) for item in inventory["elf_files"] if item["kind"] != "project"
        },
        scanner_enabled=True,
        runtime_enabled=True,
    )
    verify_sbom(appdir)
    require(
        all(not item["runtime_paths"] for item in inventory["elf_files"]),
        "staged ELF has RPATH/RUNPATH",
    )
    require(
        not any(Path(item["path"]).name == "libmp3lame.so.0" for item in inventory["elf_files"]),
        "AppDir bundles the MP3 encoder",
    )
    subprocess.run(["desktop-file-validate", str(appdir / "oscillate.desktop")], check=True)

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
                "HOME": str(Path(temporary) / "home"),
                "XDG_CONFIG_HOME": str(Path(temporary) / "config"),
            }
        )
        launch = str(appdir / "AppRun")
        traces = [
            check_startup(
                [launch, "--check-startup", "--create", str(project)], env, "Project ready"
            ),
            check_startup(
                [launch, "--check-startup", "--open", str(project)], env, "Project ready"
            ),
        ]
        require((project / "state/CURRENT").is_file(), "project was not created")
        for trace in traces:
            for plugin in IN_APPDIR_PLUGINS:
                require(
                    f'{appdir}/usr/plugins/{plugin}" loaded library' in trace,
                    f"Qt loaded {plugin} from outside the AppDir",
                )
            require(":/qt-project.org/imports/QtQuick/qmldir" in trace, "QtQuick import missing")
        ld_env = env | {"LD_LIBRARY_PATH": str(appdir / "usr/lib")}
        helper = subprocess.run(
            [
                str(appdir / "usr/bin/oscillate_vst3_scan_helper"),
                str(Path(temporary) / "None.vst3"),
            ],
            env=ld_env,
            capture_output=True,
            timeout=HELPER_TIMEOUT_SECONDS,
            check=True,
        )
        require(
            helper.stdout.startswith(SCAN_REPLY_PREFIX)
            and helper.stdout[SCAN_STATUS_OFFSET] != SCAN_STATUS_OK,
            "scanner helper did not report the missing bundle",
        )
        dynamic = subprocess.run(
            ["ldd", str(appdir / "usr/bin/oscillate")],
            env=ld_env,
            capture_output=True,
            text=True,
            check=True,
        ).stdout
        for soname in BUNDLED_SONAMES:
            require(
                f"{soname} => {appdir}/usr/lib/{soname}" in dynamic,
                f"staged {soname} was not selected by the loader",
            )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--appdir", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    args = parser.parse_args()
    smoke(args.appdir, args.inventory)
    print("AppDir payload, notices, SBOM, startup and VST3 helper smoke passed")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Map a staged AppDir's copied ELF files to installed package notices.

This is evidence collection, not an SPDX conclusion or redistribution approval.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from collections import Counter
from collections.abc import Callable
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def dpkg(*args: str) -> str:
    result = subprocess.run(["dpkg-query", *args], text=True, capture_output=True, check=False)
    require(result.returncode == 0, f"dpkg-query failed for {args}: {result.stderr.strip()}")
    return result.stdout.strip()


def installed_package(record: dict[str, object]) -> dict[str, object]:
    package = str(record["package"])
    source = Path(str(record["source"]))
    owners = dpkg("-S", str(source))
    require(
        any(package in line.split(": ", 1)[0].split(", ") for line in owners.splitlines()),
        f"source no longer owned by {package}: {source}",
    )
    fields = dpkg("-W", "-f=${Version}|${source:Package}|${source:Version}", package)
    parts = fields.split("|")
    require(len(parts) == 3, f"bad package/source metadata: {package}")
    version, source_package, source_version = parts
    require(version == record["version"], f"package version drift: {package}")
    require(source_package and source_version, f"missing source-package metadata: {package}")
    copyright_path = Path(str(record["copyright"]))
    require(
        copyright_path == Path("/usr/share/doc") / package.split(":", 1)[0] / "copyright",
        f"unexpected copyright path for {package}",
    )
    require(copyright_path.is_file(), f"missing package copyright: {copyright_path}")
    return {
        "binary_package": package,
        "binary_version": version,
        "source_package": source_package,
        "source_version": source_version,
        "copyright_path": str(copyright_path),
        "copyright_sha256": sha256(copyright_path),
        "copyright_text": copyright_path.read_text(errors="replace"),
    }


def copyright_clues(text: str) -> dict[str, object]:
    dep5 = bool(re.search(r"(?m)^Format:[ \t]*https?://.*copyright-format", text))
    labels = sorted(set(re.findall(r"(?m)^License:[ \t]*([^\n]+)", text)))
    default = None
    if dep5:
        for stanza in re.split(r"\n\s*\n", text):
            files = re.search(r"(?m)^Files:[ \t]*([^\n]+)", stanza)
            if files and "*" in files.group(1).split():
                licence = re.search(r"(?m)^License:[ \t]*([^\n]+)", stanza)
                if licence:
                    default = licence.group(1).strip()
                    break
    upstream = re.search(r"(?m)^Source:[ \t]*(\S+)", text) if dep5 else None
    flags = []
    if not dep5:
        flags.append("copyright_not_machine_readable")
    if not labels:
        flags.append("no_machine_readable_license_labels")
    if default is None:
        flags.append("no_general_files_license")
    if len(labels) > 1:
        flags.append("multiple_source_license_labels")
    if re.search(r"\bLGPL\b|Lesser General Public License", text, re.I):
        flags.append("possible_lgpl_replacement_obligations")
    if upstream is None:
        flags.append("upstream_source_url_not_structured")
    return {
        "copyright_format": "DEP-5" if dep5 else "prose-or-other",
        "declared_source_license_labels": labels,
        "general_files_license_clue": default,
        "upstream_source_url": upstream.group(1) if upstream else None,
        "review_flags": flags,
    }


def validate_inventory(inventory: dict[str, object], appdir: Path) -> list[dict[str, object]]:
    require(
        inventory.get("status", "").startswith("local AppDir prototype"),
        "wrong inventory kind",
    )
    payload = inventory["payload"]
    require(isinstance(payload, list) and payload, "missing payload inventory")
    paths: set[str] = set()
    for item in payload:
        relative = Path(str(item["path"]))
        require(
            not relative.is_absolute()
            and ".." not in relative.parts
            and str(relative) not in paths,
            f"unsafe or duplicate payload path: {relative}",
        )
        paths.add(str(relative))
        staged = appdir / relative
        require(staged.is_file() and not staged.is_symlink(), f"missing staged file: {relative}")
        require(sha256(staged) == item["sha256"], f"staged hash mismatch: {relative}")
        require(staged.stat().st_size == item["size"], f"staged size mismatch: {relative}")
        require(
            oct(staged.stat().st_mode & 0o777) == item["mode"],
            f"staged mode mismatch: {relative}",
        )
    actual = {str(path.relative_to(appdir)) for path in appdir.rglob("*") if path.is_file()}
    require(actual == paths, "AppDir file set differs from staged inventory")
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    require(
        hashlib.sha256(encoded).hexdigest() == inventory["tree_sha256"],
        "staged inventory tree hash mismatch",
    )
    elf_records = inventory["elf_files"]
    require(isinstance(elf_records, list), "missing ELF inventory")
    copied = [record for record in elf_records if record["kind"] != "project"]
    require(
        all(record["kind"] in ("shared-library", "qt-plugin") for record in copied),
        "unexpected copied ELF kind",
    )
    require(
        len({record["path"] for record in elf_records}) == len(elf_records),
        "duplicate ELF path",
    )
    for record in copied:
        require(record["path"] in paths, f"ELF not in payload: {record['path']}")
        require(record["sha256"] == sha256(appdir / str(record["path"])), "ELF hash mismatch")
        source = Path(str(record["source"]))
        require(source.is_absolute() and source.is_file(), f"missing host source: {source}")
        require(sha256(source) == record["sha256"], f"host source bytes changed: {source}")
        require(
            record.get("package") and record.get("version") and record.get("copyright"),
            f"missing package evidence: {record['path']}",
        )
    return copied


def build_ledger(
    inventory: dict[str, object],
    appdir: Path,
    lookup: Callable[[dict[str, object]], dict[str, object]] = installed_package,
) -> dict[str, object]:
    copied = validate_inventory(inventory, appdir)
    package_cache: dict[str, dict[str, object]] = {}
    files = []
    for record in sorted(copied, key=lambda item: str(item["path"])):
        package = str(record["package"])
        if package not in package_cache:
            evidence = lookup(record)
            require(evidence["binary_package"] == package, f"package mismatch: {package}")
            require(evidence["binary_version"] == record["version"], f"version mismatch: {package}")
            clues = copyright_clues(str(evidence["copyright_text"]))
            package_cache[package] = {
                key: value for key, value in evidence.items() if key != "copyright_text"
            } | clues
        notice_in_appdir = (
            appdir / "usr/share/doc" / package.split(":", 1)[0] / "copyright"
        ).is_file()
        files.append(
            {
                "path": record["path"],
                "kind": record["kind"],
                "sha256": record["sha256"],
                "binary_package": package,
                "source_package": package_cache[package]["source_package"],
                "source_version": package_cache[package]["source_version"],
                "copyright_path": package_cache[package]["copyright_path"],
                "copyright_sha256": package_cache[package]["copyright_sha256"],
                "notice_in_appdir": notice_in_appdir,
                "general_files_license_clue": package_cache[package]["general_files_license_clue"],
                "review_flags": package_cache[package]["review_flags"],
            }
        )
    counts = Counter(flag for entry in files for flag in entry["review_flags"])
    return {
        "schema": 1,
        "status": (
            "installed-package evidence; not a per-binary SPDX conclusion or release approval"
        ),
        "appdir_tree_sha256": inventory["tree_sha256"],
        "staging_checkout_commit": inventory["project_commit"],
        "copied_elf_count": len(files),
        "binary_package_count": len(package_cache),
        "review_flag_file_counts": dict(sorted(counts.items())),
        "copied_file_notices_in_appdir": sum(item["notice_in_appdir"] for item in files),
        "files": files,
        "packages": dict(sorted(package_cache.items())),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--appdir", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--ledger", type=Path, required=True)
    parser.add_argument("--verify", action="store_true", help="compare with existing ledger")
    args = parser.parse_args()
    try:
        inventory = json.loads(args.inventory.read_text())
        ledger = build_ledger(inventory, args.appdir.resolve(strict=True))
        encoded = json.dumps(ledger, indent=2, sort_keys=True) + "\n"
        if args.verify:
            require(
                args.ledger.read_text() == encoded,
                "ledger differs from current package evidence",
            )
        else:
            require(not args.ledger.exists(), "refusing to overwrite an existing ledger")
            args.ledger.write_text(encoded)
    except (KeyError, TypeError, ValueError, OSError, json.JSONDecodeError) as error:
        parser.exit(1, f"{error}\n")
    print(
        f"{ledger['copied_elf_count']} copied ELFs, "
        f"{ledger['binary_package_count']} packages; "
        f"review flags: {ledger['review_flag_file_counts']}"
    )


if __name__ == "__main__":
    main()

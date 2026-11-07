#!/usr/bin/env python3
"""Filesystem and package-metadata failures for the AppDir licence ledger."""

from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import appdir_licence_ledger as ledger


class LedgerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="appdir-ledger-test-")
        self.addCleanup(self.temporary.cleanup)
        root = Path(self.temporary.name)
        self.appdir = root / "test.AppDir"
        staged = self.appdir / "usr/lib/libexample.so.1"
        staged.parent.mkdir(parents=True)
        staged.write_bytes(b"independent library fixture\n")
        source = root / "installed/libexample.so.1"
        source.parent.mkdir(parents=True)
        source.write_bytes(staged.read_bytes())
        digest = hashlib.sha256(staged.read_bytes()).hexdigest()
        payload = [
            {
                "path": "usr/lib/libexample.so.1",
                "sha256": digest,
                "size": staged.stat().st_size,
                "mode": oct(staged.stat().st_mode & 0o777),
            }
        ]
        encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
        self.inventory = {
            "status": "local AppDir prototype; not for distribution",
            "tree_sha256": hashlib.sha256(encoded).hexdigest(),
            "project_commit": "fixture",
            "payload": payload,
            "elf_files": [
                {
                    "path": payload[0]["path"],
                    "kind": "shared-library",
                    "sha256": digest,
                    "source": str(source),
                    "package": "libexample:amd64",
                    "version": "1.2-3",
                    "copyright": "/usr/share/doc/libexample/copyright",
                }
            ],
        }

    @staticmethod
    def package_evidence(_: dict[str, object]) -> dict[str, object]:
        return {
            "binary_package": "libexample:amd64",
            "binary_version": "1.2-3",
            "source_package": "example-source",
            "source_version": "1.2-3",
            "copyright_path": "/usr/share/doc/libexample/copyright",
            "copyright_sha256": "abc123",
            "copyright_text": (
                "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/\n"
                "Source: https://example.invalid/source\n\n"
                "Files: *\nCopyright: Example\nLicense: MIT\n"
            ),
        }

    def test_maps_real_staged_bytes_to_source_and_notice_clue(self) -> None:
        result = ledger.build_ledger(self.inventory, self.appdir, self.package_evidence)
        self.assertEqual(result["copied_elf_count"], 1)
        self.assertEqual(result["binary_package_count"], 1)
        self.assertEqual(result["files"][0]["source_package"], "example-source")
        self.assertEqual(result["files"][0]["general_files_license_clue"], "MIT")
        self.assertFalse(result["files"][0]["notice_in_appdir"])

    def test_rejects_malicious_inventory_path_before_lookup(self) -> None:
        self.inventory["payload"][0]["path"] = "../escape"
        with self.assertRaisesRegex(ValueError, "unsafe or duplicate payload path"):
            ledger.build_ledger(self.inventory, self.appdir, self.package_evidence)

    def test_rejects_changed_staged_library(self) -> None:
        (self.appdir / "usr/lib/libexample.so.1").write_bytes(b"different bytes")
        with self.assertRaisesRegex(ValueError, "staged hash mismatch"):
            ledger.build_ledger(self.inventory, self.appdir, self.package_evidence)

    def test_rejects_changed_installed_source_bytes(self) -> None:
        Path(self.inventory["elf_files"][0]["source"]).write_bytes(b"new package bytes")
        with self.assertRaisesRegex(ValueError, "host source bytes changed"):
            ledger.build_ledger(self.inventory, self.appdir, self.package_evidence)

    def test_rejects_missing_package_owner(self) -> None:
        record = self.inventory["elf_files"][0]
        with (
            patch.object(ledger, "dpkg", return_value="other:amd64: /path"),
            self.assertRaisesRegex(ValueError, "source no longer owned"),
        ):
            ledger.installed_package(record)

    def test_rejects_missing_copyright_file(self) -> None:
        record = self.inventory["elf_files"][0]
        source = record["source"]

        def fake_dpkg(*args: str) -> str:
            if args[0] == "-S":
                return f"libexample:amd64: {source}"
            return "1.2-3|example-source|1.2-3"

        with (
            patch.object(ledger, "dpkg", side_effect=fake_dpkg),
            self.assertRaisesRegex(ValueError, "missing package copyright"),
        ):
            ledger.installed_package(record)


if __name__ == "__main__":
    unittest.main()

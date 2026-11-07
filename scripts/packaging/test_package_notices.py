"""Small negative cases for packaging notice evidence."""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from package_notices import stage_notices, verify_notices

SOURCE_ROOT = Path(__file__).resolve().parents[2]


class NoticePayloadTest(unittest.TestCase):
    def test_project_notices_and_tamper_rejection(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inventory = stage_notices(root, SOURCE_ROOT, scanner_enabled=True)
            self.assertEqual(len(inventory["project_notices"]), 3)
            verify_notices(root, expected_copied_elfs=set(), scanner_enabled=True)
            notice = root / inventory["project_notices"][0]["path"]
            notice.write_text("changed")
            with self.assertRaisesRegex(ValueError, "changed staged notice"):
                verify_notices(root, expected_copied_elfs=set(), scanner_enabled=True)

    def test_missing_copied_elf_notice_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaisesRegex(ValueError, "missing package notice evidence"):
                stage_notices(
                    root,
                    SOURCE_ROOT,
                    scanner_enabled=False,
                    copied_elfs=[
                        {
                            "path": "usr/lib/libexample.so",
                            "sha256": "0" * 64,
                            "kind": "shared-library",
                            "package": "example:amd64",
                            "version": "1",
                            "copyright": "/missing/copyright",
                        }
                    ],
                )

    def test_required_vst3_notice_cannot_be_omitted(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage_notices(root, SOURCE_ROOT, scanner_enabled=False)
            with self.assertRaisesRegex(ValueError, "project third-party notices are incomplete"):
                verify_notices(root, expected_copied_elfs=set(), scanner_enabled=True)

    def test_runtime_notice_is_pinned_and_required_for_appimage(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inventory = stage_notices(root, SOURCE_ROOT, scanner_enabled=True, runtime_enabled=True)
            self.assertEqual(len(inventory["project_notices"]), 4)
            self.assertEqual(len(inventory["runtime_static_notices"]), 7)
            verify_notices(
                root, expected_copied_elfs=set(), scanner_enabled=True, runtime_enabled=True
            )
            with self.assertRaisesRegex(ValueError, "project third-party notices are incomplete"):
                verify_notices(root, expected_copied_elfs=set(), scanner_enabled=True)
            runtime = next(
                item
                for item in inventory["project_notices"]
                if item["component"] == "type2-runtime"
            )
            (root / runtime["path"]).write_text("changed")
            with self.assertRaisesRegex(ValueError, "changed staged notice"):
                verify_notices(
                    root, expected_copied_elfs=set(), scanner_enabled=True, runtime_enabled=True
                )

    def test_missing_or_changed_runtime_static_notice_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inventory = stage_notices(root, SOURCE_ROOT, scanner_enabled=True, runtime_enabled=True)
            inventory_path = root / "usr/share/doc/oscillate/third-party/inventory.json"
            inventory["runtime_static_notices"] = inventory["runtime_static_notices"][:-1]
            inventory_path.write_text(json.dumps(inventory))
            with self.assertRaisesRegex(ValueError, "runtime static notices are incomplete"):
                verify_notices(
                    root, expected_copied_elfs=set(), scanner_enabled=True, runtime_enabled=True
                )
            inventory = stage_notices(root, SOURCE_ROOT, scanner_enabled=True, runtime_enabled=True)
            notice = root / inventory["runtime_static_notices"][0]["path"]
            notice.write_text("changed")
            with self.assertRaisesRegex(ValueError, "changed runtime static notice"):
                verify_notices(
                    root, expected_copied_elfs=set(), scanner_enabled=True, runtime_enabled=True
                )

    def test_rejects_duplicate_mapping_and_mismatched_package_notice(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inventory = stage_notices(root, SOURCE_ROOT, scanner_enabled=False)
            elf_path = "usr/lib/libexample.so"
            notice_path = "usr/share/doc/oscillate/third-party/ubuntu/libexample.copyright"
            elf = root / elf_path
            notice = root / notice_path
            elf.parent.mkdir(parents=True)
            notice.parent.mkdir(parents=True)
            elf.write_bytes(b"\x7fELF fixture")
            notice.write_text("fixture copyright")
            elf_hash = hashlib.sha256(elf.read_bytes()).hexdigest()
            notice_hash = hashlib.sha256(notice.read_bytes()).hexdigest()
            entry = {
                "path": elf_path,
                "sha256": elf_hash,
                "binary_package": "libexample",
                "binary_version": "1",
                "notice_path": notice_path,
                "notice_sha256": notice_hash,
            }
            inventory["copied_elfs"] = [entry, dict(entry)]
            inventory["copied_library_packages"] = {
                "libexample": {
                    "version": "1",
                    "notice_path": notice_path,
                    "notice_sha256": notice_hash,
                    "installed_notice_sha256": notice_hash,
                }
            }
            inventory_path = root / "usr/share/doc/oscillate/third-party/inventory.json"
            inventory_path.write_text(json.dumps(inventory))
            with self.assertRaisesRegex(ValueError, "copied ELF notice mapping is incomplete"):
                verify_notices(root, expected_copied_elfs={elf_path}, scanner_enabled=False)
            inventory["copied_elfs"] = [entry]
            inventory["copied_library_packages"]["libexample"]["notice_sha256"] = "0" * 64
            inventory_path.write_text(json.dumps(inventory))
            with self.assertRaisesRegex(ValueError, "unsafe notice path"):
                verify_notices(root, expected_copied_elfs={elf_path}, scanner_enabled=False)


if __name__ == "__main__":
    unittest.main()

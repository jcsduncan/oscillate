"""Small negative cases for packaging notice evidence."""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from package_notices import (
    COMBINED_NOTICES,
    cited_common_licenses,
    stage_notices,
    verify_notices,
)

SOURCE_ROOT = Path(__file__).resolve().parents[2]
# nlohmann/json and PFFFT, plus the VST3 SDK root and pluginterfaces notices.
SCANNER_PROJECT_NOTICES = 4
# The AppImage adds the type-2 runtime's own licence.
APPIMAGE_PROJECT_NOTICES = SCANNER_PROJECT_NOTICES + 1
RUNTIME_STATIC_NOTICE_FILES = 7
# Every Ubuntu image keeps libc6's copyright file, which cites common licences.
HOST_PACKAGE = "libc6"
HOST_COPYRIGHT = Path("/usr/share/doc") / HOST_PACKAGE / "copyright"
COPIED_LIBRARY = "usr/lib/libc-fixture.so.6"
# The cited name a fixture licence directory serves through a symlink.
SYMLINKED_LICENCE = "GPL-3"
SYMLINK_TARGET = "GPL-3-full-text"


class NoticePayloadTest(unittest.TestCase):
    def test_project_notices_and_tamper_rejection(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inventory = stage_notices(root, SOURCE_ROOT, scanner_enabled=True)
            self.assertEqual(len(inventory["project_notices"]), SCANNER_PROJECT_NOTICES)
            verify_notices(root, expected_copied_elfs=set(), scanner_enabled=True)
            notice = root / inventory["project_notices"][0]["path"]
            notice.write_text("changed")
            with self.assertRaisesRegex(ValueError, "changed staged notice"):
                verify_notices(root, expected_copied_elfs=set(), scanner_enabled=True)

    def test_compiled_in_pffft_notice_is_shipped_and_required(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inventory = stage_notices(root, SOURCE_ROOT, scanner_enabled=False)
            pffft = next(
                item for item in inventory["project_notices"] if item["component"] == "pffft"
            )
            self.assertEqual(
                (root / pffft["path"]).read_bytes(),
                (SOURCE_ROOT / "third_party/pffft/LICENSE.txt").read_bytes(),
            )
            inventory_path = root / "usr/share/doc/oscillate/third-party/inventory.json"
            inventory["project_notices"] = [
                item for item in inventory["project_notices"] if item["component"] != "pffft"
            ]
            inventory_path.write_text(json.dumps(inventory))
            with self.assertRaisesRegex(ValueError, "project third-party notices are incomplete"):
                verify_notices(root, expected_copied_elfs=set(), scanner_enabled=False)

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
            self.assertEqual(len(inventory["project_notices"]), APPIMAGE_PROJECT_NOTICES)
            self.assertEqual(len(inventory["runtime_static_notices"]), RUNTIME_STATIC_NOTICE_FILES)
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


class CommonLicenceTest(unittest.TestCase):
    def test_cited_names_drop_sentence_full_stops(self) -> None:
        text = (
            "see /usr/share/common-licenses/LGPL-2.1.\n"
            "and /usr/share/common-licenses/GPL-3, or /usr/share/common-licenses/Apache-2.0"
        )
        self.assertEqual(cited_common_licenses(text), {"LGPL-2.1", "GPL-3", "Apache-2.0"})

    def stage_copied_library(self, root: Path, licences: Path) -> dict[str, object]:
        library = root / COPIED_LIBRARY
        library.parent.mkdir(parents=True)
        library.write_bytes(b"\x7fELF fixture")
        return stage_notices(
            root,
            SOURCE_ROOT,
            scanner_enabled=False,
            common_licenses=licences,
            copied_elfs=[
                {
                    "path": COPIED_LIBRARY,
                    "sha256": hashlib.sha256(library.read_bytes()).hexdigest(),
                    "kind": "shared-library",
                    "package": f"{HOST_PACKAGE}:amd64",
                    "version": "1",
                    "copyright": str(HOST_COPYRIGHT),
                }
            ],
        )

    def fixture_licences(self, root: Path) -> Path:
        licences = root / "common-licenses"
        licences.mkdir()
        for name in cited_common_licenses(HOST_COPYRIGHT.read_text(errors="replace")):
            (licences / name).write_text(f"full text of {name}\n")
        (licences / SYMLINK_TARGET).write_text("full text behind a symlink\n")
        (licences / SYMLINKED_LICENCE).unlink(missing_ok=True)
        (licences / SYMLINKED_LICENCE).symlink_to(SYMLINK_TARGET)
        return licences

    @unittest.skipUnless(HOST_COPYRIGHT.is_file(), "needs an Ubuntu host package copyright")
    def test_cited_licences_and_combined_notices_are_staged_and_checked(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "stage"
            licences = self.fixture_licences(Path(temporary))
            inventory = self.stage_copied_library(root, licences)
            cited = cited_common_licenses(HOST_COPYRIGHT.read_text(errors="replace"))
            staged = {name for item in inventory["common_licenses"] for name in item["cited_as"]}
            self.assertEqual(staged, cited)
            self.assertTrue(cited)
            combined = (root / COMBINED_NOTICES).read_text()
            self.assertIn(f"Ubuntu package {HOST_PACKAGE}:amd64 1", combined)
            for item in inventory["common_licenses"]:
                self.assertIn((root / item["path"]).read_text(), combined)
            if SYMLINKED_LICENCE in cited:
                self.assertIn(
                    SYMLINK_TARGET, {item["name"] for item in inventory["common_licenses"]}
                )
            verify_notices(root, expected_copied_elfs={COPIED_LIBRARY}, scanner_enabled=False)

            (root / COMBINED_NOTICES).write_text("changed")
            with self.assertRaisesRegex(ValueError, "NOTICES.txt does not match"):
                verify_notices(root, expected_copied_elfs={COPIED_LIBRARY}, scanner_enabled=False)

            inventory_path = root / "usr/share/doc/oscillate/third-party/inventory.json"
            inventory["common_licenses"] = inventory["common_licenses"][1:]
            inventory_path.write_text(json.dumps(inventory))
            with self.assertRaisesRegex(ValueError, "cited common licence texts differ"):
                verify_notices(root, expected_copied_elfs={COPIED_LIBRARY}, scanner_enabled=False)

    @unittest.skipUnless(HOST_COPYRIGHT.is_file(), "needs an Ubuntu host package copyright")
    def test_a_cited_licence_missing_on_the_host_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            licences = Path(temporary) / "empty-common-licenses"
            licences.mkdir()
            with self.assertRaisesRegex(ValueError, "cites a missing licence text"):
                self.stage_copied_library(Path(temporary) / "stage", licences)


if __name__ == "__main__":
    unittest.main()

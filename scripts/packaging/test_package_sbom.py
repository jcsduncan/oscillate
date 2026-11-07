#!/usr/bin/env python3
"""The SPDX SBOM shipped in each package: content, structure and tamper checks."""

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from package_notices import stage_notices
from package_sbom import SBOM_PATH, check_document, verify_sbom, write_sbom

SOURCE_ROOT = Path(__file__).resolve().parents[2]
VERSION = "0.4.2"
NLOHMANN_VERSION = "3.11.3-1"
EPOCH = 1_700_000_000
CREATED = "2023-11-14T22:13:20Z"
RUNTIME_SHA256 = "ab" * 32
DEPENDS = "libc6 (>= 2.38), libjack-jackd2-0 (>= 1.9.10) | libjack-0.125, qml6-module-qtquick"
COPIED_LIBRARY = "usr/lib/libexample.so.1"
COPIED_PACKAGE = "libexample1:amd64"
COPIED_VERSION = "1.2-3"


class SbomTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="oscillate-sbom-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        app = self.root / "usr/bin/oscillate"
        app.parent.mkdir(parents=True)
        app.write_bytes(b"\x7fELF app")

    def by_name(self, document):
        return {package["name"]: package for package in document["packages"]}

    def related(self, document, element, kind):
        return {
            item["relatedSpdxElement"]
            for item in document["relationships"]
            if item["spdxElementId"] == element and item["relationshipType"] == kind
        }

    def write_deb(self):
        notices = stage_notices(self.root, SOURCE_ROOT, scanner_enabled=True)
        control = self.root / "DEBIAN/control"
        control.parent.mkdir()
        control.write_text("Package: oscillate\n")
        return write_sbom(
            self.root,
            package_name="oscillate",
            version=VERSION,
            form="deb",
            notice_inventory=notices,
            nlohmann_version=NLOHMANN_VERSION,
            creation_epoch=EPOCH,
            depends=DEPENDS,
        )

    def test_deb_lists_payload_compiled_in_code_and_dependencies(self):
        document = self.write_deb()
        self.assertEqual(verify_sbom(self.root), document)
        self.assertEqual(json.loads((self.root / SBOM_PATH).read_text()), document)
        self.assertEqual(document["creationInfo"]["created"], CREATED)
        names = {item["fileName"] for item in document["files"]}
        self.assertIn("./usr/bin/oscillate", names)
        self.assertIn("./usr/share/doc/oscillate/third-party/project/pffft.txt", names)
        self.assertNotIn("./DEBIAN/control", names)
        self.assertNotIn(f"./{SBOM_PATH}", names)
        packages = self.by_name(document)
        root = packages["oscillate"]
        self.assertEqual(root["versionInfo"], VERSION)
        self.assertEqual(root["licenseDeclared"], "GPL-3.0-or-later")
        self.assertEqual(packages["nlohmann-json"]["versionInfo"], NLOHMANN_VERSION)
        for name in ("nlohmann-json", "pffft", "vst3sdk-pluginterfaces"):
            self.assertEqual(packages[name]["licenseDeclared"], "NOASSERTION")
            self.assertIn(
                packages[name]["SPDXID"], self.related(document, root["SPDXID"], "STATIC_LINK")
            )
        depends = self.related(document, root["SPDXID"], "DEPENDS_ON")
        for name in ("libc6", "libjack-jackd2-0", "libjack-0.125", "qml6-module-qtquick"):
            self.assertIn(packages[name]["SPDXID"], depends)
        self.assertEqual(len(self.related(document, root["SPDXID"], "CONTAINS")), len(names))

    def test_appimage_maps_copied_libraries_and_the_runtime(self):
        notices = stage_notices(self.root, SOURCE_ROOT, scanner_enabled=True, runtime_enabled=True)
        library = self.root / COPIED_LIBRARY
        library.parent.mkdir(parents=True)
        library.write_bytes(b"\x7fELF library")
        notices["copied_elfs"] = [{"path": COPIED_LIBRARY, "binary_package": COPIED_PACKAGE}]
        notices["copied_library_packages"] = {
            COPIED_PACKAGE: {
                "version": COPIED_VERSION,
                "notice_path": "usr/share/doc/oscillate/third-party/ubuntu/libexample1.copyright",
            }
        }
        document = write_sbom(
            self.root,
            package_name="oscillate",
            version=VERSION,
            form="appimage",
            notice_inventory=notices,
            nlohmann_version=NLOHMANN_VERSION,
            creation_epoch=EPOCH,
            runtime_sha256=RUNTIME_SHA256,
        )
        packages = self.by_name(document)
        copied = packages["libexample1"]
        self.assertEqual(copied["versionInfo"], COPIED_VERSION)
        library_id = next(
            item["SPDXID"]
            for item in document["files"]
            if item["fileName"] == f"./{COPIED_LIBRARY}"
        )
        self.assertEqual(self.related(document, copied["SPDXID"], "CONTAINS"), {library_id})
        runtime = packages["appimage-type2-runtime"]
        self.assertEqual(runtime["checksums"][0]["checksumValue"], RUNTIME_SHA256)
        static = self.related(document, runtime["SPDXID"], "STATIC_LINK")
        self.assertEqual(
            {name for name, package in packages.items() if package["SPDXID"] in static},
            {"libfuse", "mimalloc", "musl", "squashfuse", "zlib", "zstd"},
        )

    def test_changed_added_or_removed_files_fail_verification(self):
        self.write_deb()
        app = self.root / "usr/bin/oscillate"
        app.write_bytes(b"\x7fELF changed")
        with self.assertRaisesRegex(ValueError, "hash differs"):
            verify_sbom(self.root)
        self.write_deb_again()
        extra = self.root / "usr/share/extra.txt"
        extra.write_text("unlisted")
        with self.assertRaisesRegex(ValueError, "unlisted"):
            verify_sbom(self.root)
        extra.unlink()
        app.unlink()
        with self.assertRaisesRegex(ValueError, "absent"):
            verify_sbom(self.root)

    def write_deb_again(self):
        (self.root / "DEBIAN/control").unlink()
        (self.root / "DEBIAN").rmdir()
        self.write_deb()

    def test_structural_checks(self):
        document = self.write_deb()
        broken = json.loads(json.dumps(document))
        broken["files"][1]["SPDXID"] = broken["files"][0]["SPDXID"]
        with self.assertRaisesRegex(ValueError, "not unique"):
            check_document(broken)
        broken = json.loads(json.dumps(document))
        broken["relationships"].append(
            {
                "spdxElementId": "SPDXRef-DOCUMENT",
                "relationshipType": "DESCRIBES",
                "relatedSpdxElement": "SPDXRef-Missing",
            }
        )
        with self.assertRaisesRegex(ValueError, "unknown element"):
            check_document(broken)
        broken = json.loads(json.dumps(document))
        broken["files"][0]["checksums"] = broken["files"][0]["checksums"][1:]
        with self.assertRaisesRegex(ValueError, "incomplete"):
            check_document(broken)


if __name__ == "__main__":
    unittest.main()

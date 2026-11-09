#!/usr/bin/env python3
"""The runtime dependency inventory: generation and the check against built packages."""

import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import package_flatpak
from package_dependencies import (
    APPDIR_SIDECAR_STATUS,
    DEB_SIDECAR_STATUS,
    INVENTORY,
    depends_groups,
    encoded_inventory,
    verify,
)
from package_profiles import PROFILES
from package_sources import OFFER_JSON, OFFER_TEXT, build_offer

DEB = PROFILES["deb"]
APPIMAGE = PROFILES["appimage"]
FLATPAK = PROFILES["flatpak"]
FLATPAK_LIBRARY = "lib/libsndfile.so.1.0.37"
EXTRA_PACKAGE = "libextra1"
AMD64 = ":amd64"
COMBINED_NOTICES = "usr/share/doc/oscillate/third-party/NOTICES.txt"
PROJECT_COMMIT = "0" * 40


def deb_sidecar(groups):
    """A .deb sidecar whose Depends lists `groups`, each with a version as shlibdeps writes it."""
    depends = ", ".join(
        " | ".join(f"{name} (>= 1.0)" for name in group.split(" | ")) for group in groups
    )
    return {"status": DEB_SIDECAR_STATUS, "depends": depends}


def notice_path(name):
    return f"usr/share/doc/oscillate/third-party/ubuntu/{name}{AMD64}.copyright"


def appdir_sidecar(bundled, host):
    """An AppDir sidecar copying one library from each `bundled` package, with
    its notice staged and its source in the offer."""
    libraries = [
        {
            "kind": "shared-library",
            "package": name + AMD64,
            "source_package": f"{name}-source",
            "source_version": "1.0-1",
            "path": f"usr/lib/{name}.so",
        }
        for name in bundled
    ]
    elf_files = [{"kind": "project", "path": "usr/bin/oscillate"}, *libraries]
    exclusions = [{"package": name + AMD64, "soname": f"{name}.so"} for name in host]
    paths = [item["path"] for item in elf_files] + [notice_path(name) for name in bundled]
    paths += [COMBINED_NOTICES, str(OFFER_TEXT), str(OFFER_JSON)]
    return {
        "status": APPDIR_SIDECAR_STATUS,
        "elf_files": elf_files,
        "host_base_exclusions": exclusions,
        "payload": [{"path": path} for path in paths],
        "third_party_notices": {
            name + AMD64: {"notice_path": notice_path(name)} for name in bundled
        },
        "combined_notices": COMBINED_NOTICES,
        "source_offer": build_offer(libraries, version="1.0", project_commit=PROJECT_COMMIT),
    }


def flatpak_sidecar():
    """A Flatpak sidecar on the profile's runtime with libsndfile built in,
    its licence installed and its sources in the offer."""
    (runtime,) = FLATPAK.runtime.system
    (module,) = FLATPAK.runtime.bundled
    licence = f"{package_flatpak.LICENCE_DIR}/{module}/COPYING"
    offer = package_flatpak.build_offer(
        package_flatpak.load_manifest(), version="1.0", commit=PROJECT_COMMIT
    )
    paths = [
        "bin/oscillate",
        FLATPAK_LIBRARY,
        licence,
        str(package_flatpak.COMBINED_NOTICES),
        str(package_flatpak.OFFER_TEXT),
        str(package_flatpak.OFFER_JSON),
    ]
    return {
        "status": package_flatpak.SIDECAR_STATUS,
        "runtime": runtime,
        "payload": [{"path": path} for path in paths],
        "elf_files": [
            package_flatpak.classify_elf("bin/oscillate", None),
            package_flatpak.classify_elf(FLATPAK_LIBRARY, "libsndfile.so.1"),
        ],
        "unresolved": {},
        "licence_dir": str(package_flatpak.LICENCE_DIR),
        "combined_notices": str(package_flatpak.COMBINED_NOTICES),
        "offer_files": [str(package_flatpak.OFFER_TEXT), str(package_flatpak.OFFER_JSON)],
        "source_offer": offer,
    }


class InventoryFileTests(unittest.TestCase):
    def test_checked_in_inventory_is_generated_from_the_profiles(self):
        self.assertEqual(INVENTORY.read_text(), encoded_inventory())

    def test_inventory_lists_every_profile(self):
        profiles = json.loads(encoded_inventory())["profiles"]
        self.assertEqual(set(profiles), set(PROFILES))
        self.assertEqual(profiles["deb"]["bundled"], [])
        self.assertIn("libc6", profiles["appimage"]["system"])
        self.assertIn("libspa-0.2-modules", profiles["appimage"]["conditional"])
        self.assertEqual(profiles["deb"]["conditional"], {})
        self.assertEqual(profiles["flatpak"]["system"], ["org.kde.Platform//6.10"])
        self.assertEqual(profiles["flatpak"]["bundled"], ["libsndfile"])


class DependsParsingTests(unittest.TestCase):
    def test_versions_and_architectures_are_dropped_and_alternatives_kept(self):
        self.assertEqual(
            depends_groups("libc6 (>= 2.43), libjack-jackd2-0:amd64 (>= 1.9) | libjack-0.125"),
            ["libc6", "libjack-jackd2-0 | libjack-0.125"],
        )

    def test_empty_alternative_is_rejected(self):
        with self.assertRaises(ValueError):
            depends_groups("libc6, | libx11-6")


class VerifyTests(unittest.TestCase):
    def test_matching_deb_passes(self):
        self.assertEqual(verify(DEB, deb_sidecar(DEB.runtime.system)), [])

    def test_deb_with_an_undeclared_dependency_fails(self):
        problems = verify(DEB, deb_sidecar((*DEB.runtime.system, EXTRA_PACKAGE)))
        self.assertEqual(len(problems), 1)
        self.assertIn("not declared", problems[0])
        self.assertIn(EXTRA_PACKAGE, problems[0])

    def test_deb_missing_a_declared_dependency_fails(self):
        problems = verify(DEB, deb_sidecar(DEB.runtime.system[1:]))
        self.assertEqual(len(problems), 1)
        self.assertIn(DEB.runtime.system[0], problems[0])

    def test_matching_appdir_passes(self):
        sidecar = appdir_sidecar(APPIMAGE.runtime.bundled, APPIMAGE.runtime.system)
        self.assertEqual(verify(APPIMAGE, sidecar), [])

    def test_appdir_bundling_an_undeclared_package_fails(self):
        sidecar = appdir_sidecar(
            (*APPIMAGE.runtime.bundled, EXTRA_PACKAGE), APPIMAGE.runtime.system
        )
        problems = verify(APPIMAGE, sidecar)
        self.assertEqual(len(problems), 1)
        self.assertIn("bundled", problems[0])
        self.assertIn(EXTRA_PACKAGE, problems[0])

    def test_appdir_leaving_an_undeclared_package_to_the_host_fails(self):
        sidecar = appdir_sidecar(
            APPIMAGE.runtime.bundled, (*APPIMAGE.runtime.system, EXTRA_PACKAGE)
        )
        problems = verify(APPIMAGE, sidecar)
        self.assertEqual(len(problems), 1)
        self.assertIn("system", problems[0])

    def test_appdir_without_a_notice_for_a_bundled_package_fails(self):
        sidecar = appdir_sidecar(APPIMAGE.runtime.bundled, APPIMAGE.runtime.system)
        lame = "libmp3lame0"
        sidecar["payload"].remove({"path": notice_path(lame)})
        problems = verify(APPIMAGE, sidecar)
        self.assertEqual(len(problems), 1)
        self.assertIn(f"no staged copyright notice for {lame}", problems[0])

    def test_appdir_without_a_source_entry_for_a_bundled_package_fails(self):
        sidecar = appdir_sidecar(APPIMAGE.runtime.bundled, APPIMAGE.runtime.system)
        qt = "libqt6core6t64"
        sidecar["source_offer"]["ubuntu"] = [
            entry
            for entry in sidecar["source_offer"]["ubuntu"]
            if qt not in entry["binary_packages"]
        ]
        problems = verify(APPIMAGE, sidecar)
        self.assertEqual(len(problems), 1)
        self.assertIn(f"no source entry for {qt}", problems[0])

    def test_appdir_with_a_wrong_source_link_or_no_runtime_sources_fails(self):
        sidecar = appdir_sidecar(APPIMAGE.runtime.bundled, APPIMAGE.runtime.system)
        sidecar["source_offer"]["ubuntu"][0]["url"] = "https://example.invalid/source.dsc"
        sidecar["source_offer"]["runtime"] = []
        problems = verify(APPIMAGE, sidecar)
        self.assertEqual(len(problems), 2)
        self.assertIn("wrong link", problems[0])
        self.assertIn("runtime sources", problems[1])

    def test_appdir_without_the_offer_text_fails(self):
        sidecar = appdir_sidecar(APPIMAGE.runtime.bundled, APPIMAGE.runtime.system)
        sidecar["payload"].remove({"path": str(OFFER_TEXT)})
        problems = verify(APPIMAGE, sidecar)
        self.assertEqual(problems, [f"notices: {OFFER_TEXT} is not in the package"])

    def test_matching_flatpak_passes(self):
        self.assertEqual(verify(FLATPAK, flatpak_sidecar()), [])

    def test_flatpak_on_another_runtime_fails(self):
        sidecar = flatpak_sidecar()
        sidecar["runtime"] = "org.kde.Platform//6.9"
        problems = verify(FLATPAK, sidecar)
        self.assertEqual(len(problems), 2)
        self.assertIn("flatpak system", problems[0])

    def test_flatpak_library_no_module_owns_fails(self):
        sidecar = flatpak_sidecar()
        sidecar["elf_files"].append(
            package_flatpak.classify_elf("lib/libextra.so.1", "libextra.so.1")
        )
        problems = verify(FLATPAK, sidecar)
        self.assertIn("lib/libextra.so.1", problems[0])
        self.assertIn("not declared", problems[0])

    def test_flatpak_without_a_licence_for_a_bundled_module_fails(self):
        sidecar = flatpak_sidecar()
        sidecar["payload"] = [
            item
            for item in sidecar["payload"]
            if not item["path"].startswith(str(package_flatpak.LICENCE_DIR))
        ]
        problems = verify(FLATPAK, sidecar)
        self.assertEqual(len(problems), 1)
        self.assertIn("no licence file for libsndfile", problems[0])

    def test_flatpak_without_a_source_entry_or_offer_text_fails(self):
        sidecar = flatpak_sidecar()
        sidecar["source_offer"]["modules"] = [
            module
            for module in sidecar["source_offer"]["modules"]
            if module["module"] != "libsndfile"
        ]
        sidecar["payload"].remove({"path": str(package_flatpak.OFFER_TEXT)})
        problems = verify(FLATPAK, sidecar)
        self.assertEqual(len(problems), 2)
        self.assertIn(f"{package_flatpak.OFFER_TEXT} is not in the package", problems[0])
        self.assertIn("no source entry for libsndfile", problems[1])

    def test_flatpak_with_an_unresolved_library_fails(self):
        sidecar = flatpak_sidecar()
        sidecar["unresolved"] = {"bin/oscillate": ["libjack.so.0"]}
        self.assertEqual(
            verify(FLATPAK, sidecar), ["bin/oscillate: unresolved libraries libjack.so.0"]
        )

    def test_wrong_sidecar_for_the_profile_is_rejected(self):
        with self.assertRaises(ValueError):
            verify(APPIMAGE, deb_sidecar(DEB.runtime.system))
        with self.assertRaises(ValueError):
            verify(DEB, appdir_sidecar(APPIMAGE.runtime.bundled, APPIMAGE.runtime.system))
        with self.assertRaises(ValueError):
            verify(FLATPAK, deb_sidecar(DEB.runtime.system))


if __name__ == "__main__":
    unittest.main()

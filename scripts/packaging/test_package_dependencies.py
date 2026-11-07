#!/usr/bin/env python3
"""The runtime dependency inventory: generation and the check against built packages."""

import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from package_dependencies import (
    APPDIR_SIDECAR_STATUS,
    DEB_SIDECAR_STATUS,
    INVENTORY,
    depends_groups,
    encoded_inventory,
    verify,
)
from package_profiles import PROFILES

DEB = PROFILES["deb"]
APPIMAGE = PROFILES["appimage"]
EXTRA_PACKAGE = "libextra1"
AMD64 = ":amd64"


def deb_sidecar(groups):
    """A .deb sidecar whose Depends lists `groups`, each with a version as shlibdeps writes it."""
    depends = ", ".join(
        " | ".join(f"{name} (>= 1.0)" for name in group.split(" | ")) for group in groups
    )
    return {"status": DEB_SIDECAR_STATUS, "depends": depends}


def appdir_sidecar(bundled, host):
    elf_files = [{"kind": "project", "path": "usr/bin/oscillate"}] + [
        {"kind": "shared-library", "package": name + AMD64, "path": f"usr/lib/{name}.so"}
        for name in bundled
    ]
    exclusions = [{"package": name + AMD64, "soname": f"{name}.so"} for name in host]
    return {
        "status": APPDIR_SIDECAR_STATUS,
        "elf_files": elf_files,
        "host_base_exclusions": exclusions,
    }


class InventoryFileTests(unittest.TestCase):
    def test_checked_in_inventory_is_generated_from_the_profiles(self):
        self.assertEqual(INVENTORY.read_text(), encoded_inventory())

    def test_inventory_lists_every_profile(self):
        profiles = json.loads(encoded_inventory())["profiles"]
        self.assertEqual(set(profiles), set(PROFILES))
        self.assertEqual(profiles["deb"]["bundled"], [])
        self.assertIn("libc6", profiles["appimage"]["system"])


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

    def test_wrong_sidecar_for_the_profile_is_rejected(self):
        with self.assertRaises(ValueError):
            verify(APPIMAGE, deb_sidecar(DEB.runtime.system))
        with self.assertRaises(ValueError):
            verify(DEB, appdir_sidecar(APPIMAGE.runtime.bundled, APPIMAGE.runtime.system))


if __name__ == "__main__":
    unittest.main()

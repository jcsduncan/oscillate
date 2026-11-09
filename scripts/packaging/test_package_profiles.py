#!/usr/bin/env python3
"""The package build check: profile switches, source tree, version and inputs."""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from package_profiles import (
    APP_RELATIVE,
    HELPER_RELATIVE,
    PROFILES,
    SOURCE_ROOT,
    check_build,
    feature_readme,
    package_version,
)

PROJECT_VERSION = "0.4.2"
RELEASE_TAG = f"v{PROJECT_VERSION}"
SUFFIX = "ci.17.gabc1234"


class PackageBuildCheckTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="oscillate-package-profile-")
        self.addCleanup(self.temporary.cleanup)
        self.build = Path(self.temporary.name)
        for relative in (APP_RELATIVE, HELPER_RELATIVE):
            (self.build / relative).parent.mkdir(parents=True, exist_ok=True)
            (self.build / relative).write_bytes(b"\x7fELF")

    def write_cache(self, profile, overrides=None):
        values = dict(profile.switches)
        values["CMAKE_HOME_DIRECTORY"] = str(SOURCE_ROOT)
        values["CMAKE_PROJECT_VERSION"] = PROJECT_VERSION
        values.update(overrides or {})
        (self.build / "CMakeCache.txt").write_text(
            "\n".join(f"{key}:STRING={value}" for key, value in values.items()) + "\n"
        )

    def test_matching_build_gives_inputs_and_release_version(self):
        for profile in PROFILES.values():
            with self.subTest(profile=profile.name):
                self.write_cache(profile)
                inputs = check_build(self.build, profile, release_tag=RELEASE_TAG)
                self.assertEqual(inputs.version, PROJECT_VERSION)
                self.assertEqual(inputs.app, self.build.resolve() / APP_RELATIVE)
                self.assertEqual(inputs.helper, self.build.resolve() / HELPER_RELATIVE)
                self.assertEqual(inputs.switches, dict(sorted(profile.switches.items())))

    def test_every_profile_switch_is_enforced(self):
        for profile in PROFILES.values():
            for name, value in profile.switches.items():
                with self.subTest(profile=profile.name, switch=name):
                    self.write_cache(profile, {name: "OFF" if value in ("ON", "1") else "ON"})
                    with self.assertRaisesRegex(ValueError, name):
                        check_build(self.build, profile, version_suffix=SUFFIX)

    def test_both_packages_have_the_codec(self):
        codec = "OSCILLATE_BUILD_CODEC_ADAPTER"
        for profile in PROFILES.values():
            with self.subTest(profile=profile.name):
                self.assertEqual(profile.switches[codec], "ON")
                self.write_cache(profile, {codec: "OFF"})
                with self.assertRaisesRegex(ValueError, codec):
                    check_build(self.build, profile, version_suffix=SUFFIX)

    def test_the_appimage_bundles_the_codec_libraries(self):
        bundled = PROFILES["appimage"].runtime.bundled
        for package in ("libsndfile1", "libmp3lame0", "libmpg123-0t64", "libflac14"):
            with self.subTest(package=package):
                self.assertIn(package, bundled)

    def test_foreign_source_tree_or_missing_helper_is_rejected(self):
        profile = PROFILES["deb"]
        self.write_cache(profile, {"CMAKE_HOME_DIRECTORY": str(self.build / "elsewhere")})
        with self.assertRaisesRegex(ValueError, "different source tree"):
            check_build(self.build, profile, version_suffix=SUFFIX)
        self.write_cache(profile)
        (self.build / HELPER_RELATIVE).unlink()
        with self.assertRaisesRegex(ValueError, "lacks"):
            check_build(self.build, profile, version_suffix=SUFFIX)

    def test_versions(self):
        self.assertEqual(
            package_version(PROJECT_VERSION, None, SUFFIX), f"{PROJECT_VERSION}~{SUFFIX}"
        )
        for tag, suffix, message in (
            ("v9.9.9", None, "does not match"),
            (RELEASE_TAG, SUFFIX, "no version suffix"),
            (None, None, "needs a version suffix"),
            (None, "bad suffix", "unusable version suffix"),
        ):
            with self.subTest(tag=tag, suffix=suffix), self.assertRaisesRegex(ValueError, message):
                package_version(PROJECT_VERSION, tag, suffix)
        with self.assertRaisesRegex(ValueError, "unusable project version"):
            package_version("", RELEASE_TAG, None)

    def test_readme_lists_included_features(self):
        codec_line = "  - AIFF, FLAC and MP3 import and export (libsndfile, libmpg123)\n"
        for profile in PROFILES.values():
            with self.subTest(profile=profile.name):
                readme = feature_readme(profile, PROJECT_VERSION)
                self.assertIn("VST3 instruments and effects", readme)
                self.assertIn(codec_line, readme)
                self.assertNotIn("Left out of this package", readme)


if __name__ == "__main__":
    unittest.main()

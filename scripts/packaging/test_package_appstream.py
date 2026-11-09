#!/usr/bin/env python3
"""The metainfo's releases, generated from Release Please's CHANGELOG.md."""

import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from package_appstream import (
    CHANGELOG,
    METAINFO,
    RELEASE_LIMIT,
    Release,
    Section,
    clean_item,
    load_releases,
    metainfo_with_releases,
    parse_changelog,
    write_metainfo,
)
from package_flatpak import project_version
from package_profiles import FLATPAK_APP_ID
from package_sbom import PROJECT_URL

NEW_VERSION = "0.3.0"
NEW_DATE = "2026-11-09"
OLD_VERSION = "0.2.0"
OLD_DATE = "2026-11-08"
FEATURES = "Features"
FIXES = "Bug Fixes"
TRIM = "trim a MIDI clip from its left edge"
COPY = "Ctrl+drag a clip to copy it where you drop it"
FIT = "fit Halo's keytrack knob label"
CRASH = "local crash reports & a published known-issues page"
ISSUES = f"{PROJECT_URL}/issues"
COMMITS = f"{PROJECT_URL}/commit"
# Release Please's own format, with a compare link on one heading and not
# the other, scoped and unscoped items, and a wrapped item.
CHANGELOG_TEXT = f"""# Changelog

## [{NEW_VERSION}]({PROJECT_URL}/compare/v{OLD_VERSION}...v{NEW_VERSION}) ({NEW_DATE})


### {FEATURES}

* **app,application,engine,project:** {TRIM} ([#275]({ISSUES}/275)) ([704de78]({COMMITS}/704de78))
* {COPY} ([5a67b0e]({COMMITS}/5a67b0e)), closes [#12]({ISSUES}/12)


### {FIXES}

* **app:** fit Halo's keytrack
  knob label ([#256]({ISSUES}/256)) ([edab48b]({COMMITS}/edab48b))

## {OLD_VERSION} ({OLD_DATE})


### {FEATURES}

* **app,foundation:** {CRASH} ([#196]({ISSUES}/196)) ([6e28c3c]({COMMITS}/6e28c3c))
"""
EXPECTED = [
    Release(
        NEW_VERSION,
        NEW_DATE,
        (Section(FEATURES, (TRIM, COPY)), Section(FIXES, (FIT,))),
    ),
    Release(OLD_VERSION, OLD_DATE, (Section(FEATURES, (CRASH,)),)),
]


def changelog_of(count: int) -> str:
    """A changelog of count releases with no notes, newest first."""
    return "".join(f"## 0.{minor}.0 ({NEW_DATE})\n" for minor in reversed(range(count)))


class ParseTests(unittest.TestCase):
    def test_releases_sections_and_items_are_read_newest_first(self):
        self.assertEqual(parse_changelog(CHANGELOG_TEXT), EXPECTED)

    def test_links_references_scopes_and_markup_are_stripped(self):
        self.assertEqual(
            clean_item(f"**app:** add `Edge`, see [the docs]({PROJECT_URL}) ([abc1234](x))"),
            "add Edge, see the docs",
        )

    def test_a_heading_that_is_not_a_release_is_refused(self):
        with self.assertRaisesRegex(ValueError, "release heading"):
            parse_changelog("## Unreleased\n")

    def test_a_bad_date_is_refused(self):
        with self.assertRaisesRegex(ValueError, "bad date"):
            parse_changelog(f"## {NEW_VERSION} (2026-13-40)\n")

    def test_a_release_without_notes_has_no_sections(self):
        self.assertEqual(
            parse_changelog(f"## {NEW_VERSION} ({NEW_DATE})\n"),
            [Release(NEW_VERSION, NEW_DATE, ())],
        )


class GenerateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="oscillate-appstream-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.changelog = self.root / "CHANGELOG.md"
        self.changelog.write_text(CHANGELOG_TEXT)

    def generated(self) -> ET.Element:
        output = self.root / "out" / "metainfo.xml"
        write_metainfo(output, changelog=self.changelog)
        return ET.parse(output).getroot()

    def test_the_metainfo_gains_one_release_per_changelog_release(self):
        root = self.generated()
        self.assertEqual(root.findtext("id"), FLATPAK_APP_ID)
        releases = root.findall("releases/release")
        self.assertEqual(
            [(release.get("version"), release.get("date")) for release in releases],
            [(NEW_VERSION, NEW_DATE), (OLD_VERSION, OLD_DATE)],
        )
        newest = releases[0]
        self.assertEqual(
            newest.findtext("url[@type='details']"), f"{PROJECT_URL}/releases/tag/v{NEW_VERSION}"
        )
        description = newest.find("description")
        self.assertEqual([p.text for p in description.findall("p")], [FEATURES, FIXES])
        self.assertEqual(
            [[li.text for li in ul.findall("li")] for ul in description.findall("ul")],
            [[TRIM, COPY], [FIT]],
        )

    def test_a_release_without_notes_has_no_description(self):
        self.changelog.write_text(changelog_of(1))
        self.assertIsNone(self.generated().find("releases/release/description"))

    def test_only_the_newest_releases_are_listed(self):
        self.changelog.write_text(changelog_of(RELEASE_LIMIT + 1))
        versions = [release.version for release in load_releases(self.changelog)]
        self.assertEqual(len(versions), RELEASE_LIMIT)
        self.assertEqual(versions[0], f"0.{RELEASE_LIMIT}.0")

    def test_a_changelog_without_releases_is_refused(self):
        self.changelog.write_text("# Changelog\n")
        with self.assertRaisesRegex(ValueError, "no releases"):
            self.generated()

    def test_a_template_with_its_own_releases_is_refused(self):
        template = self.root / "template.xml"
        template.write_text("<component><releases/></component>")
        with self.assertRaisesRegex(ValueError, "its own <releases>"):
            metainfo_with_releases(EXPECTED, template)


class RepositoryTests(unittest.TestCase):
    def test_the_project_version_is_the_newest_changelog_release(self):
        # Release Please bumps CMakeLists.txt and CHANGELOG.md together, so a
        # release tag always has release notes for the metainfo.
        self.assertEqual(load_releases(CHANGELOG)[0].version, project_version())

    def test_the_repository_metainfo_generates(self):
        root = ET.fromstring(metainfo_with_releases(load_releases(CHANGELOG), METAINFO))
        self.assertTrue(root.findall("releases/release"))


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""The AppStream metainfo with its releases taken from CHANGELOG.md.

assets/packaging/io.github.jcsduncan.Oscillate.metainfo.xml holds everything
but the releases. Release Please writes CHANGELOG.md; this script turns its
newest releases into the metainfo's <releases> when a package is built:

  --output FILE   write the metainfo with the generated releases
"""

from __future__ import annotations

import argparse
import datetime
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path

# Run as `python3 -I`, which leaves the script's directory off the path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from package_profiles import FLATPAK_APP_ID, SOURCE_ROOT
from package_sbom import PROJECT_URL

CHANGELOG = SOURCE_ROOT / "CHANGELOG.md"
METAINFO = SOURCE_ROOT / "assets/packaging" / f"{FLATPAK_APP_ID}.metainfo.xml"
# The installed name AppStream tools look for.
METAINFO_NAME = METAINFO.name
# How many releases the metainfo lists, newest first.
RELEASE_LIMIT = 5
INDENT = "  "
# Release Please's headings: `## [0.3.0](compare-url) (2026-11-09)`, or
# `## 0.1.0 (2026-11-01)` without a compare link.
RELEASE_HEADING = re.compile(
    r"^## \[?(?P<version>[0-9][0-9A-Za-z.+-]*)\]?(?:\([^)]*\))? \((?P<date>[0-9-]+)\)\s*$"
)
SECTION_HEADING = re.compile(r"^### (?P<title>.+?)\s*$")
ITEM = re.compile(r"^[*-] (?P<text>.+)$")
CONTINUATION = re.compile(r"^\s+(?P<text>\S.*)$")
# A parenthesised group made only of links: Release Please's `([#275](…))`
# pull request and `([704de78](…))` commit references.
LINK_GROUP = re.compile(r"\s*\((?:\[[^\]]*\]\([^)]*\)(?:,\s*)?)+\)")
# `, closes [#12](…)` after an item.
CLOSES = re.compile(r",?\s*closes\s+(?:\[#\d+\]\([^)]*\)(?:,?\s*)?)+", re.IGNORECASE)
LINK = re.compile(r"\[(?P<text>[^\]]*)\]\([^)]*\)")
# The leading `**scope:**` of a Conventional Commit; scopes are module names.
SCOPE = re.compile(r"^\*\*[^*]+:\*\*\s*")
EMPHASIS = re.compile(r"\*\*|`")
SPACE = re.compile(r"\s+")


@dataclass(frozen=True)
class Section:
    title: str
    items: tuple[str, ...]


@dataclass(frozen=True)
class Release:
    version: str
    date: str
    sections: tuple[Section, ...]


def clean_item(text: str) -> str:
    """One changelog bullet as plain text: no links, references, scope or markup."""
    text = CLOSES.sub("", text)
    text = LINK_GROUP.sub("", text)
    text = LINK.sub(lambda match: match.group("text"), text)
    text = SCOPE.sub("", text.strip())
    return SPACE.sub(" ", EMPHASIS.sub("", text)).strip()


def parse_changelog(text: str) -> list[Release]:
    """Every release in a Release Please changelog, in the file's order (newest first)."""
    releases: list[Release] = []
    heading: re.Match[str] | None = None
    sections: list[Section] = []
    title: str | None = None
    items: list[str] = []

    def close_section() -> None:
        nonlocal title, items
        if title is not None and items:
            sections.append(Section(title, tuple(clean_item(item) for item in items)))
        title, items = None, []

    def close_release() -> None:
        nonlocal sections
        close_section()
        if heading is not None:
            version, date = heading.group("version", "date")
            try:
                datetime.date.fromisoformat(date)
            except ValueError as error:
                raise ValueError(f"release {version}: bad date {date!r}") from error
            releases.append(Release(version, date, tuple(sections)))
        sections = []

    for line in text.splitlines():
        if line.startswith("## "):
            close_release()
            heading = RELEASE_HEADING.match(line)
            if heading is None:
                raise ValueError(f"not a Release Please release heading: {line!r}")
        elif heading is None:
            continue
        elif match := SECTION_HEADING.match(line):
            close_section()
            title = match.group("title")
        elif title is not None and (match := ITEM.match(line)):
            items.append(match.group("text"))
        elif items and (match := CONTINUATION.match(line)):
            items[-1] += " " + match.group("text")
    close_release()
    return releases


def load_releases(changelog: Path = CHANGELOG, limit: int = RELEASE_LIMIT) -> list[Release]:
    """The newest releases the metainfo lists."""
    return parse_changelog(changelog.read_text(encoding="utf-8"))[:limit]


def release_element(release: Release) -> ET.Element:
    element = ET.Element("release", {"version": release.version, "date": release.date})
    url = ET.SubElement(element, "url", {"type": "details"})
    url.text = f"{PROJECT_URL}/releases/tag/v{release.version}"
    if release.sections:
        description = ET.SubElement(element, "description")
        for section in release.sections:
            ET.SubElement(description, "p").text = section.title
            bullets = ET.SubElement(description, "ul")
            for item in section.items:
                ET.SubElement(bullets, "li").text = item
    return element


def releases_element(releases: list[Release]) -> ET.Element:
    if not releases:
        raise ValueError("the changelog has no releases")
    element = ET.Element("releases")
    element.extend(release_element(release) for release in releases)
    return element


def metainfo_with_releases(releases: list[Release], template: Path = METAINFO) -> bytes:
    """The template metainfo with <releases> generated from the changelog."""
    parser = ET.XMLParser(target=ET.TreeBuilder(insert_comments=True))
    root = ET.parse(template, parser).getroot()
    if root.find("releases") is not None:
        raise ValueError(f"{template.name} has its own <releases>; they come from the changelog")
    root.append(releases_element(releases))
    ET.indent(root, space=INDENT)
    return ET.tostring(root, encoding="UTF-8", xml_declaration=True) + b"\n"


def write_metainfo(
    output: Path, *, changelog: Path = CHANGELOG, template: Path = METAINFO
) -> list[Release]:
    releases = load_releases(changelog)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(metainfo_with_releases(releases, template))
    return releases


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        releases = write_metainfo(args.output)
    except (OSError, ValueError, ET.ParseError) as error:
        parser.exit(1, f"package_appstream: {error}\n")
    print(f"{args.output}: {', '.join(release.version for release in releases)}")


if __name__ == "__main__":
    main()

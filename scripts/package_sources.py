#!/usr/bin/env python3
"""The AppImage's written source offer and its links to the exact sources.

Every Ubuntu package the AppImage copies a library or Qt plugin from is listed
under its source package and exact source version, linked to that version's
.dsc on Launchpad, which names and hashes the upstream and Ubuntu tarballs
(`dget <url>` fetches and checks them all). The AppImage runtime and the
LGPL libfuse and squashfuse code it links statically are linked to their pinned
upstream archives, and Oscillate itself to the archive of the commit it was
built from.

The offer is written to SOURCE-OFFER.txt and, machine-readable, to
third-party/sources.json:

  check-urls --appdir DIR   fail when any source link in DIR's offer is unreachable
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

# Run as `python3 -I`, which leaves the script's directory off the path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from package_profiles import TYPE2_RUNTIME_REPOSITORY, TYPE2_RUNTIME_TAG
from package_sbom import PROJECT_URL

OFFER_STATUS = "oscillate corresponding-source offer"
DOC_DIR = Path("usr/share/doc/oscillate")
OFFER_TEXT = DOC_DIR / "SOURCE-OFFER.txt"
OFFER_JSON = DOC_DIR / "third-party/sources.json"
LAUNCHPAD = "https://launchpad.net/ubuntu"
OFFER_YEARS = 3
# Source code the type-2 runtime links statically under the LGPL, and the
# runtime's own build scripts and patches. Archive hashes are the ones the
# runtime's install-dependencies script pins.
RUNTIME_SOURCES = (
    {
        "component": "appimage-type2-runtime",
        "version": TYPE2_RUNTIME_TAG,
        "url": f"{TYPE2_RUNTIME_REPOSITORY}/archive/refs/tags/{TYPE2_RUNTIME_TAG}.tar.gz",
        "note": "Runtime source, build scripts and the patch applied to libfuse's mount.c",
    },
    {
        "component": "libfuse",
        "version": "3.15.0",
        "url": "https://github.com/libfuse/libfuse/releases/download/fuse-3.15.0/fuse-3.15.0.tar.xz",
        "sha256": "70589cfd5e1cff7ccd6ac91c86c01be340b227285c5e200baa284e401eea2ca0",
        "note": "LGPL-2.1 library linked statically into the AppImage runtime",
    },
    {
        "component": "squashfuse",
        "version": "0.5.2",
        "url": "https://github.com/vasi/squashfuse/archive/0.5.2.tar.gz",
        "sha256": "db0238c5981dabbd80ee09ae15387f390091668ca060a7bc38047912491443d3",
        "note": "Linked statically into the AppImage runtime",
    },
)
SHARED_LIBRARY_KINDS = ("shared-library", "qt-plugin")
EPOCH = re.compile(r"^[0-9]+:")
ARCHITECTURE = re.compile(r":[a-z0-9]+$")
URL_ATTEMPTS = 4
URL_RETRY_SECONDS = 30
URL_TIMEOUT_SECONDS = 60
HTTP_OK = 200


def launchpad_source(source_package: str, source_version: str) -> dict[str, str]:
    """Launchpad's page and .dsc link for one exact Ubuntu source version."""
    dsc = f"{source_package}_{EPOCH.sub('', source_version)}.dsc"
    return {
        "page": f"{LAUNCHPAD}/+source/{source_package}/{source_version}",
        "url": f"{LAUNCHPAD}/+archive/primary/+sourcefiles/{source_package}/{source_version}/{dsc}",
    }


def build_offer(
    copied_elfs: list[dict[str, object]], *, version: str, project_commit: str
) -> dict[str, object]:
    """The offer for an AppDir's copied ELF records (stage-appdir.py's elf_files)."""
    ubuntu: dict[tuple[str, str], dict[str, object]] = {}
    for record in sorted(copied_elfs, key=lambda item: str(item["path"])):
        if record.get("kind") not in SHARED_LIBRARY_KINDS:
            continue
        source_package = str(record.get("source_package") or "")
        source_version = str(record.get("source_version") or "")
        if not source_package or not source_version:
            raise ValueError(f"no source package recorded for {record['path']}")
        entry = ubuntu.setdefault(
            (source_package, source_version),
            {
                "source_package": source_package,
                "source_version": source_version,
                **launchpad_source(source_package, source_version),
                "binary_packages": [],
                "files": [],
            },
        )
        binary = ARCHITECTURE.sub("", str(record["package"]))
        if binary not in entry["binary_packages"]:
            entry["binary_packages"].append(binary)
        entry["files"].append(str(record["path"]))
    return {
        "status": OFFER_STATUS,
        "version": version,
        "project": {
            "component": "oscillate",
            "version": version,
            "commit": project_commit,
            "url": f"{PROJECT_URL}/archive/{project_commit}.tar.gz",
        },
        "ubuntu": [
            {**entry, "binary_packages": sorted(entry["binary_packages"])}
            for _, entry in sorted(ubuntu.items())
        ],
        "runtime": [dict(item) for item in RUNTIME_SOURCES],
    }


def offer_text(offer: dict[str, object]) -> str:
    """The human-readable written offer."""
    version = offer["version"]
    project = offer["project"]
    lines = [
        f"Oscillate {version} AppImage: written offer of corresponding source",
        "",
        "Oscillate is free software under the GNU General Public License, version 3",
        "or later. This AppImage also carries libraries and runtime code from other",
        "projects, several under the GNU LGPL or GPL. Each is listed below with the",
        "exact source it was built from; their licence texts are in",
        "third-party/NOTICES.txt.",
        "",
        f"For at least {OFFER_YEARS} years after the Oscillate project last distributes",
        "this release, anyone who holds a copy of it may ask the Oscillate project,",
        f"through {PROJECT_URL}/issues, for the complete",
        "corresponding source code of every component below, on a medium customarily",
        "used for software interchange, for a charge no more than the cost of",
        "physically performing the source distribution. The same sources can be",
        "downloaded at no charge from the links below.",
        "",
        "To run Oscillate with a modified library, extract the image with",
        "--appimage-extract, replace the library in squashfs-root/usr/lib/ and start",
        "squashfs-root/AppRun. The runtime and libfuse sources below rebuild the",
        "AppImage runtime, and appimagetool repacks squashfs-root with it.",
        "",
        "Oscillate",
        f"  {project['component']} {project['version']} (commit {project['commit']})",
        f"    {project['url']}",
        "",
        "Ubuntu 26.04 source packages (fetch and verify with: dget <.dsc link>)",
    ]
    for entry in offer["ubuntu"]:
        lines += [
            f"  {entry['source_package']} {entry['source_version']}",
            f"    binary packages: {', '.join(entry['binary_packages'])}",
            f"    .dsc: {entry['url']}",
            f"    page: {entry['page']}",
        ]
    lines += ["", "AppImage runtime"]
    for entry in offer["runtime"]:
        lines += [
            f"  {entry['component']} {entry['version']}: {entry['note']}",
            f"    {entry['url']}",
        ]
        if "sha256" in entry:
            lines.append(f"    SHA-256 {entry['sha256']}")
    return "\n".join(lines) + "\n"


def write_offer(stage_root: Path, offer: dict[str, object]) -> None:
    for relative, text in (
        (OFFER_TEXT, offer_text(offer)),
        (OFFER_JSON, json.dumps(offer, indent=2, sort_keys=True) + "\n"),
    ):
        path = stage_root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        path.chmod(0o644)


def offer_problems(offer: dict[str, object], bundled_packages: set[str]) -> list[str]:
    """Bundled packages the offer leaves out, and entries whose links are wrong."""
    if offer.get("status") != OFFER_STATUS:
        return ["the source offer is missing or not a source offer"]
    problems = []
    listed: set[str] = set()
    for entry in offer.get("ubuntu", []):
        expected = launchpad_source(str(entry["source_package"]), str(entry["source_version"]))
        if entry.get("url") != expected["url"] or entry.get("page") != expected["page"]:
            problems.append(f"source offer: wrong link for {entry['source_package']}")
        listed.update(entry["binary_packages"])
    missing = sorted(bundled_packages - listed)
    if missing:
        problems.append(f"source offer: no source entry for {', '.join(missing)}")
    extra = sorted(listed - bundled_packages)
    if extra:
        problems.append(f"source offer: entries for packages not bundled: {', '.join(extra)}")
    if offer.get("runtime") != [dict(item) for item in RUNTIME_SOURCES]:
        problems.append("source offer: the AppImage runtime sources differ from the pinned list")
    project = offer.get("project", {})
    if project.get("url") != f"{PROJECT_URL}/archive/{project.get('commit')}.tar.gz":
        problems.append("source offer: no link to Oscillate's own source")
    return problems


def verify_offer(stage_root: Path, bundled_packages: set[str]) -> dict[str, object]:
    """Check a staged or extracted offer: complete, and its text matches its data."""
    offer = json.loads((stage_root / OFFER_JSON).read_text(encoding="utf-8"))
    problems = offer_problems(offer, bundled_packages)
    text = stage_root / OFFER_TEXT
    if not text.is_file() or text.read_text(encoding="utf-8") != offer_text(offer):
        problems.append("SOURCE-OFFER.txt does not match sources.json")
    if problems:
        raise ValueError("; ".join(problems))
    return offer


def offer_urls(offer: dict[str, object]) -> list[str]:
    return [
        offer["project"]["url"],
        *(entry["url"] for entry in offer["ubuntu"]),
        *(entry["url"] for entry in offer["runtime"]),
    ]


def reachable(url: str) -> bool:
    request = urllib.request.Request(url, method="HEAD")
    for attempt in range(URL_ATTEMPTS):
        try:
            with urllib.request.urlopen(request, timeout=URL_TIMEOUT_SECONDS) as response:
                return response.status == HTTP_OK
        except urllib.error.HTTPError as error:
            if error.code < 500:
                return False
        except (urllib.error.URLError, TimeoutError):
            pass
        if attempt + 1 < URL_ATTEMPTS:
            time.sleep(URL_RETRY_SECONDS * (attempt + 1))
    return False


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("check-urls", help="fail when a source link is unreachable")
    check.add_argument("--appdir", type=Path, required=True)
    args = parser.parse_args()
    offer = json.loads((args.appdir / OFFER_JSON).read_text(encoding="utf-8"))
    broken = [url for url in offer_urls(offer) if not reachable(url)]
    if broken:
        parser.exit(1, "unreachable source links:\n" + "\n".join(broken) + "\n")
    print(f"{len(offer_urls(offer))} source links reachable")


if __name__ == "__main__":
    main()

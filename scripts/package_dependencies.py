#!/usr/bin/env python3
"""Runtime dependency inventory of the release packages.

The inventory (docs/dependencies/runtime-dependencies.json) lists, for each
package profile in package_profiles.py, the Ubuntu packages it needs on the
host and the ones it copies in; for the Flatpak, the runtime and the
manifest modules whose libraries it installs. It is generated from the
profiles:

  write                   rewrite the inventory from the profiles
  check                   fail when the checked-in inventory is out of date
  verify --profile P --inventory SIDECAR
                          fail when a built package's sidecar (the .deb's,
                          the AppDir's or the Flatpak's inventory) differs
                          from profile P, or when a bundled package has no
                          notice or no entry in the package's source offer
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# Run as `python3 -I`, which leaves the script's directory off the path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import package_flatpak
from package_profiles import PROFILES, SOURCE_ROOT, Profile
from package_sources import OFFER_JSON, OFFER_TEXT, offer_problems

INVENTORY = SOURCE_ROOT / "docs/dependencies/runtime-dependencies.json"
INVENTORY_STATUS = "oscillate runtime dependencies"
DEB_SIDECAR_STATUS = "oscillate .deb inventory"
APPDIR_SIDECAR_STATUS = "oscillate AppDir inventory"
# The sidecar that records each profile's package contents.
SIDECAR_STATUS = {
    "deb": DEB_SIDECAR_STATUS,
    "appimage": APPDIR_SIDECAR_STATUS,
    "flatpak": package_flatpak.SIDECAR_STATUS,
}
# Ubuntu's Depends syntax: "name (>= version)" with an optional ":arch".
VERSION_CONSTRAINT = re.compile(r"\s*\([^)]*\)")
ARCHITECTURE = re.compile(r":[a-z0-9]+$")
PROJECT_ELF = "project"
REGENERATE_HINT = (
    "update the profile in scripts/package_profiles.py, then run "
    "python3 -I scripts/package_dependencies.py write"
)


def inventory() -> dict:
    """The inventory, generated from the package profiles."""
    return {
        "status": INVENTORY_STATUS,
        "generated_from": "scripts/package_profiles.py",
        "profiles": {
            name: {
                "platform": profile.platform,
                "preset": profile.preset,
                "system": sorted(profile.runtime.system),
                "bundled": sorted(profile.runtime.bundled),
                "conditional": dict(sorted(profile.runtime.conditional.items())),
            }
            for name, profile in sorted(PROFILES.items())
        },
    }


def encoded_inventory() -> str:
    """The inventory file's exact text."""
    return json.dumps(inventory(), indent=2, sort_keys=True) + "\n"


def package_name(text: str) -> str:
    """A package name without its version constraint or architecture."""
    return ARCHITECTURE.sub("", VERSION_CONSTRAINT.sub("", text).strip())


def depends_groups(depends: str) -> list[str]:
    """A Depends field as package names, alternatives joined by " | "."""
    groups = []
    for group in depends.split(","):
        names = [package_name(alternative) for alternative in group.split("|")]
        if any(not name for name in names):
            raise ValueError(f"unreadable Depends entry: {group.strip()!r}")
        groups.append(" | ".join(names))
    return groups


def observed(profile: Profile, sidecar: dict) -> tuple[list[str], list[str]]:
    """The (system, bundled) packages a built package's sidecar records."""
    expected = SIDECAR_STATUS[profile.name]
    if sidecar.get("status") != expected:
        raise ValueError(f"the {profile.name} profile is checked against the {expected!r} sidecar")
    if expected == DEB_SIDECAR_STATUS:
        return depends_groups(str(sidecar["depends"])), []
    if expected == package_flatpak.SIDECAR_STATUS:
        return package_flatpak.observed(sidecar)
    bundled = {
        package_name(str(record["package"]))
        for record in sidecar["elf_files"]
        if record["kind"] != PROJECT_ELF
    }
    system = {package_name(str(record["package"])) for record in sidecar["host_base_exclusions"]}
    return sorted(system), sorted(bundled)


def differences(label: str, declared: tuple[str, ...], found: list[str]) -> list[str]:
    """Packages declared but not found, and found but not declared."""
    problems = []
    missing = sorted(set(declared) - set(found))
    unexpected = sorted(set(found) - set(declared))
    if missing:
        problems.append(f"{label}: declared but not in the package: {', '.join(missing)}")
    if unexpected:
        problems.append(f"{label}: in the package but not declared: {', '.join(unexpected)}")
    return problems


def redistribution_problems(sidecar: dict, bundled: list[str]) -> list[str]:
    """Bundled packages without a staged notice or a source-offer entry."""
    payload = {str(item["path"]) for item in sidecar["payload"]}
    notices = {
        package_name(package): str(evidence["notice_path"])
        for package, evidence in sidecar["third_party_notices"].items()
    }
    problems = [
        f"notices: no staged copyright notice for {package}"
        for package in bundled
        if notices.get(package) not in payload
    ]
    problems += [
        f"notices: {path} is not in the package"
        for path in (sidecar["combined_notices"], str(OFFER_TEXT), str(OFFER_JSON))
        if path not in payload
    ]
    return problems + offer_problems(sidecar["source_offer"], set(bundled))


def verify(profile: Profile, sidecar: dict) -> list[str]:
    """Differences from the profile's dependencies, and bundled packages left
    out of the AppDir's or the Flatpak's notices or source offer."""
    system, bundled = observed(profile, sidecar)
    problems = differences(f"{profile.name} system", profile.runtime.system, system)
    problems += differences(f"{profile.name} bundled", profile.runtime.bundled, bundled)
    if sidecar["status"] == APPDIR_SIDECAR_STATUS:
        problems += redistribution_problems(sidecar, bundled)
    elif sidecar["status"] == package_flatpak.SIDECAR_STATUS:
        problems += package_flatpak.redistribution_problems(sidecar, bundled)
    return problems


def main() -> None:
    """Command line: write, check or verify."""
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("write", help="rewrite the inventory from the profiles")
    commands.add_parser("check", help="fail when the inventory is out of date")
    verify_command = commands.add_parser("verify", help="check a built package's sidecar")
    verify_command.add_argument("--profile", choices=sorted(PROFILES), required=True)
    verify_command.add_argument("--inventory", type=Path, required=True)
    args = parser.parse_args()

    if args.command == "write":
        INVENTORY.write_text(encoded_inventory())
        print(INVENTORY.relative_to(SOURCE_ROOT))
        return
    if args.command == "check":
        current = INVENTORY.read_text() if INVENTORY.is_file() else ""
        if current != encoded_inventory():
            parser.exit(
                1, f"{INVENTORY.relative_to(SOURCE_ROOT)} is out of date; {REGENERATE_HINT}\n"
            )
        return
    try:
        problems = verify(PROFILES[args.profile], json.loads(args.inventory.read_text()))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"cannot read {args.inventory}: {error}\n")
    if problems:
        parser.exit(1, "\n".join(problems) + f"\nIf the change is intended, {REGENERATE_HINT}\n")
    print(f"{args.profile}: runtime dependencies match the profile")


if __name__ == "__main__":
    main()

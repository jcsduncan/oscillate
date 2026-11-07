#!/usr/bin/env python3
"""Runtime dependency inventory of the release packages.

The inventory (docs/dependencies/runtime-dependencies.json) lists, for each
package profile in package_profiles.py, the Ubuntu packages it needs on the
host and the ones it copies in. It is generated from the profiles:

  write                   rewrite the inventory from the profiles
  check                   fail when the checked-in inventory is out of date
  verify --profile P --inventory SIDECAR
                          fail when a built package's sidecar (the .deb's
                          inventory or the AppDir's) differs from profile P
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# Run as `python3 -I`, which leaves the script's directory off the path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from package_profiles import PROFILES, SOURCE_ROOT, Profile

INVENTORY = SOURCE_ROOT / "docs/dependencies/runtime-dependencies.json"
INVENTORY_STATUS = "oscillate runtime dependencies"
DEB_SIDECAR_STATUS = "oscillate .deb inventory"
APPDIR_SIDECAR_STATUS = "oscillate AppDir inventory"
# The sidecar that records each profile's package contents.
SIDECAR_STATUS = {"deb": DEB_SIDECAR_STATUS, "appimage": APPDIR_SIDECAR_STATUS}
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
        "distribution": "Ubuntu 26.04 amd64",
        "profiles": {
            name: {
                "preset": profile.preset,
                "system": sorted(profile.runtime.system),
                "bundled": sorted(profile.runtime.bundled),
            }
            for name, profile in sorted(PROFILES.items())
        },
    }


def encoded_inventory() -> str:
    return json.dumps(inventory(), indent=2, sort_keys=True) + "\n"


def package_name(text: str) -> str:
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
    bundled = {
        package_name(str(record["package"]))
        for record in sidecar["elf_files"]
        if record["kind"] != PROJECT_ELF
    }
    system = {package_name(str(record["package"])) for record in sidecar["host_base_exclusions"]}
    return sorted(system), sorted(bundled)


def differences(label: str, declared: tuple[str, ...], found: list[str]) -> list[str]:
    problems = []
    missing = sorted(set(declared) - set(found))
    unexpected = sorted(set(found) - set(declared))
    if missing:
        problems.append(f"{label}: declared but not in the package: {', '.join(missing)}")
    if unexpected:
        problems.append(f"{label}: in the package but not declared: {', '.join(unexpected)}")
    return problems


def verify(profile: Profile, sidecar: dict) -> list[str]:
    """Every difference between the profile's dependencies and the sidecar's."""
    system, bundled = observed(profile, sidecar)
    return differences(f"{profile.name} system", profile.runtime.system, system) + differences(
        f"{profile.name} bundled", profile.runtime.bundled, bundled
    )


def main() -> None:
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

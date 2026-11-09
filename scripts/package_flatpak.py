#!/usr/bin/env python3
"""The Flatpak: its manifest, the documents it ships and its inventory.

packaging/flatpak/io.github.jcsduncan.Oscillate.json is the manifest. It
builds Oscillate from the source tree it sits in, so a local
`flatpak-builder` run works from a checkout; the commands below write the
concrete forms (docs/dependencies/flatpak.md):

  manifest --output FILE --commit SHA (--release-tag TAG | --version-suffix S)
           (--source-dir DIR | --git-commit)
                  a manifest that builds DIR, or the commit from GitHub, with
                  the patches it applies copied beside it
  flathub --output-dir DIR --tag TAG --commit SHA
                  the Flathub submission: the manifest building the tagged
                  commit, its patches and flathub.json
  stage --build-dir DIR --prefix /app --commit SHA (--release-tag | --version-suffix)
                  run inside the Flatpak build: the feature list, notices and
                  source offer under /app/share/doc/oscillate
  inventory --build-dir DIR --manifest FILE --output FILE
                  the sidecar of a finished flatpak-builder directory, which
                  package_dependencies.py verify checks against the profile
"""

from __future__ import annotations

import argparse
import configparser
import copy
import hashlib
import json
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# Run as `python3 -I`, which leaves the script's directory off the path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from package_notices import stage_notices
from package_profiles import (
    FLATPAK_APP_ID,
    FLATPAK_LIBRARY_MODULES,
    FLATPAK_RUNTIME_REF,
    PROFILES,
    SOURCE_ROOT,
    check_build,
    feature_readme,
    package_version,
)
from package_sbom import PROJECT_URL
from package_sources import OFFER_STATUS

PROFILE = PROFILES["flatpak"]
MANIFEST_DIR = Path("packaging/flatpak")
MANIFEST = SOURCE_ROOT / MANIFEST_DIR / f"{FLATPAK_APP_ID}.json"
METAINFO = SOURCE_ROOT / "assets/packaging" / f"{FLATPAK_APP_ID}.metainfo.xml"
PATCH_DIR = "patches"
PROJECT_MODULE = "oscillate"
# Flathub builds every architecture a manifest does not exclude; the package
# profiles and the AppImage are x86-64 only.
FLATHUB_JSON = {"only-arches": ["x86_64"]}
# The stage step's fixed arguments; the commit and version are appended.
STAGE_COMMAND = (
    "python3 -I scripts/package_flatpak.py stage --build-dir build/package-flatpak "
    "--prefix ${FLATPAK_DEST}"
)
# What the manifest in the source tree passes: a local build has no commit.
LOCAL_COMMIT = "local"
LOCAL_VERSION_ARGS = ("--version-suffix", "local")
# Where the package's documents go under /app.
DOC_DIR = Path("share/doc/oscillate")
THIRD_PARTY_DIR = DOC_DIR / "third-party"
OFFER_TEXT = DOC_DIR / "SOURCE-OFFER.txt"
OFFER_JSON = THIRD_PARTY_DIR / "sources.json"
COMBINED_NOTICES = THIRD_PARTY_DIR / "NOTICES.txt"
# flatpak-builder installs each module's licence files here.
LICENCE_DIR = Path("share/licenses") / FLATPAK_APP_ID
SIDECAR_STATUS = "oscillate Flatpak inventory"
PROJECT_EXECUTABLES = ("bin/oscillate", "bin/oscillate_vst3_scan_helper")
PROJECT_ELF = "project"
LIBRARY_ELF = "shared-library"
OTHER_ELF = "other"
ELF_MAGIC = b"\x7fELF"
# flatpak-builder's split debug info, exported as the app's .Debug extension.
DEBUG_DIR = Path("lib/debug")
READ_CHUNK_BYTES = 1 << 20
FILE_MODE = 0o644
# Printed between the SONAME and the ldd output of one ELF in the sandbox.
ELF_REPORT_SEPARATOR = "--- ldd"
ELF_REPORT_SCRIPT = (
    "readelf -d \"$1\" | sed -n 's/.*Library soname: \\[\\(.*\\)\\]/\\1/p'; "
    f"echo '{ELF_REPORT_SEPARATOR}'; ldd \"$1\""
)
LDD_LINE = re.compile(r"^\s*(\S+)\s+=>\s+(.+?)(?:\s+\(0x[0-9a-f]+\))?$")
NOT_FOUND = "not found"
# The field that fixes each kind of offered source to exact contents.
PINNED_BY = {"archive": "sha256", "git": "commit", "patch": "path"}
PROJECT_VERSION = re.compile(r"^project\(oscillate VERSION ([0-9][0-9A-Za-z.]*)", re.MULTILINE)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(READ_CHUNK_BYTES), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_manifest(path: Path = MANIFEST) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def project_module(manifest: dict) -> dict:
    """The module that builds Oscillate itself."""
    modules = [module for module in manifest["modules"] if module["name"] == PROJECT_MODULE]
    if len(modules) != 1:
        raise ValueError(f"the manifest needs exactly one {PROJECT_MODULE!r} module")
    return modules[0]


def stage_command(commit: str, version_args: tuple[str, ...]) -> str:
    return " ".join((STAGE_COMMAND, "--commit", commit, *version_args))


def version_args(release_tag: str | None, version_suffix: str | None) -> tuple[str, ...]:
    if (release_tag is None) == (version_suffix is None):
        raise ValueError("give exactly one of a release tag and a version suffix")
    return ("--release-tag", release_tag) if release_tag else ("--version-suffix", version_suffix)


def concrete_manifest(
    template: dict, *, project_source: dict, commit: str, versions: tuple[str, ...]
) -> dict:
    """The manifest with Oscillate's source and the stage step's commit and version set."""
    manifest = copy.deepcopy(template)
    module = project_module(manifest)
    sources = module["sources"]
    if not sources or sources[0].get("type") != "dir":
        raise ValueError(f"the {PROJECT_MODULE} module's first source must be the source tree")
    commands = module["build-commands"]
    if not commands or not commands[-1].startswith(STAGE_COMMAND):
        raise ValueError(f"the {PROJECT_MODULE} module must end with the stage step")
    sources[0] = project_source
    commands[-1] = stage_command(commit, versions)
    return manifest


def patch_paths(manifest: dict) -> list[str]:
    """Every patch file the manifest applies, relative to the manifest."""
    return sorted(
        source["path"]
        for module in manifest["modules"]
        for source in module.get("sources", [])
        if source.get("type") == "patch"
    )


def write_manifest(manifest: dict, output: Path) -> None:
    """Write the manifest and copy the patches it names beside it."""
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    for relative in patch_paths(manifest):
        destination = output.parent / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(MANIFEST.parent / relative, destination)


def git_source(commit: str, tag: str | None = None) -> dict:
    source = {"type": "git", "url": f"{PROJECT_URL}.git"}
    if tag is not None:
        source["tag"] = tag
    source["commit"] = commit
    return source


def directory_source(template: dict, directory: Path) -> dict:
    source = dict(project_module(template)["sources"][0])
    source["path"] = str(directory.resolve())
    return source


def metainfo_versions(path: Path = METAINFO) -> list[str]:
    return [release.get("version", "") for release in ET.parse(path).iter("release")]


def project_version(cmakelists: Path = SOURCE_ROOT / "CMakeLists.txt") -> str:
    match = PROJECT_VERSION.search(cmakelists.read_text(encoding="utf-8"))
    if match is None:
        raise ValueError(f"no project version in {cmakelists}")
    return match.group(1)


def write_flathub(
    output_dir: Path,
    *,
    tag: str,
    commit: str,
    project: str | None = None,
    metainfo: Path = METAINFO,
) -> Path:
    """The Flathub submission for one release tag of the project version
    (by default the one CMakeLists.txt declares)."""
    version = package_version(project or project_version(), tag, None)
    if version not in metainfo_versions(metainfo):
        raise ValueError(
            f'{metainfo.name} has no <release version="{version}">; '
            "add the release notes before submitting"
        )
    manifest = concrete_manifest(
        load_manifest(),
        project_source=git_source(commit, tag),
        commit=commit,
        versions=version_args(tag, None),
    )
    output = output_dir / MANIFEST.name
    write_manifest(manifest, output)
    (output_dir / "flathub.json").write_text(json.dumps(FLATHUB_JSON, indent=2) + "\n")
    return output


# ---- The source offer --------------------------------------------------------------------


def offered_source(source: dict, commit: str) -> dict:
    """One manifest source as the offer lists it."""
    kind = source["type"]
    if kind == "archive":
        return {"type": kind, "url": source["url"], "sha256": source["sha256"]}
    if kind == "git":
        entry = {"type": kind, "url": source["url"], "commit": source["commit"]}
        if "tag" in source:
            entry["tag"] = source["tag"]
        return entry
    if kind == "patch":
        path = MANIFEST_DIR / source["path"]
        return {"type": kind, "path": str(path), "url": f"{PROJECT_URL}/blob/{commit}/{path}"}
    raise ValueError(f"the source offer cannot describe a {kind!r} source")


def build_offer(manifest: dict, *, version: str, commit: str) -> dict:
    """The offer for every module the manifest builds into the Flatpak."""
    modules = []
    for module in manifest["modules"]:
        sources = [
            offered_source(source, commit)
            for source in module.get("sources", [])
            if source["type"] != "dir"
        ]
        if sources:
            modules.append({"module": module["name"], "sources": sources})
    return {
        "status": OFFER_STATUS,
        "form": PROFILE.name,
        "version": version,
        "project": {
            "component": PROJECT_MODULE,
            "version": version,
            "commit": commit,
            "url": f"{PROJECT_URL}/archive/{commit}.tar.gz",
        },
        "modules": modules,
        "runtime": FLATPAK_RUNTIME_REF,
    }


def offer_text(offer: dict) -> str:
    version = offer["version"]
    project = offer["project"]
    lines = [
        f"Oscillate {version} Flatpak: written offer of corresponding source",
        "",
        "Oscillate is free software under the GNU General Public License, version 3",
        "or later. This Flatpak also builds code from the projects listed below, some",
        "under the GNU LGPL; each is listed with the exact source it was built from.",
        "Their licence texts are in third-party/NOTICES.txt.",
        "",
        "For at least three years after the Oscillate project last distributes this",
        "release, anyone who holds a copy of it may ask the Oscillate project,",
        f"through {PROJECT_URL}/issues, for the complete",
        "corresponding source code of every component below, on a medium customarily",
        "used for software interchange, for a charge no more than the cost of",
        "physically performing the source distribution. The same sources can be",
        "downloaded at no charge from the links below.",
        "",
        "To run Oscillate with a modified library, build it and start the app with",
        "the folder holding it first on the library path:",
        f"  flatpak run --filesystem=DIR:ro --env=LD_LIBRARY_PATH=DIR {FLATPAK_APP_ID}",
        "",
        f"The {offer['runtime']} runtime and the libraries Oscillate loads from it",
        "are not part of this package; Flathub distributes them with their sources.",
        "",
        "Oscillate",
        f"  {project['component']} {project['version']} (commit {project['commit']})",
        f"    {project['url']}",
    ]
    for module in offer["modules"]:
        lines += ["", f"Module {module['module']}"]
        for source in module["sources"]:
            lines.append(f"  {source['type']}: {source['url']}")
            if "sha256" in source:
                lines.append(f"    SHA-256 {source['sha256']}")
            if "commit" in source:
                lines.append(f"    commit {source['commit']}")
    return "\n".join(lines) + "\n"


def offer_problems(offer: dict, bundled_modules: set[str]) -> list[str]:
    """Bundled modules the offer leaves out, and sources it cannot pin."""
    if offer.get("status") != OFFER_STATUS or offer.get("form") != PROFILE.name:
        return ["the source offer is missing or not the Flatpak's"]
    problems = []
    listed = set()
    for module in offer.get("modules", []):
        listed.add(module["module"])
        for source in module["sources"]:
            pin = PINNED_BY.get(source.get("type"))
            if not source.get("url") or pin is None or not source.get(pin):
                problems.append(f"source offer: an unpinned source in {module['module']}")
    missing = sorted(bundled_modules - listed)
    if missing:
        problems.append(f"source offer: no source entry for {', '.join(missing)}")
    project = offer.get("project", {})
    if project.get("url") != f"{PROJECT_URL}/archive/{project.get('commit')}.tar.gz":
        problems.append("source offer: no link to Oscillate's own source")
    return problems


# ---- Staging inside the Flatpak build ----------------------------------------------------


def write_document(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    path.chmod(FILE_MODE)


def stage(
    build_dir: Path,
    prefix: Path,
    *,
    commit: str,
    release_tag: str | None,
    version_suffix: str | None,
) -> dict:
    """Write the feature list, notices and source offer under prefix."""
    inputs = check_build(build_dir, PROFILE, release_tag=release_tag, version_suffix=version_suffix)
    for executable in PROJECT_EXECUTABLES:
        if not (prefix / executable).is_file():
            raise ValueError(f"{prefix / executable} is not installed")
    write_document(prefix / DOC_DIR / "README", feature_readme(PROFILE, inputs.version))
    licences = {
        module: sorted(path for path in (prefix / LICENCE_DIR / module).glob("*") if path.is_file())
        for module in PROFILE.runtime.bundled
    }
    stage_notices(
        prefix,
        SOURCE_ROOT,
        scanner_enabled=True,
        copied_elfs=[],
        bundled_licences=licences,
        third_party_dir=THIRD_PARTY_DIR,
    )
    offer = build_offer(load_manifest(), version=inputs.version, commit=commit)
    write_document(prefix / OFFER_JSON, json.dumps(offer, indent=2, sort_keys=True) + "\n")
    write_document(prefix / OFFER_TEXT, offer_text(offer))
    return offer


# ---- The inventory of a finished build ---------------------------------------------------


def parse_elf_report(report: str) -> tuple[str | None, dict[str, str]]:
    """The SONAME and ldd resolution printed by ELF_REPORT_SCRIPT."""
    soname_part, _, ldd_part = report.partition(ELF_REPORT_SEPARATOR)
    soname = soname_part.strip() or None
    resolved = {}
    for line in ldd_part.splitlines():
        match = LDD_LINE.match(line)
        if match:
            resolved[match.group(1)] = match.group(2).strip()
    return soname, resolved


def classify_elf(path: str, soname: str | None) -> dict:
    """One ELF under /app: Oscillate's own, a manifest module's library, or neither."""
    if path in PROJECT_EXECUTABLES:
        return {"path": path, "kind": PROJECT_ELF}
    module = FLATPAK_LIBRARY_MODULES.get(soname or "")
    if path.startswith("lib/") and module is not None:
        return {"path": path, "kind": LIBRARY_ELF, "soname": soname, "module": module}
    return {"path": path, "kind": OTHER_ELF, "soname": soname}


def shipped_files(files: Path) -> list[Path]:
    """Every path under /app the app ships; the split debug info goes to the
    separate .Debug extension."""
    return [
        path
        for path in sorted(files.rglob("*"))
        if DEBUG_DIR not in path.relative_to(files).parents
    ]


def payload(files: Path) -> list[dict]:
    entries = []
    for path in shipped_files(files):
        relative = str(path.relative_to(files))
        if path.is_symlink():
            entries.append({"path": relative, "symlink": str(path.readlink())})
        elif path.is_file():
            entries.append(
                {"path": relative, "sha256": sha256(path), "mode": oct(path.stat().st_mode & 0o777)}
            )
    return entries


def is_elf(path: Path) -> bool:
    if path.is_symlink() or not path.is_file():
        return False
    with path.open("rb") as file:
        return file.read(len(ELF_MAGIC)) == ELF_MAGIC


def elf_report(build_dir: Path, manifest: Path, relative: str) -> str:
    """SONAME and library resolution of one ELF, run in the manifest's SDK
    with the build's /app tree mounted read-only and first on the library path."""
    declared = load_manifest(manifest)
    files = (build_dir / "files").resolve()
    command = [
        "flatpak",
        "run",
        f"--filesystem={files}:ro",
        f"--env=LD_LIBRARY_PATH={files}/lib",
        "--command=sh",
        f"{declared['sdk']}//{declared['runtime-version']}",
        "-c",
        ELF_REPORT_SCRIPT,
        "sh",
        str(files / relative),
    ]
    result = subprocess.run(command, check=False, capture_output=True, text=True)
    if result.returncode != 0:
        raise ValueError(f"cannot inspect {relative}: {result.stderr.strip()}")
    return result.stdout


def runtime_ref(metadata: str) -> str:
    """The runtime as NAME//BRANCH from the build's metadata file."""
    parser = configparser.ConfigParser(interpolation=None)
    parser.read_string(metadata)
    name, _arch, branch = parser["Application"]["runtime"].split("/")
    return f"{name}//{branch}"


def inventory(build_dir: Path, manifest: Path, report=elf_report) -> dict:
    files = build_dir / "files"
    elf_files = []
    unresolved = {}
    for path in shipped_files(files):
        if not is_elf(path):
            continue
        relative = str(path.relative_to(files))
        soname, resolved = parse_elf_report(report(build_dir, manifest, relative))
        elf_files.append(classify_elf(relative, soname))
        missing = sorted(name for name, target in resolved.items() if target == NOT_FOUND)
        if missing:
            unresolved[relative] = missing
    offer = json.loads((files / OFFER_JSON).read_text(encoding="utf-8"))
    return {
        "status": SIDECAR_STATUS,
        "app_id": FLATPAK_APP_ID,
        "version": offer["version"],
        "runtime": runtime_ref((build_dir / "metadata").read_text(encoding="utf-8")),
        "payload": payload(files),
        "elf_files": elf_files,
        "unresolved": unresolved,
        "licence_dir": str(LICENCE_DIR),
        "combined_notices": str(COMBINED_NOTICES),
        "offer_files": [str(OFFER_TEXT), str(OFFER_JSON)],
        "source_offer": offer,
    }


def observed(sidecar: dict) -> tuple[list[str], list[str]]:
    """The (runtime, bundled modules) a Flatpak sidecar records; an ELF that no
    module owns shows up as an undeclared bundled entry."""
    bundled = set()
    for record in sidecar["elf_files"]:
        if record["kind"] == LIBRARY_ELF:
            bundled.add(record["module"])
        elif record["kind"] != PROJECT_ELF:
            bundled.add(f"unowned {record['path']}")
    return [sidecar["runtime"]], sorted(bundled)


def redistribution_problems(sidecar: dict, bundled: list[str]) -> list[str]:
    """Bundled modules without a licence or a source-offer entry, missing
    documents, and libraries the runtime does not provide."""
    paths = {item["path"] for item in sidecar["payload"]}
    licence_dir = sidecar["licence_dir"]
    problems = [
        f"notices: no licence file for {module} under {licence_dir}"
        for module in bundled
        if not any(path.startswith(f"{licence_dir}/{module}/") for path in paths)
    ]
    problems += [
        f"notices: {path} is not in the package"
        for path in (sidecar["combined_notices"], *sidecar["offer_files"])
        if path not in paths
    ]
    problems += [
        f"{path}: unresolved libraries {', '.join(names)}"
        for path, names in sorted(sidecar["unresolved"].items())
    ]
    return problems + offer_problems(sidecar["source_offer"], set(bundled))


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    commands = parser.add_subparsers(dest="command", required=True)

    def add_version(command: argparse.ArgumentParser) -> None:
        group = command.add_mutually_exclusive_group(required=True)
        group.add_argument("--release-tag")
        group.add_argument("--version-suffix")

    manifest_command = commands.add_parser("manifest", help="write a concrete manifest")
    manifest_command.add_argument("--output", type=Path, required=True)
    manifest_command.add_argument("--commit", required=True)
    add_version(manifest_command)
    source = manifest_command.add_mutually_exclusive_group(required=True)
    source.add_argument("--source-dir", type=Path)
    source.add_argument("--git-commit", action="store_true")

    flathub_command = commands.add_parser("flathub", help="write the Flathub submission")
    flathub_command.add_argument("--output-dir", type=Path, required=True)
    flathub_command.add_argument("--tag", required=True)
    flathub_command.add_argument("--commit", required=True)

    stage_command_parser = commands.add_parser("stage", help="stage documents inside the build")
    stage_command_parser.add_argument("--build-dir", type=Path, required=True)
    stage_command_parser.add_argument("--prefix", type=Path, required=True)
    stage_command_parser.add_argument("--commit", required=True)
    add_version(stage_command_parser)

    inventory_command = commands.add_parser("inventory", help="write a finished build's sidecar")
    inventory_command.add_argument("--build-dir", type=Path, required=True)
    inventory_command.add_argument("--manifest", type=Path, required=True)
    inventory_command.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    try:
        if args.command == "manifest":
            template = load_manifest()
            project_source = (
                git_source(args.commit)
                if args.git_commit
                else directory_source(template, args.source_dir)
            )
            manifest = concrete_manifest(
                template,
                project_source=project_source,
                commit=args.commit,
                versions=version_args(args.release_tag, args.version_suffix),
            )
            write_manifest(manifest, args.output)
            print(args.output)
        elif args.command == "flathub":
            print(write_flathub(args.output_dir, tag=args.tag, commit=args.commit))
        elif args.command == "stage":
            stage(
                args.build_dir,
                args.prefix,
                commit=args.commit,
                release_tag=args.release_tag,
                version_suffix=args.version_suffix,
            )
        else:
            sidecar = inventory(args.build_dir, args.manifest)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(sidecar, indent=2, sort_keys=True) + "\n")
            print(f"{args.output}: {sidecar['version']} on {sidecar['runtime']}")
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"package_flatpak: {error}\n")


if __name__ == "__main__":
    main()

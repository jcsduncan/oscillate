"""Write and check the SPDX 2.3 bill of materials shipped inside each package.

The SBOM lists every file in the package with its hashes, the third-party
code compiled into the project's executables, the Ubuntu packages whose files
the AppImage copies, and the Ubuntu packages the .deb depends on. Third-party
licences are recorded as NOASSERTION with a pointer to the shipped notice: the
SBOM reports provenance and does not conclude any licence (G8, issue #93).
"""

from __future__ import annotations

import hashlib
import json
import subprocess
from datetime import UTC, datetime
from pathlib import Path

from package_notices import RUNTIME_STATIC_NOTICES

SBOM_PATH = Path("usr/share/doc/oscillate/sbom.spdx.json")
PROJECT_URL = "https://github.com/jcsduncan/oscillate"
PROJECT_LICENCE = "GPL-3.0-or-later"
NO_ASSERTION = "NOASSERTION"
UBUNTU_DISTRO = "ubuntu-26.04"
# Package-internal paths that are not payload: dpkg-deb's control directory.
NOT_PAYLOAD = ("DEBIAN",)
READ_CHUNK_BYTES = 1 << 20

# Third-party source compiled into the project's executables. The nlohmann
# version comes from the build host's development package.
PFFFT = {
    "name": "pffft",
    "version": "1.1.0",
    "download": "https://github.com/marton78/pffft/tree/1fdcb508e5082052ab406d560b7c4622b8b4b0dd",
    "purl": "pkg:github/marton78/pffft@v1.1.0",
    "notice": "pffft",
    "comment": "Vendored in third_party/pffft with allocation-failure.patch applied.",
}
VST3_PLUGINTERFACES = {
    "name": "vst3sdk-pluginterfaces",
    "version": "v3.8.1_build_84",
    "download": "https://github.com/steinbergmedia/vst3_pluginterfaces/tree/"
    "4f547e8e102b47de4a8b8aaf343c73b700786372",
    "purl": "pkg:github/steinbergmedia/vst3sdk@v3.8.1_build_84",
    "notice": "vst3-pluginterfaces",
    "comment": "Interface headers only; no SDK source file is compiled.",
}
TYPE2_RUNTIME = {
    "name": "appimage-type2-runtime",
    "version": "20251108",
    "download": "https://github.com/AppImage/type2-runtime/releases/tag/20251108",
    "purl": "pkg:github/AppImage/type2-runtime@20251108",
    "notice": "type2-runtime",
}


def file_hashes(path: Path) -> tuple[str, str]:
    sha1 = hashlib.sha1(usedforsecurity=False)
    sha256 = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(READ_CHUNK_BYTES), b""):
            sha1.update(block)
            sha256.update(block)
    return sha1.hexdigest(), sha256.hexdigest()


def installed_version(package: str) -> str:
    result = subprocess.run(
        ["dpkg-query", "-W", "-f=${Version}", package], capture_output=True, text=True, check=False
    )
    if result.returncode != 0 or not result.stdout:
        raise ValueError(f"build host lacks package {package}")
    return result.stdout


def payload_paths(stage_root: Path) -> list[Path]:
    return sorted(
        path.relative_to(stage_root)
        for path in stage_root.rglob("*")
        if path.is_file()
        and not path.is_symlink()
        and path.relative_to(stage_root) != SBOM_PATH
        and path.relative_to(stage_root).parts[0] not in NOT_PAYLOAD
    )


def spdx_id(kind: str, name: str) -> str:
    safe = "".join(
        character if character.isalnum() or character in ".-" else "-" for character in name
    )
    return f"SPDXRef-{kind}-{safe}"


def ubuntu_purl(package: str, version: str | None) -> str:
    at_version = f"@{version}" if version else ""
    return f"pkg:deb/ubuntu/{package}{at_version}?arch=amd64&distro={UBUNTU_DISTRO}"


def third_party_package(
    identifier: str,
    name: str,
    version: str | None,
    download: str,
    purl: str,
    comment: str,
) -> dict[str, object]:
    package: dict[str, object] = {
        "SPDXID": identifier,
        "name": name,
        "downloadLocation": download,
        "filesAnalyzed": False,
        "licenseConcluded": NO_ASSERTION,
        "licenseDeclared": NO_ASSERTION,
        "copyrightText": NO_ASSERTION,
        "externalRefs": [
            {
                "referenceCategory": "PACKAGE-MANAGER",
                "referenceType": "purl",
                "referenceLocator": purl,
            }
        ],
        "comment": comment,
    }
    if version:
        package["versionInfo"] = version
    return package


def notice_path(notice_inventory: dict[str, object], component: str) -> str:
    for item in notice_inventory["project_notices"]:
        if item["component"] == component:
            return "/" + str(item["path"])
    raise ValueError(f"no staged notice for {component}")


def dependency_names(depends: str) -> list[tuple[str, str]]:
    """(package, clause) for every package named in a Debian Depends field."""
    names = []
    for clause in depends.split(","):
        clause = clause.strip()
        for alternative in clause.split("|"):
            name = alternative.strip().split(" ", 1)[0].split(":", 1)[0]
            if not name:
                raise ValueError(f"malformed Depends clause: {clause!r}")
            names.append((name, clause))
    return names


def write_sbom(
    stage_root: Path,
    *,
    package_name: str,
    version: str,
    form: str,
    notice_inventory: dict[str, object],
    nlohmann_version: str,
    creation_epoch: int,
    depends: str | None = None,
    runtime_sha256: str | None = None,
) -> dict[str, object]:
    """Write the SBOM for the files staged under ``stage_root``."""
    paths = payload_paths(stage_root)
    if not paths:
        raise ValueError("nothing staged to describe")
    root_id = spdx_id("Package", package_name)
    files = []
    file_ids: dict[str, str] = {}
    tree = hashlib.sha256()
    for index, relative in enumerate(paths):
        sha1, sha256 = file_hashes(stage_root / relative)
        tree.update(f"{relative}={sha256}\n".encode())
        identifier = f"SPDXRef-File-{index}"
        file_ids[str(relative)] = identifier
        files.append(
            {
                "SPDXID": identifier,
                "fileName": f"./{relative}",
                "checksums": [
                    {"algorithm": "SHA1", "checksumValue": sha1},
                    {"algorithm": "SHA256", "checksumValue": sha256},
                ],
                "licenseConcluded": NO_ASSERTION,
                "copyrightText": NO_ASSERTION,
            }
        )
    root = {
        "SPDXID": root_id,
        "name": package_name,
        "versionInfo": version,
        "downloadLocation": PROJECT_URL,
        "filesAnalyzed": False,
        "licenseConcluded": NO_ASSERTION,
        "licenseDeclared": PROJECT_LICENCE,
        "copyrightText": NO_ASSERTION,
        "primaryPackagePurpose": "APPLICATION",
        "externalRefs": [
            {
                "referenceCategory": "PACKAGE-MANAGER",
                "referenceType": "purl",
                "referenceLocator": f"pkg:github/jcsduncan/oscillate@v{version}",
            }
        ],
        "comment": f"Oscillate {form} package.",
    }
    packages = [root]
    relationships = [
        {
            "spdxElementId": "SPDXRef-DOCUMENT",
            "relationshipType": "DESCRIBES",
            "relatedSpdxElement": root_id,
        }
    ]

    def relate(element: str, kind: str, related: str) -> None:
        relationships.append(
            {"spdxElementId": element, "relationshipType": kind, "relatedSpdxElement": related}
        )

    for identifier in file_ids.values():
        relate(root_id, "CONTAINS", identifier)

    compiled_in = [
        {
            "name": "nlohmann-json",
            "version": nlohmann_version,
            "download": "https://github.com/nlohmann/json",
            "purl": ubuntu_purl("nlohmann-json3-dev", nlohmann_version),
            "notice": "nlohmann-json3-dev",
            "comment": "Header-only library from the build host's development package.",
        },
        PFFFT,
        VST3_PLUGINTERFACES,
    ]
    for component in compiled_in:
        identifier = spdx_id("Package", component["name"])
        packages.append(
            third_party_package(
                identifier,
                component["name"],
                component["version"],
                component["download"],
                component["purl"],
                f"{component['comment']} Notice: "
                + notice_path(notice_inventory, component["notice"]),
            )
        )
        relate(root_id, "STATIC_LINK", identifier)

    for name, evidence in sorted(notice_inventory["copied_library_packages"].items()):
        binary = name.split(":", 1)[0]
        identifier = spdx_id("Package", f"ubuntu-{binary}")
        packages.append(
            third_party_package(
                identifier,
                binary,
                str(evidence["version"]),
                NO_ASSERTION,
                ubuntu_purl(binary, str(evidence["version"])),
                f"Copied from Ubuntu 26.04. Notice: /{evidence['notice_path']}",
            )
        )
        for item in notice_inventory["copied_elfs"]:
            if item["binary_package"] == name:
                relate(identifier, "CONTAINS", file_ids[str(item["path"])])

    if depends is not None:
        seen: set[str] = set()
        for name, clause in dependency_names(depends):
            if name in seen:
                continue
            seen.add(name)
            identifier = spdx_id("Package", f"ubuntu-{name}")
            packages.append(
                third_party_package(
                    identifier,
                    name,
                    None,
                    NO_ASSERTION,
                    ubuntu_purl(name, None),
                    f"Ubuntu runtime dependency, not shipped in this package: {clause}",
                )
            )
            relate(root_id, "DEPENDS_ON", identifier)

    if runtime_sha256 is not None:
        runtime_id = spdx_id("Package", TYPE2_RUNTIME["name"])
        runtime = third_party_package(
            runtime_id,
            TYPE2_RUNTIME["name"],
            TYPE2_RUNTIME["version"],
            TYPE2_RUNTIME["download"],
            TYPE2_RUNTIME["purl"],
            "AppImage runtime prepended to the image. "
            f"Notice: {notice_path(notice_inventory, TYPE2_RUNTIME['notice'])}",
        )
        runtime["checksums"] = [{"algorithm": "SHA256", "checksumValue": runtime_sha256}]
        packages.append(runtime)
        relate(root_id, "CONTAINS", runtime_id)
        # Notice files are named <library>-<version>-<file>; one library can
        # have several (libfuse ships its licence and the LGPL text).
        libraries: dict[str, tuple[str, list[str]]] = {}
        for component, (filename, _) in sorted(RUNTIME_STATIC_NOTICES.items()):
            library, library_version, _ = filename.split("-", 2)
            entry = libraries.setdefault(library, (library_version, []))
            entry[1].append(f"/usr/share/doc/oscillate/third-party/runtime-static/{component}.txt")
        for library, (library_version, notices) in sorted(libraries.items()):
            identifier = spdx_id("Package", f"runtime-{library}")
            packages.append(
                third_party_package(
                    identifier,
                    library,
                    None,
                    NO_ASSERTION,
                    f"pkg:generic/{library}",
                    "Statically linked into the AppImage runtime; its compiled version is not "
                    f"attested. Notices (from upstream {library_version}): " + ", ".join(notices),
                )
            )
            relate(runtime_id, "STATIC_LINK", identifier)

    created = datetime.fromtimestamp(creation_epoch, UTC).strftime("%Y-%m-%dT%H:%M:%SZ")
    document = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"{package_name}-{version}-{form}",
        "documentNamespace": (
            f"{PROJECT_URL}/sbom/{package_name}-{version}-{form}-{tree.hexdigest()}"
        ),
        "creationInfo": {"created": created, "creators": ["Tool: oscillate-package-sbom"]},
        "documentDescribes": [root_id],
        "packages": packages,
        "files": files,
        "relationships": relationships,
    }
    check_document(document)
    destination = stage_root / SBOM_PATH
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n")
    destination.chmod(0o644)
    return document


def check_document(document: dict[str, object]) -> None:
    """Structural SPDX 2.3 checks: required fields, unique ids, resolvable links."""
    if document.get("spdxVersion") != "SPDX-2.3" or document.get("dataLicense") != "CC0-1.0":
        raise ValueError("SBOM is not an SPDX 2.3 document")
    for field in ("name", "documentNamespace", "creationInfo", "packages", "files"):
        if not document.get(field):
            raise ValueError(f"SBOM lacks {field}")
    identifiers = ["SPDXRef-DOCUMENT"]
    for package in document["packages"]:
        for field in ("SPDXID", "name", "downloadLocation", "licenseConcluded", "licenseDeclared"):
            if not package.get(field):
                raise ValueError(f"SBOM package lacks {field}: {package.get('name')}")
        identifiers.append(package["SPDXID"])
    for item in document["files"]:
        algorithms = {checksum["algorithm"] for checksum in item.get("checksums", [])}
        if not item.get("SPDXID") or not item.get("fileName") or "SHA1" not in algorithms:
            raise ValueError(f"SBOM file entry is incomplete: {item.get('fileName')}")
        identifiers.append(item["SPDXID"])
    if len(identifiers) != len(set(identifiers)):
        raise ValueError("SBOM SPDX identifiers are not unique")
    known = set(identifiers)
    for relationship in document["relationships"]:
        if not {relationship["spdxElementId"], relationship["relatedSpdxElement"]} <= known:
            raise ValueError(f"SBOM relationship names an unknown element: {relationship}")


def verify_sbom(stage_root: Path) -> dict[str, object]:
    """Check the shipped SBOM against the files actually in the package."""
    sbom = stage_root / SBOM_PATH
    if not sbom.is_file() or sbom.is_symlink():
        raise ValueError("package has no SBOM")
    document = json.loads(sbom.read_text())
    check_document(document)
    listed = {}
    for item in document["files"]:
        checksums = {
            checksum["algorithm"]: checksum["checksumValue"] for checksum in item["checksums"]
        }
        listed[item["fileName"].removeprefix("./")] = checksums
    actual = {str(path) for path in payload_paths(stage_root)}
    if set(listed) != actual:
        missing = sorted(actual - set(listed))
        extra = sorted(set(listed) - actual)
        raise ValueError(f"SBOM file list differs from package: unlisted {missing}, absent {extra}")
    for relative, checksums in listed.items():
        sha1, sha256 = file_hashes(stage_root / relative)
        if checksums.get("SHA1") != sha1 or checksums.get("SHA256") != sha256:
            raise ValueError(f"SBOM hash differs from package file: {relative}")
    return document

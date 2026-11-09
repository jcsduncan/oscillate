"""Release package feature profiles and their CMake build check.

Each package form is built from its own Release tree. The profile names every
CMake switch that changes what the package can do, so a package can only be
made from a build configured exactly as documented in
docs/dependencies/release-packages.md.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

SOURCE_ROOT = Path(__file__).resolve().parent.parent
APP_RELATIVE = Path("apps/daw_quick/oscillate")
HELPER_RELATIVE = Path("apps/daw_quick/oscillate_vst3_scan_helper")

# Switches every package shares: a static Release Quick build with no test
# hooks or local runtime paths, the VST3 host, JACK output, both
# libsamplerate converters and the libsndfile and libmpg123 codec adapter.
COMMON_SWITCHES = {
    "BUILD_TESTING": "OFF",
    "CMAKE_BUILD_TYPE": "Release",
    "CMAKE_SKIP_BUILD_RPATH": "ON",
    "OSCILLATE_SHARED_LIBS": "OFF",
    "OSCILLATE_BUILD_QUICK_UI": "ON",
    "OSCILLATE_BUILD_VST3_SCANNER": "ON",
    "OSCILLATE_REQUIRE_JACK": "ON",
    "JACK_FOUND": "1",
    "OSCILLATE_BUILD_EXPORT_RESAMPLER": "ON",
    "OSCILLATE_BUILD_SOURCE_RESAMPLER": "ON",
    "OSCILLATE_BUILD_CODEC_ADAPTER": "ON",
}

# What each optional feature gives the user, by the switch that enables it.
FEATURES = {
    "OSCILLATE_BUILD_VST3_SCANNER": "VST3 instruments and effects (out-of-process scanner)",
    "JACK_FOUND": "JACK output and real-time export",
    "OSCILLATE_BUILD_EXPORT_RESAMPLER": "Export sample-rate conversion (libsamplerate)",
    "OSCILLATE_BUILD_SOURCE_RESAMPLER": "Source sample-rate conversion (libsamplerate)",
    "OSCILLATE_BUILD_CODEC_ADAPTER": "AIFF, FLAC and MP3 import and export (libsndfile, libmpg123)",
}

UBUNTU_PLATFORM = "Ubuntu 26.04 amd64"

# The Flatpak: its ID (Flathub's form for a project hosted on GitHub), the KDE
# runtime that carries Qt 6.10, and the libraries it builds because the
# runtime lacks them. The runtime's libsndfile is built without MPEG, so the
# Flatpak builds its own against the runtime's LAME and mpg123; every other
# library comes from the runtime.
FLATPAK_APP_ID = "io.github.jcsduncan.Oscillate"
FLATPAK_RUNTIME = "org.kde.Platform"
FLATPAK_RUNTIME_VERSION = "6.10"
FLATPAK_RUNTIME_REF = f"{FLATPAK_RUNTIME}//{FLATPAK_RUNTIME_VERSION}"
# Each shared library the Flatpak installs under /app/lib, by SONAME, and the
# manifest module that builds it.
FLATPAK_LIBRARY_MODULES = {"libsndfile.so.1": "libsndfile"}


# Ubuntu 26.04 packages each package needs at run time, checked against the
# built package by scripts/package_dependencies.py. A package name stands for
# the package on amd64; a Depends alternative is written "a | b".
@dataclass(frozen=True)
class RuntimeDependencies:
    # Installed on the host: the .deb's Depends field (versions left out), or
    # what the AppImage leaves to the host.
    system: tuple[str, ...]
    # Copied into the package: the packages owning every library and Qt plugin
    # the AppImage bundles. The .deb bundles none.
    bundled: tuple[str, ...]
    # Host packages loaded at run time rather than linked, so no sidecar
    # records them and verify cannot check them: package -> when it is needed.
    conditional: dict[str, str] = field(default_factory=dict)


DEB_DEPENDS = (
    "libc6",
    "libgcc-s1",
    "libjack-jackd2-0 | libjack-0.125",
    "libmpg123-0t64",
    "libpipewire-0.3-0t64",
    "libqt6core6t64",
    "libqt6gui6",
    "libqt6qml6",
    "libqt6quick6",
    "libqt6widgets6",
    "libsamplerate0",
    "libsndfile1",
    "libssl3t64",
    "libstdc++6",
    "libx11-6",
    "qml6-module-qtquick",
)

# glibc's base ABI and libproxy (on every Ubuntu desktop through
# glib-networking) stay on the host; stage-appdir.py copies everything else
# the app and its Qt plugins load.
APPIMAGE_HOST = ("libc6", "libproxy1v5")
# The .deb gets these through its PipeWire library's own Depends.
APPIMAGE_CONDITIONAL = {
    "libspa-0.2-modules": "PipeWire output: the bundled PipeWire client loads its SPA "
    "support plugins from the host's PipeWire installation",
}
APPIMAGE_BUNDLED = (
    "libatomic1",
    "libb2-1",
    "libblkid1",
    "libbrotli1",
    "libbz2-1.0",
    "libcom-err2",
    "libcurl3t64-gnutls",
    "libdb5.3t64",
    "libdbus-1-3",
    "libdouble-conversion3",
    "libduktape207",
    "libegl1",
    "libexpat1",
    "libffi8",
    "libflac14",
    "libfontconfig1",
    "libfreetype6",
    "libgcc-s1",
    "libglib2.0-0t64",
    "libglvnd0",
    "libglx0",
    "libgmp10",
    "libgnutls30t64",
    "libgomp1",
    "libgraphite2-3",
    "libgssapi-krb5-2",
    "libharfbuzz0b",
    "libhogweed6t64",
    "libice6",
    "libicu78",
    "libidn2-0",
    "libjack-jackd2-0",
    "libk5crypto3",
    "libkeyutils1",
    "libkrb5-3",
    "libkrb5support0",
    "libldap2",
    "libmd4c0",
    "libmount1",
    "libmp3lame0",
    "libmpg123-0t64",
    "libnettle8t64",
    "libnghttp2-14",
    "libogg0",
    "libopengl0",
    "libopus0",
    "libp11-kit0",
    "libpcre2-16-0",
    "libpcre2-8-0",
    "libpipewire-0.3-0t64",
    "libpng16-16t64",
    "libpsl5t64",
    "libqt6core6t64",
    "libqt6dbus6",
    "libqt6gui6",
    "libqt6network6",
    "libqt6opengl6",
    "libqt6qml6",
    "libqt6qmlmeta6",
    "libqt6qmlmodels6",
    "libqt6qmlworkerscript6",
    "libqt6quick6",
    "libqt6waylandclient6",
    "libqt6widgets6",
    "librtmp1",
    "libsamplerate0",
    "libsasl2-2",
    "libselinux1",
    "libsm6",
    "libsndfile1",
    "libssh2-1t64",
    "libssl3t64",
    "libstdc++6",
    "libsystemd0",
    "libtasn1-6",
    "libunistring5",
    "libuuid1",
    "libvorbis0a",
    "libvorbisenc2",
    "libwayland-client0",
    "libwayland-cursor0",
    "libx11-6",
    "libx11-xcb1",
    "libxau6",
    "libxcb-cursor0",
    "libxcb-icccm4",
    "libxcb-image0",
    "libxcb-keysyms1",
    "libxcb-randr0",
    "libxcb-render-util0",
    "libxcb-render0",
    "libxcb-shape0",
    "libxcb-shm0",
    "libxcb-sync1",
    "libxcb-util1",
    "libxcb-xfixes0",
    "libxcb-xinput0",
    "libxcb-xkb1",
    "libxcb1",
    "libxdmcp6",
    "libxkbcommon-x11-0",
    "libxkbcommon0",
    "libzstd1",
    "zlib1g",
)


@dataclass(frozen=True)
class Profile:
    name: str
    preset: str
    switches: dict[str, str]
    runtime: RuntimeDependencies
    # What the package runs on: an Ubuntu release, or a Flatpak runtime.
    platform: str = UBUNTU_PLATFORM
    # Whether the package ships an SPDX SBOM (sbom.spdx.json).
    sbom: bool = True
    # Optional features left out, by switch, with the reason.
    excluded: dict[str, str] = field(default_factory=dict)

    def included_features(self) -> list[str]:
        return [
            description
            for switch, description in FEATURES.items()
            if self.switches.get(switch) in ("ON", "1")
        ]

    def excluded_features(self) -> dict[str, str]:
        return {FEATURES[switch]: reason for switch, reason in self.excluded.items()}


PROFILES = {
    "deb": Profile(
        name="deb",
        preset="package-deb",
        switches=COMMON_SWITCHES,
        runtime=RuntimeDependencies(system=DEB_DEPENDS, bundled=()),
    ),
    "appimage": Profile(
        name="appimage",
        preset="package-appimage",
        switches=COMMON_SWITCHES,
        runtime=RuntimeDependencies(
            system=APPIMAGE_HOST, bundled=APPIMAGE_BUNDLED, conditional=APPIMAGE_CONDITIONAL
        ),
    ),
    # For the Flatpak, "system" is the runtime and "bundled" the manifest
    # modules whose libraries it installs under /app.
    "flatpak": Profile(
        name="flatpak",
        preset="package-flatpak",
        switches=COMMON_SWITCHES,
        runtime=RuntimeDependencies(
            system=(FLATPAK_RUNTIME_REF,),
            bundled=tuple(sorted(set(FLATPAK_LIBRARY_MODULES.values()))),
        ),
        platform=f"{FLATPAK_RUNTIME_REF} x86_64 (Flatpak)",
        sbom=False,
    ),
}

# Debian upstream-version characters; a CI suffix follows a "~" so it sorts
# before the release it precedes.
VERSION_PATTERN = re.compile(r"[0-9][A-Za-z0-9.+~]*")
SUFFIX_PATTERN = re.compile(r"[A-Za-z0-9.]+")


@dataclass(frozen=True)
class BuildInputs:
    app: Path
    helper: Path
    version: str
    switches: dict[str, str]


def read_cmake_cache(cache_path: Path) -> dict[str, str]:
    cache: dict[str, str] = {}
    for line in cache_path.read_text().splitlines():
        if line.startswith(("#", "//")) or "=" not in line:
            continue
        name_and_type, value = line.split("=", 1)
        cache[name_and_type.split(":", 1)[0]] = value
    return cache


def package_version(project_version: str, release_tag: str | None, suffix: str | None) -> str:
    """The package version: the project version, or a pre-release CI build of it."""
    if not VERSION_PATTERN.fullmatch(project_version):
        raise ValueError(f"unusable project version: {project_version!r}")
    if release_tag is not None:
        if suffix is not None:
            raise ValueError("a release package takes no version suffix")
        if release_tag != f"v{project_version}":
            raise ValueError(f"tag {release_tag} does not match project version {project_version}")
        return project_version
    if suffix is None:
        raise ValueError("a non-release package needs a version suffix")
    if not SUFFIX_PATTERN.fullmatch(suffix):
        raise ValueError(f"unusable version suffix: {suffix!r}")
    return f"{project_version}~{suffix}"


def check_build(
    build_dir: Path,
    profile: Profile,
    *,
    release_tag: str | None = None,
    version_suffix: str | None = None,
) -> BuildInputs:
    """Bind a package to one Release build tree configured with its profile."""
    build_dir = build_dir.resolve(strict=True)
    cache = read_cmake_cache(build_dir / "CMakeCache.txt")
    for name, expected in profile.switches.items():
        if cache.get(name) != expected:
            raise ValueError(
                f"{profile.name} package requires {name}={expected}; got {cache.get(name)!r}"
            )
    if Path(cache.get("CMAKE_HOME_DIRECTORY", "/nonexistent")).resolve() != SOURCE_ROOT:
        raise ValueError("package build tree was configured from a different source tree")
    version = package_version(cache.get("CMAKE_PROJECT_VERSION", ""), release_tag, version_suffix)
    app = build_dir / APP_RELATIVE
    helper = build_dir / HELPER_RELATIVE
    for path in (app, helper):
        if not path.is_file():
            raise ValueError(f"package build tree lacks {path.relative_to(build_dir)}")
    return BuildInputs(
        app=app,
        helper=helper,
        version=version,
        switches={name: cache[name] for name in sorted(profile.switches)},
    )


def feature_readme(profile: Profile, version: str) -> str:
    """The plain-text feature list shipped inside each package."""
    lines = [
        f"Oscillate {version} ({profile.name})",
        "",
        "Included optional features:",
        *(f"  - {feature}" for feature in profile.included_features()),
        "",
    ]
    if profile.excluded:
        lines += [
            "Left out of this package:",
            *(
                f"  - {feature}: {reason}"
                for feature, reason in profile.excluded_features().items()
            ),
            "",
        ]
    lines += [
        "Licence: GPL-3.0-or-later (LICENSE). Third-party notices are in",
        "third-party/, indexed by third-party/inventory.json and joined in",
        "third-party/NOTICES.txt.",
    ]
    if profile.sbom:
        lines += [
            "sbom.spdx.json is an SPDX 2.3 bill of materials for every file in this",
            "package.",
        ]
    if profile.runtime.bundled:
        lines += [
            "",
            "SOURCE-OFFER.txt is the written offer of source for the libraries and",
            "runtime code this package bundles, with links to their exact sources.",
        ]
    return "\n".join(lines) + "\n"


# Pinned AppImage build inputs (GitHub release assets, verified by SHA-256).
APPIMAGETOOL = {
    "release": "https://github.com/AppImage/appimagetool/releases/tag/1.9.1",
    "url": "https://github.com/AppImage/appimagetool/releases/download/1.9.1/"
    "appimagetool-x86_64.AppImage",
    "sha256": "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0",
}
TYPE2_RUNTIME_REPOSITORY = "https://github.com/AppImage/type2-runtime"
TYPE2_RUNTIME_TAG = "20251108"
TYPE2_RUNTIME = {
    "release": f"{TYPE2_RUNTIME_REPOSITORY}/releases/tag/{TYPE2_RUNTIME_TAG}",
    "url": f"{TYPE2_RUNTIME_REPOSITORY}/releases/download/{TYPE2_RUNTIME_TAG}/runtime-x86_64",
    "sha256": "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d",
}

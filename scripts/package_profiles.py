"""Release package feature profiles and their CMake build check.

Each package form is built from its own Release tree. The profile names every
CMake switch that changes what the package can do, so a package can only be
made from a build configured exactly as documented in
docs/dependencies/release-packages.md.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

SOURCE_ROOT = Path(__file__).resolve().parent.parent
APP_RELATIVE = Path("apps/daw_quick/oscillate")
HELPER_RELATIVE = Path("apps/daw_quick/oscillate_vst3_scan_helper")

# Switches every package shares: a static Release Quick build with no test
# hooks or local runtime paths, the VST3 host, JACK output and both
# libsamplerate converters.
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
    "OSCILLATE_BUILD_CONVOLUTION_DSP": "OFF",
}

# What each optional feature gives the user, by the switch that enables it.
FEATURES = {
    "OSCILLATE_BUILD_VST3_SCANNER": "VST3 instruments and effects (out-of-process scanner)",
    "JACK_FOUND": "JACK output and real-time export",
    "OSCILLATE_BUILD_EXPORT_RESAMPLER": "Export sample-rate conversion (libsamplerate)",
    "OSCILLATE_BUILD_SOURCE_RESAMPLER": "Source sample-rate conversion (libsamplerate)",
    "OSCILLATE_BUILD_CODEC_ADAPTER": "AIFF, FLAC and MP3 import and export (libsndfile)",
    "OSCILLATE_BUILD_CONVOLUTION_DSP": "Convolution reverb",
}

CONVOLUTION_REASON = (
    "ADR-0020 keeps the convolution core out of release builds until its graph "
    "admission and multi-instance policy are qualified"
)
CODEC_REASON = (
    "the AppImage would have to bundle Ubuntu's libmp3lame, whose fft.c is "
    "classified GPL-1+; redistribution is undecided (G8, issue #93)"
)


@dataclass(frozen=True)
class Profile:
    name: str
    preset: str
    switches: dict[str, str]
    excluded: dict[str, str]

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
        switches=COMMON_SWITCHES | {"OSCILLATE_BUILD_CODEC_ADAPTER": "ON"},
        excluded={"OSCILLATE_BUILD_CONVOLUTION_DSP": CONVOLUTION_REASON},
    ),
    "appimage": Profile(
        name="appimage",
        preset="package-appimage",
        switches=COMMON_SWITCHES | {"OSCILLATE_BUILD_CODEC_ADAPTER": "OFF"},
        excluded={
            "OSCILLATE_BUILD_CODEC_ADAPTER": CODEC_REASON,
            "OSCILLATE_BUILD_CONVOLUTION_DSP": CONVOLUTION_REASON,
        },
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
        "Left out of this package:",
        *(f"  - {feature}: {reason}" for feature, reason in profile.excluded_features().items()),
        "",
        "Licence: GPL-3.0-or-later (LICENSE). Third-party notices are in",
        "third-party/, indexed by third-party/inventory.json. sbom.spdx.json is an",
        "SPDX 2.3 bill of materials for every file in this package.",
    ]
    return "\n".join(lines) + "\n"


# Pinned AppImage build inputs (GitHub release assets, verified by SHA-256).
APPIMAGETOOL = {
    "release": "https://github.com/AppImage/appimagetool/releases/tag/1.9.1",
    "url": "https://github.com/AppImage/appimagetool/releases/download/1.9.1/"
    "appimagetool-x86_64.AppImage",
    "sha256": "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0",
}
TYPE2_RUNTIME = {
    "release": "https://github.com/AppImage/type2-runtime/releases/tag/20251108",
    "url": "https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64",
    "sha256": "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d",
}

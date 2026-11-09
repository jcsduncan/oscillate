# Flatpak

Oscillate's Flatpak has the app ID `io.github.jcsduncan.Oscillate` (Flathub's
form for a project hosted at `github.com/jcsduncan`). The manifest is
[`packaging/flatpak/io.github.jcsduncan.Oscillate.json`](../../packaging/flatpak/io.github.jcsduncan.Oscillate.json).
The [packaging workflow](../../.github/workflows/packaging.yml) builds it,
checks it, and runs an install, launch and remove smoke test. On a release it
attaches the bundle to the GitHub release
([release packages](release-packages.md)).

## Runtime and modules

The app runs on the KDE runtime `org.kde.Platform//6.10`, which provides the
project's minimum Qt version, 6.10, and builds with `org.kde.Sdk//6.10`. The
runtime (Freedesktop SDK 25.08 underneath) provides Qt, PipeWire with its
JACK library, OpenSSL, libsamplerate, mpg123, LAME, FLAC, Ogg, Vorbis, Opus
and X11. The manifest builds only what the runtime lacks:

| Module | Source | Why |
| --- | --- | --- |
| `nlohmann-json` | 3.12.0 release archive, SHA-256 pinned | Header-only and needed only at build time; not in the runtime. |
| `libsndfile` | 1.2.2 release archive, SHA-256 pinned, with four upstream security fixes for CVE-2022-33065, CVE-2024-50612, CVE-2025-56226 and CVE-2026-37555 ([`patches/libsndfile`](../../packaging/flatpak/patches/libsndfile)) | The runtime's libsndfile is built without MPEG, and the codec adapter needs MP3. This build links the runtime's LAME and mpg123. |
| `oscillate` | The source tree, plus the VST3 SDK `v3.8.1_build_84` and its `pluginterfaces` at pinned commits | Configured with the `package-flatpak` CMake preset, the `package-deb` switches, so the [`flatpak` profile](../../scripts/package_profiles.py) holds. |

The Flatpak has the same features as the `.deb`.

## Sandbox permissions

| Permission | Why |
| --- | --- |
| `--share=ipc` | X11 shared memory, which Qt uses under XWayland and X11. |
| `--socket=wayland` | The window on a Wayland desktop. |
| `--socket=fallback-x11` | The window on an X11 desktop; unused when Wayland is available. |
| `--device=dri` | GPU rendering for Qt Quick. |
| `--socket=pulseaudio` | Flatpak's standard audio permission, which desktops show as the app's sound access. It covers PulseAudio-compatible servers, PipeWire's among them. |
| `--filesystem=xdg-run/pipewire-0` | The PipeWire socket. The PipeWire output, and JACK through PipeWire's JACK library, connect to it. |
| `--filesystem=xdg-music` | Import from and export to the Music folder, which the export dialog opens by default. |
| `--filesystem=~/.oscillate:create` | Oscillate's project folder, `~/.oscillate/projects`, shared with the `.deb` and the AppImage. |
| `--filesystem=~/.vst3:ro` | The per-user VST3 plug-in folder the plug-in scanner reads. |

Nothing else is granted: no network, no home folder, no other devices. File
dialogs outside these folders go through the desktop's file chooser portal,
which grants access to the chosen file. PipeWire raises its client thread to
real-time priority through the realtime portal, which needs no permission.

- **JACK.** The runtime's `libjack.so.0` is PipeWire's JACK implementation,
  so the JACK output reaches the desktop's PipeWire graph. A standalone
  `jackd` is not reachable from the sandbox: its sockets and shared memory
  are not shared.
- **MIDI.** Oscillate records MIDI from the computer keyboard and has no
  hardware MIDI input, so no device permission is needed.
- **Settings and caches** go to the sandbox's own folders under
  `~/.var/app/io.github.jcsduncan.Oscillate/`, as do logs and crash reports.
- **Sampled instrument libraries** (Concert, Fret, Wren, Bow) are read from
  `$XDG_DATA_HOME/oscillate/instruments/`, which in the sandbox is
  `~/.var/app/io.github.jcsduncan.Oscillate/data/oscillate/instruments/`.
  Copy an installed library there, or point the instrument's variable
  (`OSCILLATE_CONCERT_LIBRARY`, `OSCILLATE_FRET_LIBRARY`,
  `OSCILLATE_WREN_LIBRARY`, `OSCILLATE_BOW_LIBRARY`) at it and grant read
  access, for example
  `flatpak override --user --filesystem=~/.local/share/oscillate/instruments:ro --env=OSCILLATE_CONCERT_LIBRARY=$HOME/.local/share/oscillate/instruments/concert/salamander-grand-v3 io.github.jcsduncan.Oscillate`.

## VST3 plug-ins

VST3 hosting is experimental. In the Flatpak:

- The scanner looks in `~/.vst3`, which the sandbox can read, and in
  `/usr/lib/vst3`, `/usr/lib64/vst3`, `/usr/local/lib/vst3` and
  `/usr/local/lib64/vst3`. Inside the sandbox `/usr` is the runtime, so
  plug-ins the host installed system-wide are not found.
- Plug-ins packaged as `org.freedesktop.LinuxAudio.Plugins` extensions mount
  under `/app/extensions/Plugins/vst3`, which the scanner does not search, so
  the manifest declares no extension point.
- A plug-in runs inside the sandbox with the runtime's libraries; one that
  needs a host library the runtime lacks fails to load.
- Plug-in editors embed X11 windows, so they need X11 or XWayland.

## Metainfo

The [metainfo template](../../assets/packaging/io.github.jcsduncan.Oscillate.metainfo.xml)
has no `<releases>`. `package_flatpak.py stage` installs it to
`/app/share/metainfo/` with the newest releases of
[`CHANGELOG.md`](../../CHANGELOG.md), which Release Please writes:
`package_appstream.py` turns each release's sections into a `<description>`
and drops the scopes, pull request and commit links.

## Notices and source offer

flatpak-builder installs each module's licence files under
`/app/share/licenses/io.github.jcsduncan.Oscillate/<module>/`. The last build
step, `package_flatpak.py stage`, writes to `/app/share/doc/oscillate/`:

| File | Contents |
| --- | --- |
| `README` | Version and features, from the `flatpak` profile. |
| `LICENSE` | GPL-3.0-or-later. |
| `third-party/` | Notices for the code compiled into Oscillate (nlohmann/json, PFFFT, the VST3 SDK) and the licence files of `libsndfile`, indexed by `inventory.json` and joined in `NOTICES.txt`. |
| `SOURCE-OFFER.txt`, `third-party/sources.json` | The written source offer: Oscillate's commit, and every source the manifest builds into the package, each pinned by SHA-256 or commit. |

The runtime is not part of the bundle; Flathub distributes it with its
sources.

## Checks

| Stage | Check |
| --- | --- |
| Lint | `flatpak-builder-lint manifest` on the Flathub form of the manifest, `flatpak-builder-lint appstream` and `appstreamcli validate` on the generated metainfo, and `flatpak-builder-lint repo` on the built repository, screenshots mirrored as Flathub does. |
| Build | `stage` binds the build to the `flatpak` profile's switches, as the other packages are bound to theirs. |
| Runtime dependencies | `package_flatpak.py inventory` resolves every ELF under `/app` in the SDK sandbox, `/app/lib` first on the library path; `package_dependencies.py verify --profile flatpak` fails on an unresolved library, a runtime other than the profile's, a library no declared module owns, or a bundled module without a licence file or a source-offer entry. |
| Smoke | [`flatpak-smoke.sh`](../../tests/packaging/flatpak-smoke.sh) installs the bundle, creates and reopens a project with `--check-startup` offscreen, uninstalls with `--delete-data`, and checks that nothing is left. |

Source archives and git repositories are downloaded before the build, with
retries and backoff, and kept between runs with the build cache and ccache.
The workflow runs in the `ghcr.io/flathub-infra/flatpak-github-actions:kde-6.10`
image, pinned by digest, which carries the runtime, the SDK and the linter.

## Build locally

```sh
flatpak install flathub org.kde.Platform//6.10 org.kde.Sdk//6.10
flatpak-builder --user --install --force-clean build/flatpak/app \
  packaging/flatpak/io.github.jcsduncan.Oscillate.json
flatpak run io.github.jcsduncan.Oscillate
```

## Publishing on Flathub

Flathub builds from its own repository, `flathub/io.github.jcsduncan.Oscillate`,
which holds the manifest in Flathub's form: Oscillate's source is the release
tag at its commit instead of the local tree. For a release `vX.Y.Z`:

1. Tag the release Release Please made, so `CHANGELOG.md` has its notes.
2. Write the submission:
   `python3 -I scripts/package_flatpak.py flathub --tag vX.Y.Z --commit $(git rev-parse vX.Y.Z^{commit}) --output-dir build/flathub`.
   It refuses a tag that is not the project version, or a version
   `CHANGELOG.md` has no release for. It writes the manifest, its patches and
   `flathub.json`, which limits the build to x86-64.
3. For the first submission, open a pull request against the `new-pr` branch
   of `flathub/flathub` with those files; once accepted, Flathub creates the
   app repository. Later releases are pull requests to that repository.

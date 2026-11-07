# Oscillate

A Linux-first digital audio workstation. The desktop application uses Qt
Quick; its audio engine, project model, DSP, media pipeline and VST3 host are
written in C++20. Ubuntu 26.04 LTS on x86-64 is the initial target. JUCE is not
used.

This repository is **pre-release**. It can create and reopen projects and run
the arrangement-to-export workflow, but it has not passed the product's release
qualification. Keep a backup of important projects.

## What works today

- **Arrangement.** Import audio and draw MIDI clips; edit clips, notes,
  velocity, sustain/sostenuto/soft pedals, markers, tempo and time-signature
  maps. Chords are ordinary notes in MIDI clips, with a chord strip, the
  Progression Lab and Comp ([ADR-0084](docs/decisions/0084-chord-clips.md)).
  Standard MIDI files can be imported.
- **Recording from the computer keyboard.** Play the selected track's
  instrument from the QWERTY keyboard and record MIDI into it, with overdub,
  a metronome and a one-bar count-in
  ([ADR-0086](docs/decisions/0086-midi-recording-and-click.md)). There is no
  hardware MIDI input and no audio recording.
- **Instruments.** Thirteen built-in instruments on one native synth platform:
  Prism (subtractive, the default), Ember (vintage analogue poly), Nova
  (flagship wavetable synth), Pulse (analogue-modelled drums), Halo
  (six-operator FM), Torque (mono bass), Drift (granular), Anvil (drawn kick),
  Knock (modelled percussion), Crate (chop sampler), and three sampled
  instruments whose libraries are installed separately: Concert (grand
  piano), Fret (fingered bass) and Wren (concert flute).
- **Effects.** Twenty-three native effects and utilities: Contour (filter),
  Forge (saturation), Orbit (modulation), Relay (delay), Expanse (reverb),
  Vista (stereo width), Deck (DJ isolator), Pocket (kick/bass masking), Pump
  (drawn volume shaper), Chop (gate and stutter), Temper (resonance
  suppressor and dynamic EQ), Strata (multiband dynamics), Flyby (spatial
  pass-by), Dust (texture and ageing), Shape (EQ), Press (compressor),
  Ceiling (limiter), Lens (spectrum), Loudness (EBU R128), Scope
  (oscilloscope and alignment), Tune (tuner), Trim (gain and stereo) and Signal
  (test generator). A convolution reverb is available in builds configured
  with `-DOSCILLATE_BUILD_CONVOLUTION_DSP=ON`.
- **Mixing.** Each track has one source → insert chain → output path. Buses,
  sends, returns, sidechains, delay compensation, macros, racks and freeze.
  Most control edits are heard while the transport runs.
- **Plugins.** Linux VST3 instruments and effects, with out-of-process
  scanning, saved state and a native editor where the plugin and window system
  support it (X11 embedding).
- **Audio editing and analysis.** Tempo and key analysis, non-destructive warp
  and pitch, Clay note editing of audio
  ([ADR-0081](docs/decisions/0081-clay-note-editing.md)), groove extraction
  and Arrange from a loop.
- **Automation and export.** Track, clip, send and device automation drawn on
  the arrangement. Mix Check reports problems offline. Export masters and
  processed stems as WAV, and as FLAC or MP3 in codec-enabled builds.

Session View, audio recording, hardware MIDI input and advanced MIDI editing
are outside the current v1 scope. Plugin compatibility varies. The
[implementation plan](docs/IMPLEMENTATION_PLAN.md) lists open engineering and
qualification work; the [PRD](docs/PRD.md) defines the intended product.

## Build and run

Install the base tools and desktop dependencies on Ubuntu 26.04:

~~~sh
sudo apt-get install cmake ninja-build g++ pkgconf libpipewire-0.3-dev \
  libssl-dev nlohmann-json3-dev qt6-base-dev qt6-declarative-dev \
  python3 python3-venv
cmake --preset quick
scripts/build.sh build/quick --target oscillate
./build/quick/apps/daw_quick/oscillate
~~~

To build and run the fast tests (no audio device or display needed):

~~~sh
scripts/build.sh build/quick --target oscillate_tests
QT_QPA_PLATFORM=offscreen ctest --preset quick-fast -LE live -j4
~~~

The app opens a project library under ~/.oscillate/projects/. You can also
start it with --create FOLDER or --open FOLDER. The build script caps memory
(when a user systemd manager is running) and uses six compiler jobs, or
OSCILLATE_BUILD_JOBS, so development remains usable on a desktop. The
canonical local app is build/quick/apps/daw_quick/oscillate; other build
trees may contain different feature configurations.

The base Quick preset does not enable every optional dependency. For encoded
AIFF/FLAC/MP3 import, install libsndfile1-dev and configure with
-DOSCILLATE_BUILD_CODEC_ADAPTER=ON. The VST3 host needs the pinned SDK checkout
described in [local dependency setup](docs/dependencies/local-development.md)
and -DOSCILLATE_BUILD_VST3_SCANNER=ON with
-DOSCILLATE_VST3_SDK_ROOT=/path/to/vst3sdk. Optional JACK and resampling builds
need libjack-jackd2-dev and libsamplerate0-dev. Use CMakeUserPresets.json for
machine-specific paths; it is ignored by Git.

## Optional sample libraries

Concert, Fret and Wren play sample libraries that are not part of this
repository. Each installer downloads a pinned upstream release, verifies it,
converts it and writes it where the instrument looks. They need Python 3,
ffmpeg (built with libsoxr for Fret and Wren) and, for Concert and Fret, git.

| Instrument | Library and licence | Installer | Override |
| --- | --- | --- | --- |
| Concert | Salamander Grand Piano V3, CC-BY 3.0 | `python3 -I tools/concert/install_salamander.py` | `OSCILLATE_CONCERT_LIBRARY` |
| Fret | Karoryfer Black and Blue Basses (darkblack), CC0 1.0 | `python3 -I tools/fret/install_black_and_blue.py` | `OSCILLATE_FRET_LIBRARY` |
| Wren | University of Iowa Musical Instrument Samples flute | `python3 -I tools/wren/install_iowa_flute.py` | `OSCILLATE_WREN_LIBRARY` |

By default the libraries go under `~/.local/share/oscillate/instruments/`
(or `$XDG_DATA_HOME`). Setting the override variable changes the directory for
both the installer and the instrument. Without its library, an instrument
loads but reports that the library is missing. All environment variables are
listed in [docs/environment.md](docs/environment.md).

## Local checks

Build and run the tests affected by a change during feature work:

~~~sh
scripts/build.sh build/quick --target oscillate_project_tests
TMPDIR="$PWD/build/quick/scratch" ctest --test-dir build/quick \
  -R '^project_contract$' --output-on-failure
~~~

The Quick default build contains the app and its dependencies. Test binaries
remain configured but build only when requested. Before a full local sweep:

~~~sh
scripts/build.sh build/quick --target oscillate_tests
TMPDIR="$PWD/build/quick/scratch" ctest --test-dir build/quick \
  --output-on-failure -j4
~~~

Tests have fast, slow and live tiers; `ctest --preset quick-fast -j4` runs the
fast tier after its binaries are built. See [test guidance](tests/README.md)
and [contributor guidance](AGENTS.md) for sanitizer, coverage, format, tidy,
benchmark and real-device checks.

Run `python3 -I scripts/check-doc-links.py` after editing documentation. It
checks local file links without fetching remote pages.

## Repository map

| Path | Purpose |
| --- | --- |
| apps/daw_quick/ | Desktop entry point, Qt-facing adapters and QML |
| include/oscillate/<module>/ | Public C++ contracts |
| src/<module>/ | Engine, model, media, DSP, plugin and persistence implementations |
| tests/<module>/ | Unit, contract, integration and audio-oracle tests |
| assets/starter/ | Generated starter audio and its provenance |
| tools/ | Opt-in generators, sample-library installers and analysis tools |
| third_party/ | Vendored PFFFT and third-party notices |
| cmake/, scripts/ | Build policy and local quality tooling |
| docs/ | Product, architecture, behaviour, decisions and qualification evidence |

Start with the [documentation index](docs/README.md), [HLD](docs/HLD.md) and
[functional specs](docs/specs/README.md) before changing a component.
[CONTRIBUTING.md](CONTRIBUTING.md) explains how to report problems and propose
changes; [SECURITY.md](SECURITY.md) covers vulnerability reports.

## Licence

Oscillate is free software, licensed under the [GNU General Public License
version 3 or later](LICENSE). Third-party components keep their own terms; see
[third_party/](third_party/) and the [dependency documentation](docs/dependencies/).

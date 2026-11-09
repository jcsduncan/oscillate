# Environment variables and user files

The application reads a small number of environment variables. None is needed
for ordinary use.

## Application

| Variable | Read by | Effect |
| --- | --- | --- |
| `OSCILLATE_LOG` | `src/foundation/log.cpp` | Log filter: comma-separated items, each a level (`error`, `warning`, `info`, `debug`) or a category's level (`audio=debug`). Categories: `engine`, `device` (alias `audio`), `plugin`, `media`, `project`, `processing`, `app`. Default `info` in the log file and `warning` on the terminal; when set, the terminal shows the same lines as the file. Unknown items are ignored with a warning. |
| `OSCILLATE_TRACE` | `apps/daw_quick/src/performance_trace.cpp` | Records a performance trace ([ADR-0061](decisions/0061-performance-tracing.md)) from launch and writes it to the given path on quit. Summarise it with [trace_report](../tools/trace_report/README.md). |
| `OSCILLATE_TRACE_BUFFER_MB` | `src/foundation/trace.cpp` | Size of a performance trace's event pool in MiB, 1 to 4096 (default 32, about a million events). Allocated when a recording starts and freed when it stops. When it fills, each thread overwrites its oldest events. |
| `OSCILLATE_AUDIO_WORKERS` | `apps/daw_quick/src/routed_playback.cpp` | Number of audio helper threads for parallel rendering ([ADR-0048](decisions/0048-parallel-audio-rendering.md)). `0` renders on the device callback alone. Unset or invalid uses the machine default, one fewer than the hardware threads the process may run on, at most 7 ([ADR-0110](decisions/0110-heavy-projects-across-the-helpers.md)); values above the pool maximum (15) are clamped. |
| `OSCILLATE_CONCERT_LIBRARY` | `src/native_dsp/synth/concert_library.cpp` | Directory of Concert's piano library. |
| `OSCILLATE_FRET_LIBRARY` | `src/native_dsp/synth/fret_library.cpp` | Directory of Fret's bass library. |
| `OSCILLATE_WREN_LIBRARY` | `src/native_dsp/synth/wren_library.cpp` | Directory of Wren's flute library. |
| `OSCILLATE_BOW_LIBRARY` | `src/native_dsp/synth/bow_library.cpp` | Directory of Bow's strings library. |
| `OSCILLATE_STICK_LIBRARY` | `src/native_dsp/synth/stick_library.cpp` | Directory holding Stick's kits, each in its own folder (`drs-studio`, `muldjord`, `garage`). |
| `XDG_DATA_HOME` | presets, grooves, racks, traces, sample libraries | Base for user data; defaults to `~/.local/share`. |
| `XDG_CACHE_HOME` | shared source-analysis cache, Nova preset map | Base for disposable caches; defaults to `~/.cache`. |
| `XDG_STATE_HOME` | log files, crash reports | Base for the log and crash-report directories; defaults to `~/.local/state`. |
| `HOME` | project library, VST3 scan roots, fallbacks above | Used when the XDG variables are unset. |

The sample-library installers in `tools/concert`, `tools/fret`, `tools/wren`,
`tools/bow` and `tools/stick` write to the same directory the instrument reads, so set the
variable for both or for neither. See the README's
[optional sample libraries](../README.md#sample-libraries).

## Where the application keeps files

| Path | Contents |
| --- | --- |
| `~/.oscillate/projects/` | Default project library. Each project is a directory ([persistence](../src/persistence/README.md)). |
| `$XDG_DATA_HOME/oscillate/presets/` | User presets and favourites for native devices. |
| `$XDG_DATA_HOME/oscillate/grooves/` | User grooves. |
| `$XDG_DATA_HOME/oscillate/racks/` | User device racks. |
| `$XDG_DATA_HOME/oscillate/traces/` | Performance traces saved from the command palette. |
| `$XDG_DATA_HOME/oscillate/instruments/` | Installed sample libraries (Concert, Fret, Wren, Bow, Stick). |
| `$XDG_STATE_HOME/oscillate/logs/` | The application log, `oscillate.log`, also written to stderr. It rotates at 1 MiB, keeping `oscillate.1.log` to `oscillate.4.log`. Status messages, Qt and QML warnings, device start and stop, VST3 scan failures with the plugin's stderr, and audio-callback events (silenced blocks, xruns) go here. |
| `$XDG_STATE_HOME/oscillate/diagnostics/` | [Diagnostics bundles](diagnostics.md) saved from the command palette or `oscillate --save-diagnostics`, one `oscillate-diagnostics-<time>.zip` each. Nothing here is sent anywhere. |
| `$XDG_STATE_HOME/oscillate/crash-reports/` | Local [crash reports](crash-reports.md): `session.txt` (this session's description), `crashed` (the marker a crash leaves) and one `oscillate-crash-<time>.txt` per crash. Nothing here is sent anywhere. |
| `$XDG_CACHE_HOME/oscillate/` | Disposable caches: `source-analysis/` (shared waveform, tempo and key results) and `constellation/` (Nova preset map). Safe to delete while the application is closed. |
| `~/.vst3`, `/usr/lib/vst3`, `/usr/lib64/vst3`, `/usr/local/lib/vst3`, `/usr/local/lib64/vst3` | VST3 scan roots. |

Default exports go to `oscillate/` inside the Music folder (the home folder
when there is no Music folder).

## Build and tooling

| Variable | Read by | Effect |
| --- | --- | --- |
| `OSCILLATE_BUILD_JOBS` | `scripts/build.sh` | Compiler jobs (default 6). |
| `OSCILLATE_BUILD_MEMORY_HIGH`, `OSCILLATE_BUILD_MEMORY_MAX` | `scripts/build.sh` | Memory throttle and hard limit for the build's systemd scope (defaults 10G and 14G). Ignored without a user systemd manager. |
| `TMPDIR` | compilers, tests | Set to the build tree's `scratch/` directory; see [contributor guidance](../AGENTS.md#commands). |
| `OSCILLATE_MIX_CHECK_TRACE` | `tools/mix_check` | When set, the opt-in Mix Check tool prints every progress report with elapsed time. |

CMake options (`OSCILLATE_BUILD_*`, `OSCILLATE_VST3_SDK_ROOT` and so on) are
configure-time settings, not environment variables. They are described in the
README's [build section](../README.md#build) and
[local dependency setup](dependencies/local-development.md).

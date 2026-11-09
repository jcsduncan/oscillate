# Diagnostics bundle

A diagnostics bundle is one zip file that Oscillate saves on your computer
when you ask for it. Nothing is uploaded or sent anywhere. You decide whether
to attach it to an issue.

## Saving one

- In the app: open the command palette (Ctrl+K) and run **Save diagnostics**,
  or **Save diagnostics with the latest performance trace** when you have
  saved a trace (it is unavailable while a trace is recording or saving). Oscillate writes the bundle in the background and then opens
  its folder.
- From a terminal, without starting the window:

  ```sh
  oscillate --save-diagnostics              # prints the bundle's path
  oscillate --save-diagnostics --with-trace # also includes the newest trace
  ```

  The terminal command scans the standard VST3 folders first, in a separate
  process, so it lists your plug-ins even when the app does not start. It
  does not start logging or the crash handlers, so it leaves a running
  session alone.

## Where bundles are

`$XDG_STATE_HOME/oscillate/diagnostics/`, or
`~/.local/state/oscillate/diagnostics/` when `XDG_STATE_HOME` is unset. A
bundle is named `oscillate-diagnostics-<UTC time>.zip` and is readable only
by you. Bundles are kept until you delete them. See
[environment](environment.md#where-the-application-keeps-files).

## What a bundle contains

Exactly these files, uncompressed:

| File | Contents |
| --- | --- |
| `README.txt` | What the bundle is, when it was written, and which of the files below were left out and why. |
| `session.txt` | This session as a [crash report](crash-reports.md#what-a-report-contains) describes it: Oscillate's version and the Qt version, the time, the kernel, the distribution, the processor count, total memory and the environment variables that change how Oscillate runs. |
| `build.txt` | The build type and whether this build has each optional feature: the VST3 host, JACK output and real-time export, AIFF/FLAC/MP3 import and export, and export and source sample-rate conversion. |
| `plugins.txt` | The VST3 scan: for each plug-in its name, version and scan status, and for a bundle that failed to scan its folder name and why (for example `scan timed out`). In the app this is the VST3 browser's last scan in this session; the terminal command scans first. |
| `logs/` | The application log files, as a crash report includes them: `oscillate.4.log` to `oscillate.1.log`, then `oscillate.log`, the last 1 MiB of each. |
| `crash-report/` | The newest [crash report](crash-reports.md), if Oscillate has written one. |
| `trace/` | A performance trace, only when you asked for one: the newest `trace-<time>.json` in `$XDG_DATA_HOME/oscillate/traces/`. A trace over 512 MiB is left out. |

Your home folder's path (from `HOME`, or your account's entry when `HOME` is
unset) is replaced with `~` in every file, the trace included.

## What a bundle never contains

- Audio of any kind: no source media, recordings, project audio, renders,
  exports, caches or sample libraries.
- Project files, presets, grooves, racks or plugin state.
- A memory dump.

## Read it before you share it

The logs and the crash report record what the application did, so they name
projects, tracks, clips, files and plugins, and file paths outside your home
folder. The logs also record VST3 scan failures with the plug-in's path and
its error output. Open the bundle, read the files and remove anything you do
not want to share before attaching it.

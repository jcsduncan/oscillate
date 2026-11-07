# Crash reports

When Oscillate stops unexpectedly it saves a crash report on your computer.
Nothing is uploaded or sent anywhere, now or later. You decide whether to
attach the report to an issue.

## What happens

1. At startup Oscillate writes a short description of the session
   (`session.txt`, listed below) and installs handlers for the fatal signals
   SIGSEGV, SIGBUS, SIGFPE, SIGILL and SIGABRT.
2. On a crash the handler writes one line (the signal and the time) to a
   marker file, `crashed`, and nothing else; it then lets the process die as
   it would have. A Qt fatal message or an unhandled exception adds its
   message to the same marker first.
3. On the next start Oscillate finds the marker, gathers the report, removes
   the marker and shows a notice with the report's location, a button to open
   its folder and a button to open the
   [bug form](https://github.com/jcsduncan/oscillate/issues/new?template=bug_report.yml).
   Attach the report to the form. Closing the notice keeps the report.

Each crash produces one report. Reports are kept until you delete them.

## Where reports are

`$XDG_STATE_HOME/oscillate/crash-reports/`, or
`~/.local/state/oscillate/crash-reports/` when `XDG_STATE_HOME` is unset. A
report is one plain-text file named `oscillate-crash-<UTC time>.txt`, readable
only by you. See [environment](environment.md#where-the-application-keeps-files).

## What a report contains

Exactly these sections, in this order:

| Section | Contents |
| --- | --- |
| Crash | The signal number and name, any fatal-error message, the crash time and the time the report was written. |
| Session | Oscillate's version and the Qt version; when the session started; the kernel (`uname`), the distribution's name from `/etc/os-release`, the processor count and total memory; and the values of the environment variables listed below, as they were when the crashed session started. |
| Log | The application log files, oldest first: `oscillate.4.log` to `oscillate.1.log`, then `oscillate.log` (at most 5 MiB in all). |

Environment variables in the session section: every `OSCILLATE_*` variable,
and `XDG_SESSION_TYPE`, `XDG_CURRENT_DESKTOP`, `WAYLAND_DISPLAY`, `DISPLAY`,
`QT_QPA_PLATFORM`, `QT_SCALE_FACTOR`, `QT_QUICK_BACKEND`, `PIPEWIRE_LATENCY`,
`PIPEWIRE_QUANTUM`, `JACK_DEFAULT_SERVER`, `LANG` and `LC_ALL`. No other
variable is read.

Your home folder's path is replaced with `~` everywhere in the report.

## What a report never contains

- Audio of any kind: no source media, recordings, project audio, renders,
  exports, caches or sample libraries.
- Project files, presets, grooves, racks or plugin state.
- Performance traces. A trace recorded with `OSCILLATE_TRACE` is written only
  when Oscillate quits normally; attach a trace separately if you have one.
- A memory dump. The system may still write its own core dump; Oscillate does
  not read or include it.

## Read it before you share it

The log records what the application did, so it names projects, tracks,
clips, files and plugins, and file paths outside your home folder. Open the
report in a text editor and remove anything you do not want to share before
attaching it.

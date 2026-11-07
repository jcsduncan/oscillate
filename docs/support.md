# Getting help

Oscillate is supported through its public
[issue tracker](https://github.com/jcsduncan/oscillate/issues). Nothing is
sent from your computer unless you attach it to an issue yourself.

## Before you report

Check the [known issues](https://jcsduncan.github.io/oscillate/known-issues/),
built from open issues labelled `known-issue`. If your problem is listed, add
what is different about your case to that issue instead of opening a new one.

## Reporting a problem

Open an issue with the form that fits:

| Problem | Form |
| --- | --- |
| Something does not work as documented, or Oscillate crashed | [Bug report](https://github.com/jcsduncan/oscillate/issues/new?template=bug_report.yml) |
| Playback stutters, drops out or uses too much CPU | [Performance problem](https://github.com/jcsduncan/oscillate/issues/new?template=performance.yml) |
| A VST3 plug-in fails to scan, load or sound right | [Plugin compatibility](https://github.com/jcsduncan/oscillate/issues/new?template=plugin_compatibility.yml) |

For a security problem, follow the [security policy](../SECURITY.md) instead of
opening a public issue.

## What to attach

1. A [diagnostics bundle](diagnostics.md): run **Save diagnostics** in the
   command palette (Ctrl+K), or `oscillate --save-diagnostics` in a terminal
   if the app does not start. It holds the session description, build
   features, plug-in scan, logs and the latest crash report, with your home
   folder shown as `~`. Read it first.
2. After a crash, the [crash report](crash-reports.md) Oscillate offered at
   the next start. A diagnostics bundle includes the newest one.
3. For a performance problem, a performance trace: run **Start a
   performance trace** in the command palette, reproduce the problem, then
   **Stop and save the performance trace**, then save diagnostics with the
   latest performance trace (it becomes available once the trace has
   saved; reopen the palette if it was open during the save). With
   `OSCILLATE_TRACE` set, the trace is saved to that path instead and is not
   in the bundle: attach that file separately.
4. When you can, a small project that shows the problem. Do not attach media
   you are not allowed to share.

Also say what you did, what you expected and what happened, and your audio
setup (PipeWire or JACK, buffer size and sample rate).

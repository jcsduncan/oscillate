# Offline WAV master: bounded production slice

Both offline renderers preflight the read-only insert projection
(`project::project_insert_chains`) before opening an output writer. It
distinguishes built-in and VST3 instrument sources from audio-effect slots and
keeps the saved order of active master slots. Unresolved instrument roles and
unrendered VST3 effects fail explicitly; a saved missing VST3 instrument may be
resolved only by the stopped VST3 exporter's fresh binary and class check.

The offline renderer mixes managed audio clips and built-in instrument MIDI tracks into a stereo master. Saved main routes through group buses and sends to group or return buses use the shared project route compiler and engine stereo mixer. Send gains are linear. Built-in effect chains on audio, instrument, group and return tracks run in saved order, each stage adding its own latency and bounded tail. Enabled VST3 inserts and unsupported automation fail explicitly. It processes at the project sample rate in bounded blocks, includes instrument release tails, and writes float32, PCM24 or PCM16 WAV. An optional [output-rate adapter](output-sample-rate.md) converts the completed stream to a selected supported rate. Integer output uses deterministic dither. Publication is atomic: a read or write failure leaves any prior target intact; a directory sync failure after rename is reported as uncertain durability.

The renderer preflights all tracks and clips, pins verified managed sources through `ProjectStore`, compiles MIDI events from the authoritative tempo map, and rejects an unsupported path before publishing output. Supported audio sources are mono or stereo managed WAV, with optional encoded formats and source-rate conversion when those build paths are enabled. Processed (warped, pitched or stretched) clips render through `processing::OfflineWarpClip`; for an unprocessed clip, the converted source-range endpoints must span exactly the rendered clip duration on the project frame grid. The source-rate reader is worker-only and reads at most 1,024 project frames per call. The limits are 4,096 audio clips and 128 instruments. The renderer also renders built-in master effects in saved order after routed audio and instrument summation, processed WAV track stems and optional codec master export. Offline instances keep their state across blocks and never share live processor state. Unsupported automation and plugin paths fail explicitly rather than disappearing from a master.

A latent master or track stage, such as a limiter's lookahead, is
compensated: offline master and each processed stem render an additional
drain for the route and master latency after all preceding effect tails, and
the writer discards the leading latency frames, so the published first
transient and final effect tail keep their project positions. Output headers
and selected-rate conversion use this compensated duration. Overflow is
checked before opening the writer. A failed setting, processor guard or output
operation leaves an existing master or stem bundle intact. Repeated renders
are deterministic under the same snapshot and export settings. The direct live
graph rejects master effects; the routed live graph renders them.

Every built-in effect reports a bounded tail for its saved settings
(`EffectEngine::tail_frames`), and export renders that tail by processing zero
input through the same continuous instance. A non-finite or overflowing output
fails the render, leaving any previous target intact.

There are two export authorities. During live playback, the caller supplies an observed active graph revision matching the project snapshot; pending, failed or stale revisions are rejected. For a stopped project, the control sequencer acquires `StoppedExportAuthority` from its transport gate. This move-only authority binds project ID and revision and holds a lease through preflight, rendering and publication. The gate must prevent playback starting while the lease exists. No caller may synthesize an active graph status from a saved revision.

The desktop prepares a stable snapshot, independent read-only asset store and stopped authority on its control thread, then renders on an owned worker. It reports the exported revision and keeps the window open until an in-progress export finishes. The desktop's `ControlTransportGate` refuses a playback start while a stopped-export lease is held, and refuses the lease while playback runs.

## Quick export dialog

The top bar's **Export…** button, `Ctrl+Shift+E` and the command palette open one export dialog; nothing renders until its **Export** button is pressed. The dialog edits the bridge's single export target as a file name and folder (with a native **Choose…** dialog), the master format (WAV 32-bit float/24/16, and FLAC 24/16 and MP3 in codec builds), the shared output rate and the *Trim silence at the end* option. It shows progress while the worker runs and, on success, the published path with **Show in folder**. WAV stems and JACK real-time capture sit under *More exports*. An offline export keeps running when the dialog closes.

When a project opens, the proposed target is `<Music>/oscillate/<project name>.<ext>` (Qt's Music location, else the home folder); the folder is created when the export starts. A proposed name is never used for an existing file: it becomes `<project name> 2.<ext>`, `3` and so on, re-checked when the dialog opens, when the format changes, when the export starts and after each export. A name or folder the user typed or chose is used as given; the dialog warns when it will replace an existing file, and the native chooser asks before replacing one.

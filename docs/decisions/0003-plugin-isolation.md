# ADR-0003 — Plugin scanning and execution boundaries

**Status:** Accepted; implemented for scanning (reviewed 20 October 2026). VST3 scanning runs in a forked helper with failure counting and quarantine (`src/plugins/vst3_scan.cpp`, `scan_protocol.cpp`). Hosting is in-process; the optional per-plugin isolation route is not built.
**Date:** 14 October 2026
**Requirements:** PRD §§7, 16, 21; FS-04 PI-01/PI-05.

## Context

Third-party plugin discovery can crash or hang. Running every plugin in another process increases communication and scheduling cost. In-process plugins can still crash the application or violate real-time constraints; no host can promise complete containment of arbitrary plugin faults.

## Decision

VST3 scanning and metadata validation run in a timeout-bounded helper process. Failures are recorded per plugin; repeated failures quarantine the plugin until explicit retry. Normal playback hosts validated plugins in-process. An opt-in/per-plugin isolated execution path may be introduced for known-problematic plugins after latency and state-restoration measurements. The UI labels it best-effort and reports added latency or unsupported behaviour. A plugin failure preserves its project placeholder/state; recoverable faults bypass/mute and notify. Fatal in-process faults rely on autosave/recovery, not a false guarantee of survival.

The host adapter is the only processing code exposed to VST3 SDK types. Native effects and the bundled sampler implement the generic processor API. A testable `plugin_ui` target under `src/app/plugin_ui/` implements the UI-thread Linux VST3 run loop and translates editor window handles; `app` instantiates it. X11 attachment is the initial path; Wayland/XWayland bridging and optional native Wayland interfaces are tested before support is claimed. Failed attachment retains a generic parameter editor with a reason. The exact SDK version and packaged files are separately approved under G8.

## Alternatives considered

- Scan plugins in the application process: a malformed binary can take down project opening or stall the UI.
- Isolate every plugin's audio processing: stronger process separation, but latency, scheduling and state-transfer cost threaten the 64-sample target.
- Require a custom editor for every plugin: incompatible Linux editor implementations would make usable DSP unavailable; the generic parameter view is the supported fallback.

## Consequences and evidence

The default path avoids mandatory interprocess audio scheduling. The compatibility suite covers scanner crash/hang, quarantine/retry, GUI, parameter events, state save/reopen, missing plugin, and isolated-host latency where supported. Known failures remain published release data.

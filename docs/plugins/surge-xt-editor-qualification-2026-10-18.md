# Surge XT native-editor qualification, 18 October 2026

This is a PI-03 editor-host observation for the installed Surge XT 1.3.4
instrument and Effects binaries. It extends the [host compatibility
check](surge-xt-compatibility-2026-10-18.md); it does not certify every VST3,
Wayland editor, or live project edit.

| Item | Value |
| --- | --- |
| OS and kernel | Ubuntu 26.04.1 LTS, Linux 7.0.0-38-generic x86-64 |
| Window system | Local Xvfb 21.1.22 on display `:98`, 1600×900×24; Qt `xcb` |
| Instrument | `/usr/lib/vst3/Surge XT.vst3`, class `abcdef019182faeb566d624153675854`, SHA-256 `b8584398f314819241ca7287117d5559ed838d37ef08faab9847f27ba2993cec` |
| Effects | `/usr/lib/vst3/Surge XT Effects.vst3`, class `abcdef019182faeb566d624153465854`, SHA-256 `9b2da5f1b4a81a02ad55c88eba55257eb4a7f1747a1fd0866b8eaf5b7c8d0f45` |
| Host | Qualification branch based on `47c4ff4` |

The opt-in `oscillate_vst3_editor_probe` scanned each bundle out of process,
verified its binary receipt before loading, then opened and closed the actual
native editor five times in one host instance. All ten attach, visible
event-loop, detach and reopen cycles passed. The instrument requested 913×569
content in a 937×593 host shell; Effects requested 600×490 in a 624×514
shell. Both fit the host's 1200×800 maximum viewport. This confirms size
clamping for these actual requested sizes, not a test of Surge requesting an
oversized editor. The synthetic contract separately checks that oversized
content scrolls rather than expanding the shell.

An XTest drag across the instrument's Pitch control at shell coordinates
`(75,225)` to `(115,225)` generated one completed parameter gesture through
the plugin callback. The probe also injected a Tab key after the drag, but
Xvfb's input focus remained `PointerRoot` (window ID 1), with no window
manager. There is no demonstrated editor keyboard response: **keyboard/focus
compatibility remains unverified**. The Effects editor defaulted to `Off`;
an attempted drag of its inactive first knob produced no gesture. Effects
mouse editing still needs a selected effect and explicit control test.

The instrument's five editor cycles and pointer gesture passed while a
separate process performed ten 128-frame Surge cutoff sweeps through the
existing `oscillate_vst3_live_latency_probe`; all ten process runs passed.
This proves only that independent host instances can coexist during the
isolated check. It does not exercise the same instance's editor while a
physical audio device or the DAW render graph runs, and it does not measure
callback headroom or audible response. No physical output was opened.

The focused `plugin_ui_run_loop_contract` and
`plugin_ui_x11_attach_contract` also passed under the same Xvfb display. No
production editor-host change was needed. Native Wayland, XWayland under a
real desktop compositor, high-DPI behavior, real-project save/reopen after
an editor gesture and broader plugin coverage remain open.

## Reproduce

Build the opt-in target in a configured Quick worktree and use an isolated
X11 server. This command assumes Xvfb is available on the machine:

```sh
mkdir -p build/quick/scratch
TMPDIR="$PWD/build/quick/scratch" cmake --build build/quick \
  --target oscillate_vst3_editor_probe oscillate_vst3_live_latency_probe
Xvfb :98 -screen 0 1600x900x24 -nolisten tcp -noreset &
xvfb_pid=$!
DISPLAY=:98 QT_QPA_PLATFORM=xcb TMPDIR="$PWD/build/quick/scratch" \
  build/quick/tests/app/plugin_ui/oscillate_vst3_editor_probe \
  '/usr/lib/vst3/Surge XT.vst3' abcdef019182faeb566d624153675854 \
  instrument --drag 75 225 115 225
kill "$xvfb_pid"
```

Run Effects with its bundle and class ID above and `effect` as the kind,
without `--drag`. The optional `OSCILLATE_EDITOR_PROBE_SCREENSHOT` environment
variable writes a first-cycle PNG to a path you choose inside `build/quick`.
This probe runs third-party code in-process, so use a fresh process per class.

# Surge XT native editor to routed playback smoke

**Updated:** 19 October 2026. **Scope:** opt-in X11 development evidence for PI-03 and the scoped PI-09 manual edit.
The test target is excluded from the default build and from CTest. It does not
open a physical audio device. It is not a release or plugin-wide qualification.

The test uses the installed Surge XT Synth 1.3.4 class
`abcdef019182faeb566d624153675854`, with binary SHA-256
`b8584398f314819241ca7287117d5559ed838d37ef08faab9847f27ba2993cec`.
It scans out of process and refuses an absent or changed binary before loading
third-party code in the Quick process. An isolated Xvfb display runs Qt `xcb`,
the real `DawShell.qml`, its `WindowContainer`, and the native editor. The audio
output is a test-owned callback thread that renders the real routed graph
without connecting to PipeWire, JACK, or a physical sink.
The opt-in target is present only when X11 development files are available;
normal Quick configuration does not require them for this smoke.

| Boundary | Observation |
| --- | --- |
| Pointer input and visible knob | XTest held a drag on the pinned editor's Filter 1 Cutoff at shell-local `(375,225)` to `(175,225)`. Before/held native-window screenshots differ in more than 200 pixels within the cutoff slider/value region (1,217 pixels in the first local capture); the displayed cutoff changed from about 17.2 kHz to 309 Hz. The final saved reference is parameter `1945359057`, normalized `0.414286`. This verifies that the knob visibly moved on the tested editor layout. |
| In-progress gesture and audio | The fixture starts with Filter 1 enabled (`A Filter 1 Type`, parameter `1537728286`, normalized `0.1`) and its cutoff at `0.95`. Sixteen repeated C4 notes and a one-bar loop give two unchanged audio windows and a third while the mouse remains down. On 64-frame blocks, average absolute output was `0.06861` before and `0.01946` while held; at 128 frames it was `0.06861` and `0.01968`. Both unchanged baseline windows agreed within 20%, and the test requires the held-drag level to fall below 70% of baseline. The project revision stayed unchanged while held and playback remained active. This demonstrates an audible preview on the routed instance before `endEdit`; it does not assign a sample-accurate time to the preview. |
| Completed gesture | Releasing the mouse caused one project revision, 20 → 21. The native editor reported “Revision 21 live from sample …” after the fake output's real routed callback consumed the committed event; playback remained active. Closing the editor created no second revision. |
| Persistence | A concurrent reader loaded the just-committed project generation. After editor close and stop, reopen restored the exact plugin identity, parameter references, component-state bytes, controller-state bytes (where present), and revision from that generation. The test compares the complete saved `Vst3PluginInstance`, not just the cutoff value. |

This result is distinct from the [whole-graph allocation probe](surge-xt-routed-live-graph-2026-10-18.md), whose gate remains **red**: the installed Surge binary allocated on the render thread. The smoke does not widen the admitted parameter set, establish callback headroom, test arbitrary patches or controls, or prove native Wayland behavior. The active plugin cannot expose its opaque state pair during real-time processing, so the callback receipt establishes the active revision and the reader/reopen comparison establishes the exact saved pair; byte equality between a running plugin's private state and the suspended editor clone is not asserted.

## Reproduce

Build only the explicit target in a configured Quick build. Keep scratch files
under that build tree. Use an isolated X11 display; the example shows the
locally extracted Xvfb executable used for this observation.

```sh
mkdir -p build/quick/scratch
TMPDIR="$PWD/build/quick/scratch" cmake --build build/quick \
  --target oscillate_quick_surge_native_live_smoke
"$HOME/oscillate/build/quick/scratch/xvfb/root/usr/bin/Xvfb" \
  :109 -screen 0 1600x900x24 -nolisten tcp -noreset &
xvfb_pid=$!
DISPLAY=:109 QT_QPA_PLATFORM=xcb QT_QUICK_BACKEND=software \
  QML_IMPORT_PATH="$HOME/.cache/oscillate-local-deps/qt/usr/lib/x86_64-linux-gnu/qt6/qml" \
  TMPDIR="$PWD/build/quick/scratch" timeout 35s \
  build/quick/apps/daw_quick/oscillate_quick_surge_native_live_smoke \
  '/usr/lib/vst3/Surge XT.vst3' "$PWD/build/quick/scratch" 64
DISPLAY=:109 QT_QPA_PLATFORM=xcb QT_QUICK_BACKEND=software \
  QML_IMPORT_PATH="$HOME/.cache/oscillate-local-deps/qt/usr/lib/x86_64-linux-gnu/qt6/qml" \
  TMPDIR="$PWD/build/quick/scratch" timeout 35s \
  build/quick/apps/daw_quick/oscillate_quick_surge_native_live_smoke \
  '/usr/lib/vst3/Surge XT.vst3' "$PWD/build/quick/scratch" 128
kill "$xvfb_pid"
```

Both runs printed
`PASS native_editor_drag_commit_receipt_reopen revision=21 cutoff=0.414286`.
The coordinate is a pinned test fixture for this binary and UI size, not a
mapping in production code. A different Surge version, binary, layout or theme
must be assessed separately. An absent bundle or hash mismatch exits nonzero.

The enabled-filter patch is prepared through saved VST3 parameter references,
not a plugin-specific host path. A physical-output listening test and
whole-graph allocation/headroom qualification remain separate gates.
Set `SURGE_CAPTURE_IMAGES=1` to keep `surge-before.png` and `surge-held.png`
under `BUILD_SCRATCH` for manual visual inspection.

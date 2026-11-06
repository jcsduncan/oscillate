# Surge XT native-editor input under isolated Wayland/XWayland

**Scope:** PI-03 mouse input at the Qt Quick `WindowContainer` boundary. This
check did not use the active desktop, a physical audio device, or a DAW project.

## Environment

| Item | Evidence |
| --- | --- |
| Host | Ubuntu 26.04.1 LTS, x86-64, Qt 6.10 build |
| Isolated display | Xvfb `:119` → Weston 14.0.2 X11 backend with Pixman → its XWayland `:2` |
| Qt platform for native editor | `QT_QPA_PLATFORM=xcb` connected to the nested XWayland display |
| Instrument | Installed Surge XT Synth 1.3.4, class `abcdef019182faeb566d624153675854`, SHA-256 `b8584398f314819241ca7287117d5559ed838d37ef08faab9847f27ba2993cec` |
| Host shape | Suspended VST3 host, editor shell in a Qt Quick `WindowContainer`, five attach/detach cycles per process |

Weston, Xvfb and their missing libraries were downloaded as Ubuntu packages
and unpacked under this worktree's ignored `build/quick/scratch`; no system
package was installed. Weston used a private runtime socket and XWayland
display. The probe sent XTest drags to Surge's Pitch control at shell-local
coordinates `(75,225)` → `(115,225)` and back.

The first drag immediately after the nested compositor launched produced no
completed plugin gesture. A separate XTest self-check proved that XWayland
could deliver pointer events to a newly mapped X11 window. The first
compositor instance later exited abnormally, so that initial failure is not
attributed to the DAW editor host. After restarting Weston on its X11 backend,
the first editor probe again produced no gesture. A subsequent probe showed
the pointer over Surge's nested X11 child and completed the same gesture.
Three further consecutive probe processes passed, each with five editor
attach/detach cycles. Parameter `643940465` moved from normalized `0.5` to
`0.553571`, then returned to `0.5` on the reverse drag. The focused X11 and
offscreen plugin-UI contracts passed.

With `QT_QPA_PLATFORM=wayland` on the private Weston socket, the editor
contract passed its explicit `native_wayland_unavailable` fallback. The
current production host supports X11 editor embedding through XWayland;
native Qt Wayland does not attach a VST3 editor and the app offers its generic
parameter view with a reason.

## Interpretation and limits

The successful repeated drags show that an embedded Surge editor can receive
pointer input through Qt Quick on a nested Wayland/XWayland stack. The two
first-attempt misses suggest a startup or mapping timing issue in this test
setup, but do not establish a production defect. No change to editor input
forwarding is justified without a repeatable failure in the DAW's editor
overlay. This check does not cover a GNOME Wayland compositor,
physical mouse gestures, keyboard focus, the same plugin instance during
active DAW playback, or Surge's other controls.

To repeat against an installed Weston/Xvfb setup, keep the compositor and
XWayland display isolated and run the opt-in probe from a configured Quick
build. The display number below is an example; use the one printed by Weston.

```sh
mkdir -p build/quick/scratch
TMPDIR="$PWD/build/quick/scratch" cmake --build build/quick \
  --target oscillate_vst3_editor_probe oscillate_plugin_ui_tests
DISPLAY=:2 QT_QPA_PLATFORM=xcb TMPDIR="$PWD/build/quick/scratch" \
  build/quick/tests/app/plugin_ui/oscillate_vst3_editor_probe \
  '/usr/lib/vst3/Surge XT.vst3' abcdef019182faeb566d624153675854 \
  instrument --drag 75 225 115 225 --suspend --embed
XDG_RUNTIME_DIR=/path/to/private/runtime WAYLAND_DISPLAY=w1 \
  QT_QPA_PLATFORM=wayland TMPDIR="$PWD/build/quick/scratch" \
  build/quick/tests/app/plugin_ui/oscillate_plugin_ui_tests --wayland
```

No public API, project schema, real-time path, dependency or production
editor-host behavior changed. The opt-in probe now prints its X11 pointer
target path so an input miss can be distinguished from a plugin gesture miss.

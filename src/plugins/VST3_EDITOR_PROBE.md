# VST3 editor capability probe

**Status:** Host-side preflight only; PI-03 custom-editor acceptance remains open.

`Vst3EffectHost::probe_editor()` runs on the host's UI/control owner, never in
`process()` or concurrently with host lifecycle/state calls. It asks the
controller for an `editor` view, checks the X11 XEmbed platform type, and
releases the unattached view. The result `x11_candidate` means only that the
plug-in reports support. It does not mean an editor has attached or rendered.
A missing controller, missing view, rejected platform or caught C++ exception
returns a distinct generic-editor fallback reason. An editor C++ exception
does not mark the audio processor faulted; a native crash remains outside this
containment and relies on project recovery.

Native Wayland always returns `wayland_bridge_unavailable` before creating a
view. The pinned SDK's Wayland platform requires a `wl_surface` and
`IWaylandHost` connection service, neither of which this host owns. XWayland
must be treated as an X11 XEmbed surface and then follow the X11 path, not as
a native Wayland surface. The probe cannot infer the actual display backend
from environment variables. The future UI bridge must supply the Linux
`IRunLoop`, native window owner, attach/detach ordering, resize and focus
handling, and UI-thread teardown. A failed attach must retain the generic
parameter editor and report its reason. The view must be detached and released
before controller and module destruction. The audio callback must never wait
for this lifecycle.

The synthetic controller test covers no controller, null view, unsupported
X11, X11 candidate, native Wayland fallback, exception containment, view
release and zero `attached()` calls. These are contract tests, not X11/Wayland
session or third-party compatibility tests. Before PI-03 closes, test the
actual bridge on Ubuntu 26.04 in both display sessions with two reference
plugins, UI timers/fd handlers, repeated open/close, failed attach, project
reopen and active playback. Record plugin versions and exact fallback reasons.

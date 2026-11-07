# VST3 component/controller connection-point host slice

**Scope:** distinct controller and processor objects in the pinned
`v3.8.1_build_84` SDK host. Project binding still rejects saved normalized
values without component state; this change does not publish a restored graph.

The host queries `IConnectionPoint` from both objects after they have been
initialized and the controller has its host handler. If both endpoints exist,
it connects the component to the controller, then the controller to the
component. If either endpoint is absent, it leaves both unconnected and
releases any acquired interface references at teardown. A rejected or thrown
connection prevents host creation. The destructor attempts to disconnect
every endpoint whose connection was attempted, in reverse order, before
terminating or releasing either object. This also breaks peer reference cycles
when a plug-in retains the endpoint it receives.
For a distinct controller, saved static-value staging additionally requires
both successful connections; an unpaired controller is never treated as ready
for that restore path.

The deterministic SDK factory fixture has distinct component and controller
objects. It checks connection and disconnection order, messages in both
directions after connection, balanced object lifetimes when either connect
fails, absence of a half-connection when one interface is unavailable, and
unchanged allocation-free audio processing. Its peers take and release
interface references, so a missing disconnect would fail the lifetime check.

The [Steinberg API lifecycle example](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical%2BDocumentation/API%2BDocumentation/Index.html)
shows the two connection calls after initialization. The
[IConnectionPoint reference](https://steinbergmedia.github.io/vst3_doc/vstinterfaces/classSteinberg_1_1Vst_1_1IConnectionPoint.html)
assigns connect/disconnect/notify to the UI thread. The caller must enforce
that thread ownership for construction and teardown; this C++ host has no UI
dispatcher. Connections and private messages are never invoked by the host's
audio callback.

The in-process fixture is linked directly to the host's factory seam. A
separate source-only [dynamic bundle fixture](vst3-connected-bundle.md) now
exercises the scanner receipt and verified loader with both endpoints. Neither
fixture proves third-party plug-in compatibility. Malformed endpoint returns
and connection-time exceptions remain compatibility cases. Static-value
project binding also still needs the UI dispatcher and graph publication
handoff described in
[the restore gate](vst3-static-parameter-restore.md).

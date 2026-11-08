# Export insert contract

The offline master and stem renders read
`Track::instrument_source`, each track's `effects`, and `Snapshot::master_effects`.
They do not retain a second list of processing settings. Before opening an output
writer, each export validates the saved snapshot. The ordinary native master/stem
renderer accepts built-in effect slots on audio, built-in-instrument, group and
return paths, across multiple paths. Enabled slots run in saved order; bypassed
built-in slots retain their identity without processing. A built-in effect's
latency contributes to that path's delay compensation.
The ordinary native renderer treats a bypassed VST3 effect on an audio,
built-in instrument, group or return path as an inert saved slot. It
needs no plugin binary, even when marked missing, and does not consume a
native stage. Native stages before and after it keep their saved order. An enabled VST3 effect,
including an unbound or missing one, still rejects the ordinary native
master/stem render before output publication; mixed active native/VST3 chains
are not admitted. The renderer rejects enabled VST3 master slots; a bypassed
VST3 master slot remains saved but contributes no processing or binary need.
The native exporter removes bypassed VST3 effects and their inactive parameter
lanes only from its private route compilation view; the saved snapshot, lanes
and slot identities are unchanged.
Given a VST3 host's stage preparer (`render_graph::Vst3StagePreparer`,
supplied by [VST3 offline export](vst3-offline-export.md)), the same renderer
admits every saved chain: enabled VST3 effects on any track or bus, mixed with
built-in effects in any order, and VST3 master slots, each prepared as a chain
stage (ADR-0050), and VST3 instrument sources as their chains' first stages.
Without one, the native exporter rejects VST3 instrument sources. A saved
`missing` flag may be cleared by a successful fresh scan of the plugin's exact
binary and class identity; a missing plugin is never silently omitted.

Rejected master, stem and encoded jobs preserve an existing published output.

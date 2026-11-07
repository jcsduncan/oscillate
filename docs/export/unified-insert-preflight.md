# Schema 1.13 export insert contract

The ordinary offline master/stem and stopped VST3 instrument/mixed exporters read
`Track::instrument_source`, each track's `effects`, and `Snapshot::master_effects`.
They do not retain a second list of processing settings. Before opening an output
writer, each export validates the saved snapshot. The ordinary native master/stem
renderer accepts up to two native EQ, compressor, algorithmic reverb or limiter
slots per audio, group or return path, across multiple paths. Enabled slots run
in saved order; bypassed native slots retain their identity without processing.
A native limiter contributes its lookahead to that path's delay compensation.
The ordinary native renderer treats a bypassed VST3 effect on an audio,
sampler/native-instrument, group or return path as an inert saved slot. It
needs no plugin binary, even when marked missing, and does not consume a
native stage. Native stages before and after it keep their saved order and the
existing per-path native stage limit still applies. An enabled VST3 effect,
including an unbound or missing one, still rejects the ordinary native
master/stem render before output publication; mixed active native/VST3 chains
are not admitted. The renderer rejects enabled VST3 master slots; a bypassed
VST3 master slot remains saved but contributes no processing or binary need.
The native exporter removes bypassed VST3 effects and their inactive parameter
lanes only from its private route compilation view; the saved snapshot, lanes
and slot identities are unchanged.
The native exporter rejects
VST3 instrument sources; the VST3 exporter requires exactly one VST3 source
and rejects sampler or unresolved sources. A saved
`missing` instrument flag may be cleared by a successful fresh scan of its
exact binary and class identity; a missing effect is never silently omitted.

These restrictions describe the current renderer, not the product's final
insert scope. Mixed native/VST3 effect paths and broader VST3 chains still
have separate bounded export contracts. Expansion to arbitrary ordered
effects must add both live and offline evaluation before accepting those
chains. Rejected master, stem and encoded jobs preserve an existing published
output.

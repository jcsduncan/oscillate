# VST3 instrument release and fixed-tail evidence

`Vst3InstrumentTrackProvider` reports the loaded instrument's finite VST3 tail
declaration in `PreparedTrackInsert::finite_tail_frames`. That declaration is
not proof of release after MIDI note-off. Without a session-supplied
`Vst3InstrumentTailQualification`, `fixed_tail_qualified` stays false.

The receipt binds one scanned binary digest and byte size, class identity,
saved component bytes and parameter references, 48 kHz, the exact loaded tail
declaration, and completed note-off/release probes at **both** 64- and
128-frame callback sizes. A successful probe must send MIDI note-on and
note-off events, then show finite, repeatable output that falls below a stated
residual threshold by the declared release bound and remains below it through
an additional guard interval. It must repeat after fresh state restore and
project reopen, with no callback allocations or faults. It must include held
notes, overlapping notes, notes ending at block boundaries and the longest
supported MIDI clip. The record is control-side and session-local; no project
save carries it. A changed binary, state, parameters, rate, block size or tail
declaration invalidates it. Saved automation, admitted manual parameter edits
and offline host mode also prevent admission.

The synthetic TimedInstrument fixture has a declared eight-frame release.
The provider test observes its actual note-off and verifies silence after the
eighth release frame, no late output, callback allocation freedom, exact
save/reopen identity and invalidation cases at both block sizes. This fixture
validates the provider contract only. No third-party instrument, including
Surge XT, is production-qualified by it. A separate binary-specific runner,
reviewed evidence for every admitted state and control path, and a production
caller that prevents editor or other external mutation during capture are
still required. Quick real-time WAV export must consume the prepared route
receipt before it can use this bound; that integration is tracked separately.

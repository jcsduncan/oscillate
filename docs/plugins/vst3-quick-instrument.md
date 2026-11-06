# Qt Quick VST3 instrument slice

The Quick plugin drawer now filters scanned VST3 classes for the selected track:
effects for audio tracks and instruments for instrument tracks. An instrument
track must have no bundled sampler sound or existing VST3 insert. The current
live graph admits one saved VST3 processor in the project. The selected class
is freshly rescanned and its binary and class identity checked on a worker
before the undoable insert is saved. A project revision or project switch
during verification cancels the insert.

On Play, Quick scans the saved binary again and prepares the matching
`Vst3InstrumentTrackProvider` off the audio callback. The routed source accepts
MIDI clips only for that verified, bound instrument track. The callback uses
the shared compiled MIDI events and graph transport; Stop joins the output
before sending the instrument its stopped block to release held notes. A
missing or changed binary, changed class, wrong track kind, unsupported saved
state, second instrument source, or unbound MIDI track prevents playback and
leaves the saved project intact. The transport reports the failure. In a build
without the VST3 scanner, a saved insert is retained but playback refuses it.

This slice supports one stereo-output, single-event-input instrument with a
bounded stepped or gradual tempo map, at 64- or 128-frame requested buffers. It does not add
MIDI recording, plugin custom editors, dynamic state updates, multiple
instruments or broad third-party compatibility. The synthetic `TimedInstrument`
fixture validates save/reopen, first-note audio, wrong-kind and changed-binary
rejection and Stop ordering. It is not evidence that commercial instruments
or a physical device meet the release qualification workload.

Local GCC scanner-enabled Quick/VST3 tests passed 74/74, including the existing
effect browser and the new instrument UI test. After the final public insert
service track-kind guard, focused Quick routing, effect browser, instrument
browser and insert-service tests passed 4/4 under GCC and Clang. The
scanner-disabled build passed its focused routing and browser tests 2/2; saved
VST3 playback remains unavailable without the scanner. The same four focused
tests passed under AddressSanitizer and UndefinedBehaviorSanitizer with external
Qt leak detection disabled. In that focused coverage run, the VST3 insert
service reached 99% line coverage and Quick routed playback 91%. The shared
audio-source and graph-control files are exercised by additional suites and
are not represented by those focused component figures. Changed-file
clang-tidy and formatting passed.

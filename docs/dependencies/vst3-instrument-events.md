# VST3 instrument event primitive (local qualification)

Status: host primitive only; no project, MIDI clip, graph or Qt Quick wiring.

## Contract

`load_verified_vst3_instrument` uses the same scanned binary receipt and module lifetime guard as the effect loader. It accepts only one 32-bit stereo main audio output, no audio inputs, exactly one main event input with 1–16 channels, and no event outputs. Unsupported bus layouts fail during load. The existing effect loader remains effect-only.

`process_instrument` requires one exclusive render owner, an active host, equal 1–`max_block_frames` output spans, at most 256 events by default, valid channel/pitch/velocity, and nondecreasing sample offsets strictly within the block. It forwards note-on and note-off events to VST3 `IEventList` on bus 0 without sorting; equal-offset events retain caller order. Offset 0 applies to the first sample; an event at the block size belongs in the next block at offset 0. The event list borrows the caller's span only until `process` returns. Caller storage must remain valid throughout that call. No note queue or buffer allocation occurs during the host callback. Other VST3 event types, multiple event buses, multichannel/multi-output instruments and plugin MIDI output are outside this primitive.

A process failure faults the instance, silences its output and prevents subsequent plugin processing. The caller must suspend and destroy the instance off the audio callback. In-process plugins can still hang, crash or allocate internally; host allocation counters cannot prove plugin-internal real-time safety.

## Evidence

The generated source-only `TimedInstrument.vst3` fixture is scanned, receipt-verified, loaded and activated. An independent sample-by-sample note timeline checks both channels for 64- and 128-frame blocks, including a note at the final frame, same-offset note-off/note-on ordering, and state restoration audible on the first post-restore sample. The test also checks invalid offsets/order/channel/capacity, unsupported two-event-bus topology, wrong effect entry point, no C++ `new` in the host callback, and fault quarantine with silence. The fixture binary is generated under `build/` and is not committed.

This test uses deterministic synthetic audio, not a general VST3 compatibility claim or proof of performance on commercial instruments. The host now accepts block-start sample/quarter/tempo context and a transport generation, and tracks note-offs across stop, seek and resume; see the [instrument transport contract](../../src/plugins/VST3_INSTRUMENT_TRANSPORT.md). Before project integration, the shared MIDI scheduler must supply that context and generation, define note-ID ownership, overflow and graph-publication timing, and test loop and retrigger behaviour. The compatibility suite must still cover instrument state, timing, GUI and failure behaviour.

## SDK basis

Pinned SDK: VST3 SDK `v3.8.1_build_84`. The interface specifies block-relative `Event.sampleOffset`, event `busIndex`, `ProcessData.inputEvents` and `IEventList.getEvent`:

- [VST3 Event](https://steinbergmedia.github.io/vst3_doc/vstinterfaces/structSteinberg_1_1Vst_1_1Event.html)
- [VST3 ProcessData](https://steinbergmedia.github.io/vst3_doc/vstinterfaces/structSteinberg_1_1Vst_1_1ProcessData.html)
- [VST3 IEventList](https://steinbergmedia.github.io/vst3_doc/vstinterfaces/classSteinberg_1_1Vst_1_1IEventList.html)
- [VST3 dynamic I/O and event buses](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical%2BDocumentation/Change%2BHistory/3.0.0/Multiple%2BDynamic%2BIO.html)

## Local gates (15 October 2026)

- GCC and Clang C++20 builds: focused instrument fixture/test passed.
- GCC VST3 effect-host, verified loader, connected-bundle and instrument-bundle tests: 4/4 passed.
- ASan/UBSan and TSan focused host/instrument tests: 2/2 passed in each preset. TSan here checks the single-owner path; no concurrent dispatch is claimed.
- `clang-format --dry-run --Werror`, `clang-tidy` on changed C++ sources, and `git diff --check`: passed.
- Focused GCC coverage after all four tests: `vst3_effect_host.cpp` 90% (668/742 executable lines), `vst3_bundle_loader.cpp` 86% (136/157). This is a focused coverage report, not repository-wide coverage. The loader's unexecuted lines are mostly inherited defensive failure paths; the new instrument path has direct timing, validation, topology and fault tests.

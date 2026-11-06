#pragma once

#include "oscillate/export/offline_master.hpp"
#include "oscillate/plugins/vst3_scan.hpp"

#include <filesystem>

namespace oscillate::exporting {

// Bounded headless slice: one saved VST3 instrument source with optional bus/send
// routes, or two independently verified VST3 instrument sources with either
// bare paths or exactly one verified VST3 effect on each path and no mixed
// audio, or one bare native Prism/Ember synth beside
// one bare verified VST3 instrument. The first bundle argument names the first VST3
// source; a second source is freshly scanned from its saved bundle path.
// The plugin graph runs at the project rate; the master target may be
// WAV float32/PCM24/PCM16 or, in a codec build, FLAC16/24 or MP3, with
// optional output-rate conversion through the native master's output stage. The scanner runs afresh
// before any output is opened. This does not change or process a running live instance. The stopped
// authority holds the application's exclusive transport lease. A finite plugin-reported tail of at
// most ten seconds is required. Native master EQ, compressor and reverb follow the saved chain;
// unsupported slots and source-track shapes are rejected explicitly. Each
// paired effect path has independent serial latency and finite-tail accounting;
// their maximum governs shared graph alignment and output extent.
// The overloads without a bundle scan each source from its own saved path,
// then verify the scan against that source's saved binary and class identity.
[[nodiscard]] foundation::Result<PublishOutcome>
render_vst3_instrument_master(const project::Snapshot& snapshot, StoppedExportAuthority authority,
                              plugins::Vst3Scanner& scanner, const MasterOutputOptions& options);

[[nodiscard]] foundation::Result<PublishOutcome>
render_vst3_instrument_master(const project::Snapshot& snapshot, StoppedExportAuthority authority,
                              plugins::Vst3Scanner& scanner, const std::filesystem::path& bundle,
                              const MasterOutputOptions& options);

[[nodiscard]] foundation::Result<PublishOutcome>
render_vst3_instrument_wav_master(const project::Snapshot& snapshot,
                                  StoppedExportAuthority authority, plugins::Vst3Scanner& scanner,
                                  const std::filesystem::path& bundle,
                                  const MasterRenderOptions& options);

[[nodiscard]] foundation::Result<PublishOutcome>
render_vst3_instrument_wav_master(const project::Snapshot& snapshot,
                                  StoppedExportAuthority authority, plugins::Vst3Scanner& scanner,
                                  const MasterRenderOptions& options);

// For one source the processed stem follows the master's saved routing.
// With two sources, pass an explicit source track to select one processed stem.
[[nodiscard]] foundation::Result<PublishOutcome>
render_vst3_instrument_wav_stem(const project::Snapshot& snapshot, StoppedExportAuthority authority,
                                plugins::Vst3Scanner& scanner, const std::filesystem::path& bundle,
                                const MasterRenderOptions& options);

// Select a processed source stem when the project has two instrument tracks.
// The unselected source remains in the graph, muted, preserving route alignment.
[[nodiscard]] foundation::Result<PublishOutcome>
render_vst3_instrument_wav_stem(const project::Snapshot& snapshot, StoppedExportAuthority authority,
                                plugins::Vst3Scanner& scanner, const std::filesystem::path& bundle,
                                project::TrackId source_track, const MasterRenderOptions& options);

[[nodiscard]] foundation::Result<PublishOutcome>
render_vst3_instrument_wav_stem(const project::Snapshot& snapshot, StoppedExportAuthority authority,
                                plugins::Vst3Scanner& scanner, project::TrackId source_track,
                                const MasterRenderOptions& options);

// Mixed headless slice: the saved project also contains 1-8 plain audio
// tracks, each with 1-32 nonoverlapping managed WAV clips, from at
// most 64 distinct referenced sources. Master combines all sources through
// saved routing. A selected source stem runs only that track through the same
// route graph and is padded to the common aligned export duration.
// One bare native Prism/Ember source may join one verified VST3 instrument
// and exactly one managed WAV track, with saved main/send routes and at most
// one native master reverb. Other mixed-native shapes remain unqualified.
// One enabled, verified VST3 effect may process one separate audio track;
// its finite tail and measured latency are included in the stopped render.
// Other track/bus effects and effect chains fail before output publication.
// Each output is atomically published; this API does not publish a stem directory.
// Master and stem targets accept the same formats and output rates as above.
[[nodiscard]] foundation::Result<PublishOutcome>
render_vst3_mixed_master(const project::Snapshot& snapshot,
                         const oscillate::persistence::ProjectStore& store,
                         StoppedExportAuthority authority, plugins::Vst3Scanner& scanner,
                         const std::filesystem::path& bundle, const MasterOutputOptions& options);

[[nodiscard]] foundation::Result<PublishOutcome> render_vst3_mixed_wav_master(
    const project::Snapshot& snapshot, const oscillate::persistence::ProjectStore& store,
    StoppedExportAuthority authority, plugins::Vst3Scanner& scanner,
    const std::filesystem::path& bundle, const MasterRenderOptions& options);

[[nodiscard]] foundation::Result<PublishOutcome>
render_vst3_mixed_wav_stem(const project::Snapshot& snapshot,
                           const oscillate::persistence::ProjectStore& store,
                           StoppedExportAuthority authority, plugins::Vst3Scanner& scanner,
                           const std::filesystem::path& bundle, project::TrackId source_track,
                           const MasterRenderOptions& options);

} // namespace oscillate::exporting

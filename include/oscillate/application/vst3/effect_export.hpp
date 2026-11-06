#pragma once

#include "oscillate/export/offline_master.hpp"
#include "oscillate/plugins/vst3_scan.hpp"

#include <filesystem>

namespace oscillate::exporting {

// Experimental stopped-project export slice. Up to two audio tracks have
// bounded, unwarped managed WAV clips; one has one to three enabled, verified
// serial VST3 effects. Saved main/send routing, path delay compensation and isolated source
// stems are applied. Bus/master inserts, instruments, and non-send automation
// are rejected before opening the target. A finite reported plugin tail is
// required and the sum is capped at ten seconds. Effects are instantiated afresh; no
// running playback processor or state is reused.
// The plugin graph always runs at the project rate. WAV float32/PCM24/PCM16
// and, in a codec build, FLAC16/24 or MP3 targets share the native master's
// output stage: optional libsamplerate conversion to 44.1/48/88.2/96 kHz,
// TPDF dither for integer formats and atomic publication. Loudness reports
// are not produced on this path.
[[nodiscard]] foundation::Result<PublishOutcome>
render_vst3_effect_master(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
                          StoppedExportAuthority authority, plugins::Vst3Scanner& scanner,
                          const std::filesystem::path& bundle, const MasterOutputOptions& options);

[[nodiscard]] foundation::Result<PublishOutcome> render_vst3_effect_wav_master(
    const project::Snapshot& snapshot, const persistence::ProjectStore& store,
    StoppedExportAuthority authority, plugins::Vst3Scanner& scanner,
    const std::filesystem::path& bundle, const MasterRenderOptions& options);

[[nodiscard]] foundation::Result<PublishOutcome>
render_vst3_effect_wav_stem(const project::Snapshot& snapshot,
                            const persistence::ProjectStore& store,
                            StoppedExportAuthority authority, plugins::Vst3Scanner& scanner,
                            const std::filesystem::path& bundle, project::TrackId source_track,
                            const MasterRenderOptions& options);

} // namespace oscillate::exporting

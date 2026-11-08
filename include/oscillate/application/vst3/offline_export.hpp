#pragma once

#include "oscillate/export/offline_master.hpp"
#include "oscillate/plugins/vst3_scan.hpp"

namespace oscillate::exporting {

// Offline export of a project with saved VST3 stages (ADR-0050, ADR-0090):
// VST3 instruments and effects are chain stages. Each enabled VST3 instrument
// and effect, on any track, bus or the master, is scanned afresh from its
// saved bundle and checked against the saved binary and class before any
// output opens; the render is then the shared
// offline render (render_wav_master), with every VST3 stage prepared for
// VST3 offline processing in 128-frame host blocks. No running live instance
// is reused. Worker only; the stopped authority's lease is held throughout.
[[nodiscard]] foundation::Result<PublishOutcome>
render_vst3_master(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
                   StoppedExportAuthority authority, plugins::Vst3Scanner& scanner,
                   const MasterOutputOptions& options);

// Every source track's stem, as render_wav_stems publishes its bundle.
[[nodiscard]] foundation::Result<PublishOutcome>
render_vst3_stems(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
                  StoppedExportAuthority authority, plugins::Vst3Scanner& scanner,
                  const StemRenderOptions& options);

// One source track's stem, as render_wav_stem writes it.
[[nodiscard]] foundation::Result<PublishOutcome>
render_vst3_stem(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
                 StoppedExportAuthority authority, plugins::Vst3Scanner& scanner,
                 project::TrackId track, const MasterRenderOptions& options);

} // namespace oscillate::exporting

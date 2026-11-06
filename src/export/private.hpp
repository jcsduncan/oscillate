#pragma once

#include "oscillate/export/offline_master.hpp"

#include <functional>

namespace oscillate::exporting::detail {

// Fault seam for deterministic interruption tests. Production passes no hook.
[[nodiscard]] foundation::Result<PublishOutcome>
render_wav_master_with_start_hook(const project::Snapshot& snapshot,
                                  const persistence::ProjectStore& store,
                                  const LiveRevisionState& live, const MasterRenderOptions& options,
                                  const std::function<void()>& after_writer_begin);

// Fault seam after a completed private stem, before bundle publication.
[[nodiscard]] foundation::Result<PublishOutcome> render_wav_stems_with_after_stem_hook(
    const project::Snapshot& snapshot, const persistence::ProjectStore& store,
    const LiveRevisionState& live, const StemRenderOptions& options,
    const std::function<foundation::Result<void>(std::size_t)>& after_stem);

[[nodiscard]] foundation::Result<PublishOutcome> render_wav_master_with_start_hook(
    const project::Snapshot& snapshot, const persistence::ProjectStore& store,
    StoppedExportAuthority authority, const MasterRenderOptions& options,
    const std::function<void()>& after_writer_begin);

} // namespace oscillate::exporting::detail

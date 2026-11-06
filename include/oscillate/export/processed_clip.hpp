#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/processing/offline_warp_clip.hpp"
#include "oscillate/project/model.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace oscillate::persistence {
class ProjectStore;
} // namespace oscillate::persistence

namespace oscillate::exporting {

// A saved processed (warped, stretched or pitched) clip prepared for an
// offline export. `first` and `last` are its project frames from the
// authoritative clip frame plan; `reader` yields its Q0 output frames,
// with frame 0 at `first`.
struct ProcessedExportClip {
  std::uint64_t first = 0;
  std::uint64_t last = 0;
  std::unique_ptr<processing::OfflineWarpClip> reader;
};

// Shared by master, processed-stem and VST3 effect/instrument export. Empty
// means the clip plays its source unprocessed: the caller keeps its existing
// raw reader. Otherwise the source is pinned through the store's verified
// managed descriptor (never reopened by path) and rendered at Q0 from the
// source alone, with no cache dependency. Failures are explicit.
[[nodiscard]] foundation::Result<std::optional<ProcessedExportClip>>
prepare_processed_export_clip(const project::Snapshot& snapshot,
                              const persistence::ProjectStore& store, const project::Clip& clip);

} // namespace oscillate::exporting

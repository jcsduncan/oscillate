#pragma once

#include "oscillate/processing/warp_map.hpp"
#include "oscillate/project/model.hpp"

#include <cstdint>
#include <span>
#include <variant>

namespace oscillate::processing {

using ClipWarpMarker = project::ClipWarpMarker;

struct ClipFramePlan {
  std::uint64_t project_start_frame;
  std::uint64_t source_start_frame;
  PreparedWarpMap warp;

  [[nodiscard]] std::uint64_t project_end_frame() const noexcept {
    return project_start_frame + warp.output_frames();
  }
};

// Control/worker side only. Markers exclude the two clip endpoints, which come
// from the clip's source range and placement. With no explicit markers, use
// the saved reviewed markers; a clip with none gives one segment.
[[nodiscard]] foundation::Result<ClipFramePlan>
prepare_clip_frame_plan(const project::Clip& clip, const project::SourceRecord& source,
                        const timeline::TempoMap& tempo, std::uint32_t project_rate,
                        std::span<const ClipWarpMarker> markers = {});

} // namespace oscillate::processing

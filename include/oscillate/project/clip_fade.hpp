#pragma once

#include "oscillate/project/model.hpp"

#include <cstdint>

namespace oscillate::project {

struct PreparedClipFades {
  std::uint64_t duration_frames;
  std::uint64_t in_frames;
  std::uint64_t out_frames;
  FadeCurve in_curve;
  FadeCurve out_curve;
};

// Control/worker preparation. Fade fractions are relative to the clip's
// rendered project-sample interval, so all renderers share the same boundaries.
[[nodiscard]] foundation::Result<PreparedClipFades>
prepare_clip_fades(const ClipFades& fades, std::uint64_t duration_frames);

// Bounded, allocation-free callback/export evaluation for one clip-local frame.
[[nodiscard]] double clip_fade_gain(const PreparedClipFades& fades,
                                    std::uint64_t local_frame) noexcept;

} // namespace oscillate::project

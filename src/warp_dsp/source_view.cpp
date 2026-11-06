#include "oscillate/warp_dsp/source_view.hpp"

#include <limits>

namespace oscillate::warp_dsp {

bool MemorySourceView::read(std::int64_t first, std::span<float> left,
                            std::span<float> right) noexcept {
  if (left.size() != right.size() || right_.size() != left_.size())
    return false;
  const auto frames = static_cast<std::uint64_t>(left_.size());
  const auto negative_magnitude = first < 0 ? static_cast<std::uint64_t>(-(first + 1)) + 1 : 0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto offset = static_cast<std::uint64_t>(index);
    std::uint64_t position = 0;
    bool inside = false;
    if (first < 0) {
      if (offset >= negative_magnitude) {
        position = offset - negative_magnitude;
        inside = position < frames;
      }
    } else if (offset <=
               std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(first)) {
      position = static_cast<std::uint64_t>(first) + offset;
      inside = position < frames;
    }
    left[index] = inside ? left_[static_cast<std::size_t>(position)] : 0.0F;
    right[index] = inside ? right_[static_cast<std::size_t>(position)] : 0.0F;
  }
  return true;
}

} // namespace oscillate::warp_dsp

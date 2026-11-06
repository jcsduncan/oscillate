#pragma once

#include "oscillate/warp_dsp/source_view.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace oscillate::warp_dsp::detail {

// Preallocated contiguous window of planar source frames. ensure() slides
// forward by moving retained frames and reading only new ones; backwards or
// distant jumps refill. No allocation after construction.
class SourceWindow {
public:
  explicit SourceWindow(std::size_t capacity_frames);

  void reset() noexcept { filled_ = 0; }
  [[nodiscard]] std::size_t capacity() const noexcept { return left_.size(); }
  // Makes frames [first, first + count) resident. False on read failure or
  // when count exceeds capacity.
  [[nodiscard]] bool ensure(SourceView& source, std::int64_t first, std::size_t count) noexcept;
  // Pointers to frame `first` (absolute source frame) after ensure().
  [[nodiscard]] const float* left(std::int64_t first) const noexcept {
    return left_.data() + (first - base_);
  }
  [[nodiscard]] const float* right(std::int64_t first) const noexcept {
    return right_.data() + (first - base_);
  }

private:
  std::vector<float> left_;
  std::vector<float> right_;
  std::int64_t base_ = 0;
  std::size_t filled_ = 0;
};

} // namespace oscillate::warp_dsp::detail

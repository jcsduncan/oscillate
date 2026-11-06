#include "source_window.hpp"

#include <algorithm>
#include <span>

namespace oscillate::warp_dsp::detail {

SourceWindow::SourceWindow(std::size_t capacity_frames)
    : left_(capacity_frames, 0.0F), right_(capacity_frames, 0.0F) {}

bool SourceWindow::ensure(SourceView& source, std::int64_t first, std::size_t count) noexcept {
  const auto capacity = left_.size();
  if (count > capacity)
    return false;
  const auto end = first + static_cast<std::int64_t>(count);
  const auto resident_end = base_ + static_cast<std::int64_t>(filled_);
  if (filled_ != 0 && first >= base_ && end <= resident_end)
    return true;
  // Read ahead to fill the window so forward streaming reads in large blocks.
  if (filled_ != 0 && first >= base_ && first < resident_end) {
    const auto keep = static_cast<std::size_t>(resident_end - first);
    const auto offset = static_cast<std::size_t>(first - base_);
    std::copy(left_.begin() + static_cast<std::ptrdiff_t>(offset),
              left_.begin() + static_cast<std::ptrdiff_t>(offset + keep), left_.begin());
    std::copy(right_.begin() + static_cast<std::ptrdiff_t>(offset),
              right_.begin() + static_cast<std::ptrdiff_t>(offset + keep), right_.begin());
    base_ = first;
    filled_ = keep;
  } else {
    base_ = first;
    filled_ = 0;
  }
  const auto missing = capacity - filled_;
  if (!source.read(base_ + static_cast<std::int64_t>(filled_),
                   std::span(left_).subspan(filled_, missing),
                   std::span(right_).subspan(filled_, missing))) {
    filled_ = 0;
    return false;
  }
  filled_ = capacity;
  return true;
}

} // namespace oscillate::warp_dsp::detail

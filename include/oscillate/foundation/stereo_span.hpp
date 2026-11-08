#pragma once

#include <cstddef>
#include <span>
#include <type_traits>

namespace oscillate::foundation {

// A stereo pair of channel buffers of equal length, passed as one value so
// the two channels cannot be swapped at a call. Build it with designated
// initialisers: `StereoSpan<float>{.left = l, .right = r}`.
template <typename T> struct StereoSpan {
  std::span<T> left;
  std::span<T> right;

  [[nodiscard]] constexpr std::size_t size() const noexcept { return left.size(); }
  [[nodiscard]] constexpr StereoSpan first(std::size_t count) const noexcept {
    return {.left = left.first(count), .right = right.first(count)};
  }
  [[nodiscard]] constexpr StereoSpan subspan(std::size_t offset) const noexcept {
    return {.left = left.subspan(offset), .right = right.subspan(offset)};
  }
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): std::span::subspan's order.
  [[nodiscard]] constexpr StereoSpan subspan(std::size_t offset, std::size_t count) const noexcept {
    return {.left = left.subspan(offset, count), .right = right.subspan(offset, count)};
  }
  // Writable channels read as read-only ones.
  constexpr operator StereoSpan<const T>() const noexcept
    requires(!std::is_const_v<T>)
  {
    return {.left = left, .right = right};
  }
};

} // namespace oscillate::foundation

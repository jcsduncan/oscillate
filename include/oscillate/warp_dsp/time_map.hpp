#pragma once

#include "oscillate/foundation/error.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace oscillate::warp_dsp {

// Source positions use Q48.16 fixed point: 65536 units per source frame.
inline constexpr std::uint32_t source_fraction_bits = 16;
inline constexpr std::uint64_t source_unit = std::uint64_t{1} << source_fraction_bits;
// Bounds keep quotient/remainder evaluation, including splice extrapolation,
// inside unsigned 64-bit products.
inline constexpr std::uint64_t max_segment_output_frames = std::uint64_t{1} << 31;
inline constexpr std::uint64_t max_source_position = std::uint64_t{1} << 60;
// Far above any mode's range; bounds splice extrapolation products.
inline constexpr std::uint32_t max_map_speed_bits = 10;
inline constexpr std::size_t max_time_map_segments = std::size_t{1} << 20;

// One output interval [output_begin, output_end) that reads source linearly
// from source_begin towards source_end (exclusive end positions, Q48.16).
// A splice segment starts at a source discontinuity (loop join or slice); a
// non-splice segment must continue exactly where its predecessor ended.
struct TimeMapSegment {
  std::uint64_t output_begin = 0;
  std::uint64_t output_end = 0;
  std::uint64_t source_begin = 0;
  std::uint64_t source_end = 0;
  bool splice = false;
  bool operator==(const TimeMapSegment&) const = default;
};

// Immutable output-to-source map. Built off the audio thread and shared by
// live and offline streams; evaluation is exact integer arithmetic, so the
// result never depends on how rendering is partitioned.
class WarpTimeMap {
public:
  [[nodiscard]] static foundation::Result<WarpTimeMap> create(std::vector<TimeMapSegment> segments);

  [[nodiscard]] std::span<const TimeMapSegment> segments() const noexcept { return segments_; }
  // Exact lifetime identity for prepared map-dependent DSP state. Copies share
  // the token; a separately created map has another token even when its
  // segments happen to be equal. The index retains its token to prevent reuse.
  [[nodiscard]] const std::shared_ptr<const std::byte>& identity() const noexcept {
    return identity_;
  }
  [[nodiscard]] std::uint64_t output_frames() const noexcept { return segments_.back().output_end; }
  // Index of the segment containing output_frame, or segments().size() past the end.
  [[nodiscard]] std::size_t find_segment(std::uint64_t output_frame) const noexcept;
  // Exact floor(source position) in Q48.16 for output_frame inside segment.
  [[nodiscard]] static std::uint64_t source_at(const TimeMapSegment& segment,
                                               std::uint64_t output_frame) noexcept;
  // Smallest output frame in segment whose source position is at least
  // `source`, or segment.output_end when the segment never reaches it.
  [[nodiscard]] static std::uint64_t first_output_reaching(const TimeMapSegment& segment,
                                                           std::uint64_t source) noexcept;
  // Linear continuation of segment beyond its end, used to render splice crossfades.
  [[nodiscard]] static std::uint64_t extrapolate(const TimeMapSegment& segment,
                                                 std::uint64_t output_frame) noexcept;
  // Source frames advanced per output frame; used only to choose filter bandwidth.
  [[nodiscard]] static double speed(const TimeMapSegment& segment) noexcept;

private:
  WarpTimeMap(std::vector<TimeMapSegment> segments, std::shared_ptr<const std::byte> identity)
      : segments_(std::move(segments)), identity_(std::move(identity)) {}
  std::vector<TimeMapSegment> segments_;
  std::shared_ptr<const std::byte> identity_;
};

// Exact incremental evaluation of WarpTimeMap::source_at along one segment:
// a Bresenham carry on the division remainder replaces per-frame division.
class SegmentStepper {
public:
  SegmentStepper(const TimeMapSegment& segment, std::uint64_t output_frame) noexcept;
  [[nodiscard]] std::uint64_t position() const noexcept { return position_; }
  void advance() noexcept {
    position_ += quotient_;
    error_ += remainder_;
    if (error_ >= delta_output_) {
      error_ -= delta_output_;
      ++position_;
    }
  }

private:
  std::uint64_t position_;
  std::uint64_t quotient_;
  std::uint64_t remainder_;
  std::uint64_t error_;
  std::uint64_t delta_output_;
};

// Allocation-free forward cursor over a map. Monotonic advances are amortised
// O(1); any backwards or distant jump falls back to binary search.
class TimeMapCursor {
public:
  explicit TimeMapCursor(const WarpTimeMap& map) noexcept : map_(&map) {}
  // Returns false past the end of the map.
  [[nodiscard]] bool locate(std::uint64_t output_frame) noexcept;
  [[nodiscard]] const TimeMapSegment& segment() const noexcept { return map_->segments()[index_]; }
  [[nodiscard]] std::size_t index() const noexcept { return index_; }

private:
  const WarpTimeMap* map_;
  std::size_t index_ = 0;
};

} // namespace oscillate::warp_dsp

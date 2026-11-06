#pragma once

#include "oscillate/foundation/error.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace oscillate::processing {

// An anchor binds one source frame boundary to one rendered frame boundary.
// The first and last anchors are the clip endpoints. Both coordinates are
// strictly increasing, so a source interval is never reversed or discarded.
struct WarpAnchor {
  std::uint64_t source_frame;
  std::uint64_t output_frame;
  bool operator==(const WarpAnchor&) const = default;
};

struct WarpSegment {
  WarpAnchor begin;
  WarpAnchor end;
  [[nodiscard]] std::uint64_t source_frames() const noexcept {
    return end.source_frame - begin.source_frame;
  }
  [[nodiscard]] std::uint64_t output_frames() const noexcept {
    return end.output_frame - begin.output_frame;
  }
};

// Control/worker-side immutable segment plan. This defines duration and marker
// placement only; an audio processor must preserve pitch independently.
class PreparedWarpMap {
public:
  static constexpr std::size_t max_anchors = 4'096;
  static constexpr std::uint64_t max_segment_frames = 1ULL << 31U;

  [[nodiscard]] static foundation::Result<PreparedWarpMap>
  prepare(std::span<const WarpAnchor> anchors, std::uint64_t source_frames,
          std::uint64_t output_frames);

  [[nodiscard]] std::span<const WarpSegment> segments() const noexcept { return segments_; }
  [[nodiscard]] std::uint64_t source_frames() const noexcept { return source_frames_; }
  [[nodiscard]] std::uint64_t output_frames() const noexcept { return output_frames_; }

  // Boundary coordinates include the final endpoint. Nullopt means outside
  // the clip. Integer interpolation rounds to the nearest frame and is exact
  // at every anchor; no floating-point drift accumulates across segments.
  [[nodiscard]] std::optional<std::uint64_t>
  source_at_output(std::uint64_t output_frame) const noexcept;
  [[nodiscard]] std::optional<std::uint64_t>
  output_at_source(std::uint64_t source_frame) const noexcept;

private:
  PreparedWarpMap(std::vector<WarpSegment> segments, WarpAnchor endpoint) noexcept
      : segments_(std::move(segments)), source_frames_(endpoint.source_frame),
        output_frames_(endpoint.output_frame) {}

  std::vector<WarpSegment> segments_;
  std::uint64_t source_frames_;
  std::uint64_t output_frames_;
};

} // namespace oscillate::processing

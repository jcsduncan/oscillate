#include "oscillate/processing/warp_map.hpp"
#include "oscillate/test/check.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using oscillate::processing::PreparedWarpMap;
using oscillate::processing::WarpAnchor;

using oscillate::test::require;

void exact_marker_boundaries_and_continuous_segments() {
  constexpr std::array anchors{WarpAnchor{0, 0}, WarpAnchor{48'000, 24'000},
                               WarpAnchor{72'000, 72'000}, WarpAnchor{96'000, 96'000}};
  const auto prepared = PreparedWarpMap::prepare(anchors, 96'000, 96'000);
  REQUIRE_TRUE(prepared, "warp segment plan missing");
  REQUIRE_EQ(prepared.value().segments().size(), 3, "warp segment plan missing");
  const auto& map = prepared.value();
  REQUIRE_EQ(map.source_at_output(0), 0, "warp markers moved from exact frame boundaries");
  REQUIRE_EQ(map.source_at_output(24'000), 48'000,
             "warp markers moved from exact frame boundaries");
  REQUIRE_EQ(map.source_at_output(72'000), 72'000,
             "warp markers moved from exact frame boundaries");
  REQUIRE_EQ(map.source_at_output(96'000), 96'000,
             "warp markers moved from exact frame boundaries");
  REQUIRE_EQ(map.output_at_source(48'000), 24'000,
             "warp markers moved from exact frame boundaries");
  REQUIRE_EQ(map.output_at_source(72'000), 72'000,
             "warp markers moved from exact frame boundaries");
  REQUIRE_EQ(map.source_at_output(12'000), 24'000,
             "warp segment interpolation changed its duration");
  REQUIRE_EQ(map.source_at_output(48'000), 60'000,
             "warp segment interpolation changed its duration");
  REQUIRE_EQ(map.output_at_source(60'000), 48'000,
             "warp segment interpolation changed its duration");
  REQUIRE_TRUE(!map.source_at_output(96'001), "out-of-clip coordinate mapped into source media");
  REQUIRE_TRUE(!map.output_at_source(96'001), "out-of-clip coordinate mapped into source media");
  for (std::uint64_t frame = 1; frame <= map.output_frames(); ++frame) {
    REQUIRE_GE(map.source_at_output(frame), map.source_at_output(frame - 1),
               "warp source coordinate ran backward");
  }
}

void invalid_anchor_sets_are_rejected() {
  constexpr std::array missing_start{WarpAnchor{1, 0}, WarpAnchor{100, 200}};
  constexpr std::array repeated_source{WarpAnchor{0, 0}, WarpAnchor{50, 50}, WarpAnchor{50, 100},
                                       WarpAnchor{100, 200}};
  constexpr std::array repeated_output{WarpAnchor{0, 0}, WarpAnchor{50, 50}, WarpAnchor{70, 50},
                                       WarpAnchor{100, 200}};
  constexpr std::array too_long{WarpAnchor{0, 0},
                                WarpAnchor{PreparedWarpMap::max_segment_frames + 1, 1}};
  REQUIRE_TRUE(!PreparedWarpMap::prepare(missing_start, 100, 200),
               "invalid warp anchors were accepted");
  REQUIRE_TRUE(!PreparedWarpMap::prepare(repeated_source, 100, 200),
               "invalid warp anchors were accepted");
  REQUIRE_TRUE(!PreparedWarpMap::prepare(repeated_output, 100, 200),
               "invalid warp anchors were accepted");
  REQUIRE_TRUE(!PreparedWarpMap::prepare(too_long, too_long.back().source_frame, 1),
               "invalid warp anchors were accepted");
  REQUIRE_TRUE(!PreparedWarpMap::prepare(std::array{WarpAnchor{0, 0}, WarpAnchor{0, 0}}, 0, 0),
               "invalid warp anchors were accepted");
}

void large_coordinates_do_not_overflow() {
  constexpr auto span = PreparedWarpMap::max_segment_frames;
  constexpr std::array anchors{WarpAnchor{0, 0}, WarpAnchor{span, span - 1},
                               WarpAnchor{2 * span, 2 * span - 1}};
  const auto prepared = PreparedWarpMap::prepare(anchors, 2 * span, 2 * span - 1);
  REQUIRE_OK(prepared, "bounded large warp plan rejected");
  const auto& map = prepared.value();
  REQUIRE_EQ(map.source_at_output(span - 1), span,
             "large warp coordinate overflowed or missed an anchor");
  REQUIRE_EQ(map.source_at_output(2 * span - 1), 2 * span,
             "large warp coordinate overflowed or missed an anchor");
  REQUIRE_EQ(map.output_at_source(2 * span), 2 * span - 1,
             "large warp coordinate overflowed or missed an anchor");
}
} // namespace

int main() {
  try {
    exact_marker_boundaries_and_continuous_segments();
    invalid_anchor_sets_are_rejected();
    large_coordinates_do_not_overflow();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

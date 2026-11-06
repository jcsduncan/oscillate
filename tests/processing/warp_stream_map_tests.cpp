#include "oscillate/processing/warp_stream_map.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/warp_dsp/rate_view.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
namespace processing = oscillate::processing;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace warp = oscillate::warp_dsp;

using oscillate::test::require;

processing::ClipFramePlan plan(std::uint64_t source_start) {
  constexpr std::array anchors{processing::WarpAnchor{0, 0}, processing::WarpAnchor{12, 10},
                               processing::WarpAnchor{30, 25}};
  auto prepared = processing::PreparedWarpMap::prepare(anchors, 30, 25);
  REQUIRE_OK(prepared, "fixture warp plan rejected");
  return {500, source_start, std::move(prepared).value()};
}

void exact_markers_and_trimmed_source_are_preserved() {
  auto clip = plan(1'000);
  auto converted = processing::make_warp_stream_map(clip, 48'000, 48'000);
  REQUIRE_OK(converted, "valid stream map rejected");
  const auto& map = converted.value();
  REQUIRE_EQ(map.output_frames(), 25, "clip duration or marker count changed");
  REQUIRE_EQ(map.segments().size(), 2, "clip duration or marker count changed");
  REQUIRE_EQ(warp::WarpTimeMap::source_at(map.segments()[0], 0), 1'000 * warp::source_unit,
             "trimmed source or marker moved");
  REQUIRE_EQ(warp::WarpTimeMap::source_at(map.segments()[1], 10), 1'012 * warp::source_unit,
             "trimmed source or marker moved");
  REQUIRE_EQ(map.segments().back().source_end, 1'030 * warp::source_unit,
             "trimmed source or marker moved");
  REQUIRE_EQ(warp::WarpTimeMap::source_at(map.segments()[0], 5),
             (1'000 * warp::source_unit + 6 * warp::source_unit),
             "intermediate map position changed");
}

constexpr std::array<std::uint32_t, 4> rates{44'100, 48'000, 88'200, 96'000};

// Oracle: round(frame * P * 65536 / S) half up, computed in long double,
// which is exact for these magnitudes.
std::uint64_t expected_position(std::uint64_t frame, std::uint32_t source_rate,
                                std::uint32_t project_rate) {
  const long double exact = static_cast<long double>(frame) * project_rate * warp::source_unit /
                            static_cast<long double>(source_rate);
  return static_cast<std::uint64_t>(std::floor(exact + 0.5L));
}

void every_rate_pair_maps_source_to_the_project_rate_view() {
  for (const auto source_rate : rates) {
    for (const auto project_rate : rates) {
      for (const std::uint64_t frame :
           {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{147}, std::uint64_t{160},
            std::uint64_t{source_rate} - 1, std::uint64_t{source_rate},
            std::uint64_t{source_rate} * 3'600 + 12'345}) {
        const auto position =
            processing::project_rate_source_position(frame, source_rate, project_rate);
        REQUIRE_TRUE(position, "project-rate source position is not the rounded exact ratio");
        REQUIRE_EQ(position.value(), expected_position(frame, source_rate, project_rate),
                   "project-rate source position is not the rounded exact ratio");
      }
      // Whole seconds land on whole project frames at every pair.
      REQUIRE_EQ(
          processing::project_rate_source_position(source_rate * 7ULL, source_rate, project_rate)
              .value(),
          project_rate * 7ULL * warp::source_unit,
          "whole seconds did not map to whole project frames");
      // Same plan, any rate: markers, trim and duration hold in time.
      auto clip = plan(source_rate);
      auto converted = processing::make_warp_stream_map(clip, source_rate, project_rate);
      REQUIRE_OK(converted, "supported rate pair rejected");
      const auto& map = converted.value();
      REQUIRE_EQ(map.output_frames(), 25, "conversion changed output duration or markers");
      REQUIRE_EQ(map.segments().size(), 2, "conversion changed output duration or markers");
      REQUIRE_EQ(map.segments()[0].source_begin, project_rate * warp::source_unit,
                 "trim start or marker moved in the project-rate view");
      REQUIRE_EQ(map.segments()[1].source_begin,
                 expected_position(source_rate + 12, source_rate, project_rate),
                 "trim start or marker moved in the project-rate view");
      REQUIRE_EQ(map.segments().back().source_end,
                 expected_position(source_rate + 30, source_rate, project_rate),
                 "trim start or marker moved in the project-rate view");
    }
  }
}

using oscillate::test::take;

template <typename T> T id(std::uint64_t value) { return take(T::from_bits({0, value})); }

project::SourceRecord source_at(std::uint32_t rate) {
  return {id<project::SourceId>(1),
          project::SourceEncoding::wav,
          std::string(64, 'a'),
          rate * 64ULL,
          rate,
          2,
          rate * 4ULL};
}

// An unwarped, trimmed clip whose elapsed placement uses the import rule for
// its converted duration reads its source at exactly unity speed in the
// project-rate view, so it keeps pitch and elapsed duration. A musical
// marker resolved by the shared plan keeps its source frame at any rate.
void unchanged_clip_at_another_rate_keeps_speed_and_duration() {
  const auto tempo =
      take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  for (const auto source_rate : rates) {
    for (const auto project_rate : rates) {
      const auto source = source_at(source_rate);
      const std::int64_t first = source_rate / 10;
      const std::int64_t last = first + source_rate * 2LL + 7;
      const auto start_frames = take(
          warp::project_rate_frames(static_cast<std::uint64_t>(first), source_rate, project_rate));
      const auto end_frames = take(
          warp::project_rate_frames(static_cast<std::uint64_t>(last), source_rate, project_rate));
      const auto placed = static_cast<std::int64_t>(end_frames - start_frames);
      const project::Clip clip{id<project::ClipId>(2),
                               source.id,
                               {{first}, {last}},
                               project::ElapsedInterval{{1'000}, {1'000 + placed}}};
      const auto planned =
          take(processing::prepare_clip_frame_plan(clip, source, tempo, project_rate));
      const auto map = take(processing::make_warp_stream_map(planned, source_rate, project_rate));
      const auto& segment = map.segments().front();
      const auto span = segment.source_end - segment.source_begin;
      const auto output = map.output_frames() * warp::source_unit;
      // Unity speed to within the half-frame rounding of each endpoint.
      require(map.segments().size() == 1 && map.output_frames() == end_frames - start_frames &&
                  (span > output ? span - output : output - span) <= warp::source_unit,
              "unwarped clip at another rate is not unity speed in project frames");

      const project::Clip musical{id<project::ClipId>(3),
                                  source.id,
                                  {{first}, {first + source_rate * 2LL}},
                                  project::MusicalInterval{{0}, {4 * timeline::ticks_per_quarter}}};
      const std::array markers{processing::ClipWarpMarker{
          {first + source_rate / 2}, timeline::TickPosition{timeline::ticks_per_quarter}}};
      const auto warped = take(processing::make_warp_stream_map(
          take(processing::prepare_clip_frame_plan(musical, source, tempo, project_rate, markers)),
          source_rate, project_rate));
      REQUIRE_EQ(warped.segments().size(), 2,
                 "musical marker moved in time or source under rate conversion");
      REQUIRE_EQ(warped.segments()[1].output_begin, project_rate / 2,
                 "musical marker moved in time or source under rate conversion");
      REQUIRE_EQ(warped.segments()[1].source_begin,
                 expected_position(static_cast<std::uint64_t>(first + source_rate / 2), source_rate,
                                   project_rate),
                 "musical marker moved in time or source under rate conversion");
    }
  }
}

void unsupported_rate_and_range_fail_closed() {
  auto clip = plan(1'000);
  REQUIRE_TRUE(!processing::make_warp_stream_map(clip, 7'999, 48'000),
               "unsupported rate was accepted");
  REQUIRE_TRUE(!processing::make_warp_stream_map(clip, 48'000, 32'000),
               "unsupported rate was accepted");
  REQUIRE_TRUE(!processing::project_rate_source_position(1, 48'000, 22'050),
               "unsupported rate was accepted");
  clip.source_start_frame = warp::max_source_position / warp::source_unit;
  require(!processing::make_warp_stream_map(clip, 48'000, 48'000),
          "source range beyond fixed-point limit was accepted");
  // Upsampling stretches source positions; the fixed-point bound still holds.
  clip.source_start_frame = warp::max_source_position / warp::source_unit / 2;
  require(!processing::make_warp_stream_map(clip, 44'100, 96'000),
          "converted source range beyond fixed-point limit was accepted");
}

} // namespace

int main() {
  try {
    exact_markers_and_trimmed_source_are_preserved();
    every_rate_pair_maps_source_to_the_project_rate_view();
    unchanged_clip_at_another_rate_keeps_speed_and_duration();
    unsupported_rate_and_range_fail_closed();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}

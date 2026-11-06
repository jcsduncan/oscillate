#include "oscillate/processing/clip_frame_plan.hpp"
#include "oscillate/test/check.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

using namespace oscillate;

using oscillate::test::require;

using oscillate::test::take;

template <typename T> T id(std::uint64_t value) { return take(T::from_bits({0, value})); }

timeline::TempoMap tempo(double bpm) {
  return take(timeline::TempoMap::create({{{0}, bpm, timeline::TempoShape::step}}, 1));
}

project::SourceRecord source() {
  return {id<project::SourceId>(1),
          project::SourceEncoding::wav,
          std::string(64, 'a'),
          400'000,
          48'000,
          2,
          100'000};
}

void musical_markers_follow_tempo_without_moving_source_frames() {
  constexpr auto quarter = timeline::ticks_per_quarter;
  const project::Clip clip{id<project::ClipId>(2),
                           id<project::SourceId>(1),
                           {{1'000}, {49'000}},
                           project::MusicalInterval{{0}, {2 * quarter}}};
  const std::array markers{processing::ClipWarpMarker{{25'000}, timeline::TickPosition{quarter}}};
  const auto at_120 =
      take(processing::prepare_clip_frame_plan(clip, source(), tempo(120), 48'000, markers));
  const auto at_60 =
      take(processing::prepare_clip_frame_plan(clip, source(), tempo(60), 48'000, markers));
  REQUIRE_EQ(at_120.source_start_frame, 1'000,
             "120 BPM clip frame plan changed the reviewed marker");
  REQUIRE_EQ(at_120.project_start_frame, 0, "120 BPM clip frame plan changed the reviewed marker");
  REQUIRE_EQ(at_120.warp.source_frames(), 48'000,
             "120 BPM clip frame plan changed the reviewed marker");
  REQUIRE_EQ(at_120.warp.output_frames(), 48'000,
             "120 BPM clip frame plan changed the reviewed marker");
  REQUIRE_EQ(at_120.warp.output_at_source(24'000), 24'000,
             "120 BPM clip frame plan changed the reviewed marker");
  REQUIRE_EQ(at_60.source_start_frame, 1'000, "tempo edit did not re-resolve the musical marker");
  REQUIRE_EQ(at_60.warp.source_frames(), 48'000,
             "tempo edit did not re-resolve the musical marker");
  REQUIRE_EQ(at_60.warp.output_frames(), 96'000,
             "tempo edit did not re-resolve the musical marker");
  REQUIRE_EQ(at_60.warp.output_at_source(24'000), 48'000,
             "tempo edit did not re-resolve the musical marker");
  const auto at_96k =
      take(processing::prepare_clip_frame_plan(clip, source(), tempo(120), 96'000, markers));
  REQUIRE_EQ(at_96k.warp.source_frames(), 48'000,
             "project-rate edit moved source frames or marker time");
  REQUIRE_EQ(at_96k.warp.output_frames(), 96'000,
             "project-rate edit moved source frames or marker time");
  REQUIRE_EQ(at_96k.warp.output_at_source(24'000), 48'000,
             "project-rate edit moved source frames or marker time");
  const project::Clip moved{id<project::ClipId>(2),
                            id<project::SourceId>(1),
                            {{1'000}, {49'000}},
                            project::MusicalInterval{{4 * quarter}, {6 * quarter}}};
  const auto moved_plan =
      take(processing::prepare_clip_frame_plan(moved, source(), tempo(120), 48'000, markers));
  REQUIRE_EQ(moved_plan.project_start_frame, 96'000,
             "moving a musical clip did not move its marker with it");
  REQUIRE_EQ(moved_plan.warp.output_at_source(24'000), 24'000,
             "moving a musical clip did not move its marker with it");
}

void elapsed_markers_keep_sample_boundaries() {
  const project::Clip clip{id<project::ClipId>(2),
                           id<project::SourceId>(1),
                           {{10'000}, {58'000}},
                           project::ElapsedInterval{{100'000}, {196'000}}};
  const std::array markers{processing::ClipWarpMarker{{34'000}, timeline::SamplePosition{48'000}}};
  const auto planned =
      take(processing::prepare_clip_frame_plan(clip, source(), tempo(120), 48'000, markers));
  REQUIRE_EQ(planned.project_start_frame, 100'000, "elapsed clip marker or endpoint was shifted");
  REQUIRE_EQ(planned.project_end_frame(), 196'000, "elapsed clip marker or endpoint was shifted");
  REQUIRE_EQ(planned.warp.output_at_source(24'000), 48'000,
             "elapsed clip marker or endpoint was shifted");
  const auto after_tempo_edit =
      take(processing::prepare_clip_frame_plan(clip, source(), tempo(60), 48'000, markers));
  REQUIRE_EQ(after_tempo_edit.project_start_frame, planned.project_start_frame,
             "elapsed marker followed the musical tempo map");
  REQUIRE_EQ(after_tempo_edit.warp.output_at_source(24'000), 48'000,
             "elapsed marker followed the musical tempo map");
  const project::Clip moved{id<project::ClipId>(2),
                            id<project::SourceId>(1),
                            {{10'000}, {58'000}},
                            project::ElapsedInterval{{200'000}, {296'000}}};
  const auto moved_plan =
      take(processing::prepare_clip_frame_plan(moved, source(), tempo(120), 48'000, markers));
  REQUIRE_EQ(moved_plan.project_start_frame, 200'000,
             "moving an elapsed clip did not move its marker with it");
  REQUIRE_EQ(moved_plan.warp.output_at_source(24'000), 48'000,
             "moving an elapsed clip did not move its marker with it");
}

void saved_reviewed_markers_are_the_default_plan() {
  project::Clip clip{id<project::ClipId>(2),
                     id<project::SourceId>(1),
                     {{10'000}, {58'000}},
                     project::ElapsedInterval{{100'000}, {148'000}}};
  clip.processing.reviewed_warp_markers.push_back({{34'000}, timeline::SamplePosition{32'000}});
  const auto planned =
      take(processing::prepare_clip_frame_plan(clip, source(), tempo(120), 48'000));
  REQUIRE_EQ(planned.warp.source_frames(), planned.warp.output_frames(),
             "saved same-duration warp marker was ignored by the default frame plan");
  REQUIRE_EQ(planned.warp.output_at_source(24'000), 32'000,
             "saved same-duration warp marker was ignored by the default frame plan");
}

void invalid_markers_fail_before_processing() {
  constexpr auto quarter = timeline::ticks_per_quarter;
  const project::Clip clip{id<project::ClipId>(2),
                           id<project::SourceId>(1),
                           {{1'000}, {49'000}},
                           project::MusicalInterval{{0}, {2 * quarter}}};
  const std::array wrong_timebase{
      processing::ClipWarpMarker{{25'000}, timeline::SamplePosition{24'000}}};
  const std::array outside_source{
      processing::ClipWarpMarker{{49'000}, timeline::TickPosition{quarter}}};
  const std::array reversed{
      processing::ClipWarpMarker{{30'000}, timeline::TickPosition{quarter / 2}},
      processing::ClipWarpMarker{{20'000}, timeline::TickPosition{quarter}}};
  const std::array rounded_together{
      processing::ClipWarpMarker{{20'000}, timeline::TickPosition{quarter}},
      processing::ClipWarpMarker{{21'000}, timeline::TickPosition{quarter + 1}}};
  REQUIRE_TRUE(
      !processing::prepare_clip_frame_plan(clip, source(), tempo(120), 48'000, wrong_timebase),
      "invalid marker coordinates reached the processor");
  REQUIRE_TRUE(
      !processing::prepare_clip_frame_plan(clip, source(), tempo(120), 48'000, outside_source),
      "invalid marker coordinates reached the processor");
  REQUIRE_TRUE(!processing::prepare_clip_frame_plan(clip, source(), tempo(120), 48'000, reversed),
               "invalid marker coordinates reached the processor");
  REQUIRE_TRUE(
      !processing::prepare_clip_frame_plan(clip, source(), tempo(120), 48'000, rounded_together),
      "invalid marker coordinates reached the processor");
}

} // namespace

int main() {
  try {
    musical_markers_follow_tempo_without_moving_source_frames();
    elapsed_markers_keep_sample_boundaries();
    saved_reviewed_markers_are_the_default_plan();
    invalid_markers_fail_before_processing();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

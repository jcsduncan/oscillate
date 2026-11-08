#include "oscillate/project/frozen_view.hpp"
#include "oscillate/test/check.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
namespace foundation = oscillate::foundation;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;

using oscillate::test::take;

template <typename Id> Id id(std::uint64_t value) { return take(Id::from_bits({0, value})); }

// 120 BPM at 48 kHz: a quarter note is 24'000 frames.
constexpr std::uint32_t rate = 48'000;
constexpr double bpm = 120.0;
constexpr std::int64_t quarter_frames = 24'000;

timeline::TempoMap tempo() {
  return take(timeline::TempoMap::create({{{0}, bpm, timeline::TempoShape::step}}, 1));
}

project::AutomationLane channel_lane(std::uint64_t lane, project::TrackId track,
                                     project::TrackControlTarget::Key key) {
  const auto domain = project::channel_control_domain(key);
  return {id<project::AutomationLaneId>(lane),
          project::TrackControlTarget{track, key},
          {project::AutomationRepresentation::physical, domain.semantic_key, domain.unit,
           domain.minimum, domain.maximum, domain.minimum, domain.discrete, domain.step_count,
           domain.scale},
          project::AutomationTimebase::elapsed,
          {{id<project::AutomationPointId>(lane + 1), 0, domain.minimum}},
          1};
}

project::Clip audio_clip(std::uint64_t clip, std::int64_t begin, std::int64_t end) {
  return {id<project::ClipId>(clip),
          id<project::SourceId>(1),
          {{0}, {end - begin}},
          project::ElapsedInterval{{begin}, {end}}};
}

// An audio clip with a level envelope at [1'000, 3'000), a plain audio clip
// at [5'000, 6'000), and a MIDI clip with a pan envelope on the second
// quarter, [24'000, 48'000).
project::Track laned_track() {
  constexpr std::uint64_t level_lane = 10;
  constexpr std::uint64_t pan_lane = 20;
  const auto track_id = id<project::TrackId>(2);
  project::Track track{track_id};
  auto level = audio_clip(3, 1'000, 3'000);
  level.automation.push_back(
      channel_lane(level_lane, track_id, project::TrackControlTarget::Key::channel_level));
  track.clips.push_back(std::move(level));
  track.clips.push_back(audio_clip(4, 5'000, 6'000));
  project::MidiClip notes{
      id<project::ClipId>(5),
      project::MusicalInterval{{timeline::ticks_per_quarter}, {2 * timeline::ticks_per_quarter}},
      {timeline::ticks_per_quarter},
      false,
      {}};
  notes.automation.push_back(
      channel_lane(pan_lane, track_id, project::TrackControlTarget::Key::channel_pan));
  track.midi_clips.push_back(std::move(notes));
  return track;
}

void require_range(const project::RenderSlice& slice, std::int64_t start, std::int64_t end,
                   std::string_view what) {
  REQUIRE_EQ(slice.range.start.value, start, what);
  REQUIRE_EQ(slice.range.end.value, end, what);
}

void laned_clips_become_slices_at_their_own_places() {
  constexpr std::uint64_t first = 500;
  constexpr std::uint64_t frames = 50'000;
  const auto track = laned_track();
  const auto map = tempo();
  const auto span = take(project::after_render_clip_span(track, map, rate));
  REQUIRE_TRUE(span.has_value(), "the laned clips have a span");
  REQUIRE_EQ(span->first, 1'000, "the span starts with the level envelope's clip");
  REQUIRE_EQ(span->second, static_cast<std::uint64_t>(2 * quarter_frames),
             "the span ends with the pan envelope's clip");

  const auto slices = take(project::render_slices(track, map, rate, first, frames));
  REQUIRE_EQ(slices.size(), 5, "plain, level, plain, pan, plain");
  require_range(slices[0], 0, 500, "a plain slice before the level clip");
  require_range(slices[1], 500, 2'500, "the level clip's frames of the render");
  require_range(slices[2], 2'500, quarter_frames - 500, "a plain slice between");
  require_range(slices[3], quarter_frames - 500, 2 * quarter_frames - 500,
                "the pan clip's frames of the render");
  require_range(slices[4], 2 * quarter_frames - 500, 50'000, "a plain slice to the end");
  REQUIRE_TRUE(slices[0].automation.empty() && slices[2].automation.empty() &&
                   slices[4].automation.empty(),
               "plain slices carry no lanes, so the track's lanes apply");
  REQUIRE_EQ(slices[1].automation.size(), 1, "the level slice carries its envelope");
  REQUIRE_TRUE(std::holds_alternative<project::ElapsedInterval>(slices[1].placement),
               "the level slice keeps its clip's placement");
  REQUIRE_EQ(slices[3].automation.size(), 1, "the pan slice carries its envelope");
  const auto* musical = std::get_if<project::MusicalInterval>(&slices[3].placement);
  REQUIRE_TRUE(musical != nullptr && musical->start.value == timeline::ticks_per_quarter,
               "the pan slice keeps its clip's musical placement");
}

void a_render_that_cuts_or_overlaps_an_envelope_is_refused() {
  constexpr std::uint64_t inside_the_level_clip = 2'000;
  constexpr std::uint64_t frames = 100;
  constexpr std::uint64_t past_every_clip = 100'000;
  const auto map = tempo();
  auto track = laned_track();
  const auto cut = project::render_slices(track, map, rate, inside_the_level_clip, frames);
  REQUIRE_TRUE(!cut && cut.error().code == foundation::ErrorCode::conflict,
               "a render that cuts an envelope's clip is refused");
  const auto beyond = take(project::render_slices(track, map, rate, past_every_clip, frames));
  REQUIRE_EQ(beyond.size(), 1, "a render past every clip is one plain slice");

  // A mute envelope on a clip overlapping the level clip.
  constexpr std::uint64_t mute_lane = 30;
  auto overlapping = audio_clip(6, 2'000, 4'000);
  overlapping.automation.push_back(
      channel_lane(mute_lane, track.id, project::TrackControlTarget::Key::channel_mute));
  track.clips.push_back(std::move(overlapping));
  const auto span = project::after_render_clip_span(track, map, rate);
  REQUIRE_TRUE(!span && span.error().code == foundation::ErrorCode::conflict,
               "overlapping envelopes are refused before a render runs");
  const auto sliced = project::render_slices(track, map, rate, 0, past_every_clip);
  REQUIRE_TRUE(!sliced && sliced.error().code == foundation::ErrorCode::conflict,
               "overlapping envelopes cannot slice");
}

} // namespace

int main() {
  try {
    laned_clips_become_slices_at_their_own_places();
    a_render_that_cuts_or_overlaps_an_envelope_is_refused();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "render slices passed\n";
  return 0;
}

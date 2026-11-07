#include "oscillate/application/session/pedal_service.hpp"

#include "edit_support.hpp"
#include "midi_clips.hpp"
#include "oscillate/application/midi_note_tools.hpp"
#include "oscillate/application/midi_pedal_edit.hpp"
#include "session_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ranges>
#include <utility>
#include <vector>

namespace oscillate::application {
namespace {

using ChannelKey = project::TrackControlTarget::Key;
using detail::find_midi_clip;
using detail::midi_anchor_tick;

[[nodiscard]] std::vector<project::MidiPedalPoint> lane_points(const project::MidiClip& clip,
                                                               project::MidiPedal pedal) {
  const auto lane = std::ranges::find(clip.pedals, pedal, &project::MidiPedalLane::pedal);
  return lane == clip.pedals.end() ? std::vector<project::MidiPedalPoint>{} : lane->points;
}

} // namespace

foundation::Result<void>
PedalService::set_midi_pedal_lane(project::ClipId clip, project::MidiPedal pedal,
                                  std::vector<project::MidiPedalPoint> points) {
  auto applied = model().apply({project::SetMidiPedalLane{clip, pedal, std::move(points)}});
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

foundation::Result<void> PedalService::paint_pedal_span(project::ClipId clip,
                                                        project::MidiPedal pedal,
                                                        timeline::TickPosition start,
                                                        timeline::TickPosition end, double depth) {
  const auto* found = find_midi_clip(snapshot(), clip);
  if (!found)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::not_found, "MIDI clip not found: " + clip.to_string()});
  if (end <= start || !std::isfinite(depth) || depth < 0.0 || depth > 1.0)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::invalid_argument, "Invalid pedal span"});
  return set_midi_pedal_lane(clip, pedal,
                             paint_pedal(lane_points(*found, pedal), start.value, end.value, depth,
                                         found->pattern_length.value));
}

foundation::Result<void> PedalService::remove_pedal_span(project::ClipId clip,
                                                         project::MidiPedal pedal,
                                                         timeline::TickPosition at) {
  const auto* found = find_midi_clip(snapshot(), clip);
  if (!found)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::not_found, "MIDI clip not found: " + clip.to_string()});
  return set_midi_pedal_lane(
      clip, pedal,
      application::remove_pedal_span(lane_points(*found, pedal), at, found->pattern_length.value));
}

foundation::Result<void> PedalService::move_pedal_span(project::ClipId clip,
                                                       project::MidiPedal pedal,
                                                       timeline::TickPosition at,
                                                       timeline::TickPosition start,
                                                       timeline::TickPosition end) {
  const auto* found = find_midi_clip(snapshot(), clip);
  if (!found)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::not_found, "MIDI clip not found: " + clip.to_string()});
  if (end <= start)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::invalid_argument, "Invalid pedal span"});
  const auto points = lane_points(*found, pedal);
  const auto length = found->pattern_length.value;
  for (const auto& span : pedal_spans(points, length)) {
    if (at.value >= span.start && at.value < span.end) {
      const auto lifted = application::remove_pedal_span(points, at, length);
      return set_midi_pedal_lane(clip, pedal,
                                 paint_pedal(lifted, start.value, end.value, span.depth, length));
    }
  }
  return foundation::Result<void>::failure(
      {foundation::ErrorCode::not_found, "No pedal span at that position"});
}

foundation::Result<void> PedalService::pedal_per_chord(project::ClipId clip) {
  const auto* found = find_midi_clip(snapshot(), clip);
  if (!found)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::not_found, "MIDI clip not found: " + clip.to_string()});
  // Re-press a thirty-second note after each chord lands.
  constexpr std::int64_t repress = timeline::ticks_per_quarter / 8;
  auto points =
      application::pedal_per_chord(found->notes, found->pattern_length.value, {.repress = repress});
  // Points kept after the pattern's end stay with it.
  for (const auto& point : lane_points(*found, project::MidiPedal::sustain)) {
    if (point.position >= found->pattern_length)
      points.push_back(point);
  }
  return set_midi_pedal_lane(clip, project::MidiPedal::sustain, std::move(points));
}

foundation::Result<void> PedalService::clear_midi_pedals(project::ClipId clip) {
  auto applied = model().apply({project::SetMidiPedalLane{clip, project::MidiPedal::sustain, {}},
                                project::SetMidiPedalLane{clip, project::MidiPedal::sostenuto, {}},
                                project::SetMidiPedalLane{clip, project::MidiPedal::soft, {}}});
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

} // namespace oscillate::application

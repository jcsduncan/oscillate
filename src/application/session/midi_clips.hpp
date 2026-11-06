#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/timeline/timing.hpp"

#include <algorithm>
#include <variant>

// MIDI clip lookups the session's translation units share.
namespace oscillate::application::detail {

[[nodiscard]] inline const project::MidiClip* find_midi_clip(const project::Snapshot& state,
                                                             project::ClipId clip) {
  for (const auto& track : state.tracks) {
    const auto found = std::ranges::find(track.midi_clips, clip, &project::MidiClip::id);
    if (found != track.midi_clips.end())
      return &*found;
  }
  return nullptr;
}

// The tick a MIDI clip's pattern counts from: an elapsed clip's start
// sample mapped through the tempo map, as playback maps its notes.
[[nodiscard]] inline foundation::Result<timeline::TickPosition>
midi_anchor_tick(const project::Snapshot& state, const project::MidiClip& clip) {
  if (const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement))
    return foundation::Result<timeline::TickPosition>::success(musical->start);
  return state.tempo.sample_to_tick(std::get<project::ElapsedInterval>(clip.placement).start,
                                    state.sample_rate);
}

} // namespace oscillate::application::detail

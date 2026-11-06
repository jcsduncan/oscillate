#pragma once

#include "oscillate/project/model.hpp"

#include <algorithm>

// Lookups by ID in one snapshot. Each returns nullptr when nothing matches.
namespace oscillate::project {

[[nodiscard]] inline const Track* find_track(const Snapshot& state, TrackId id) noexcept {
  const auto found = std::ranges::find(state.tracks, id, &Track::id);
  return found == state.tracks.end() ? nullptr : &*found;
}

[[nodiscard]] inline Track* find_track(Snapshot& state, TrackId id) noexcept {
  const auto found = std::ranges::find(state.tracks, id, &Track::id);
  return found == state.tracks.end() ? nullptr : &*found;
}

// The audio clip with `id` on any track.
[[nodiscard]] inline const Clip* find_clip(const Snapshot& state, ClipId id) noexcept {
  for (const auto& track : state.tracks) {
    const auto found = std::ranges::find(track.clips, id, &Clip::id);
    if (found != track.clips.end())
      return &*found;
  }
  return nullptr;
}

} // namespace oscillate::project

#include "vst3_editor_commit.hpp"

#include <algorithm>
#include <variant>

namespace oscillate::quick {

bool can_commit_vst3_editor_state(const project::Snapshot& snapshot, project::TrackId track,
                                  project::InsertSlotId slot,
                                  const project::Vst3PluginInstance& original) {
  const auto owner = std::ranges::find(snapshot.tracks, track, &project::Track::id);
  if (owner == snapshot.tracks.end())
    return false;
  const auto found = std::ranges::find(owner->effects, slot, &project::EffectInsert::id);
  if (found == owner->effects.end())
    return false;
  const auto* plugin = std::get_if<project::Vst3PluginInstance>(&found->processor);
  return plugin && *plugin == original;
}

bool can_commit_vst3_instrument_state(const project::Snapshot& snapshot, project::TrackId track,
                                      const project::Vst3PluginInstance& original) {
  const auto owner = std::ranges::find(snapshot.tracks, track, &project::Track::id);
  if (owner == snapshot.tracks.end() || owner->kind != project::TrackKind::instrument)
    return false;
  const auto* plugin = std::get_if<project::Vst3PluginInstance>(&owner->instrument_source);
  return plugin && *plugin == original;
}

} // namespace oscillate::quick

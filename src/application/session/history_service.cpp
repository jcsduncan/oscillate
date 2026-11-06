#include "oscillate/application/session/history_service.hpp"

#include "edit_support.hpp"
#include "midi_clips.hpp"
#include "session_state.hpp"

#include <vector>

namespace oscillate::application {
namespace {

using ChannelKey = project::TrackControlTarget::Key;
using detail::find_midi_clip;
using detail::midi_anchor_tick;

} // namespace

foundation::Result<void> HistoryService::undo() {
  auto undone = model().undo();
  if (!undone) {
    return foundation::Result<void>::failure(undone.error());
  }
  return foundation::Result<void>::success();
}

foundation::Result<void> HistoryService::redo() {
  auto redone = model().redo();
  if (!redone) {
    return foundation::Result<void>::failure(redone.error());
  }
  return foundation::Result<void>::success();
}

std::vector<project::ProjectModel::HistoryStep> HistoryService::undo_steps() const {
  return state_.model ? model().undo_steps() : std::vector<project::ProjectModel::HistoryStep>{};
}

std::vector<project::ProjectModel::HistoryStep> HistoryService::redo_steps() const {
  return state_.model ? model().redo_steps() : std::vector<project::ProjectModel::HistoryStep>{};
}

} // namespace oscillate::application

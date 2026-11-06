#include "oscillate/application/session/session_service.hpp"

#include "edit_support.hpp"
#include "session_state.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::application {

project::ProjectModel& SessionService::model() const noexcept { return *state_.model; }

const project::Snapshot& SessionService::snapshot() const noexcept {
  return *state_.model->snapshot();
}

std::uint64_t SessionService::next_track_number() const {
  // Every committed create advances the durable revision. Using it as an ID
  // floor prevents an undo/removal from recycling a live processor identity,
  // including after save/reopen, without persisting a second counter.
  std::uint64_t greatest = snapshot().revision;
  for (const auto& track : snapshot().tracks) {
    if (track.id.bits().high == 0) {
      greatest = std::max(greatest, track.id.bits().low);
    }
  }
  return next_number(greatest);
}

std::uint64_t SessionService::next_clip_number() const {
  std::uint64_t greatest = snapshot().revision;
  for (const auto& track : snapshot().tracks) {
    for (const auto& clip : track.clips) {
      if (clip.id.bits().high == 0) {
        greatest = std::max(greatest, clip.id.bits().low);
      }
    }
    for (const auto& clip : track.midi_clips) {
      if (clip.id.bits().high == 0) {
        greatest = std::max(greatest, clip.id.bits().low);
      }
    }
  }
  return next_number(greatest);
}

std::uint64_t SessionService::next_note_number() const {
  std::uint64_t greatest = snapshot().revision;
  for (const auto& track : snapshot().tracks) {
    for (const auto& clip : track.midi_clips) {
      for (const auto& note : clip.notes) {
        if (note.id.bits().high == 0) {
          greatest = std::max(greatest, note.id.bits().low);
        }
      }
    }
  }
  return next_number(greatest);
}

std::uint64_t SessionService::next_source_number() const {
  std::uint64_t greatest = snapshot().revision;
  for (const auto& source : snapshot().sources) {
    if (source.id.bits().high == 0) {
      greatest = std::max(greatest, source.id.bits().low);
    }
  }
  return next_number(greatest);
}

std::uint64_t SessionService::next_send_number() const {
  std::uint64_t greatest = snapshot().revision;
  for (const auto& track : snapshot().tracks) {
    for (const auto& send : track.sends) {
      if (send.id.bits().high == 0) {
        greatest = std::max(greatest, send.id.bits().low);
      }
    }
  }
  return next_number(greatest);
}

std::uint64_t SessionService::next_plugin_number() const {
  std::uint64_t greatest = snapshot().revision;
  const auto inspect = [&greatest](const project::Vst3PluginInstance& plugin) {
    if (plugin.id.bits().high == 0)
      greatest = std::max(greatest, plugin.id.bits().low);
  };
  for (const auto& track : snapshot().tracks) {
    if (const auto* source = std::get_if<project::Vst3PluginInstance>(&track.instrument_source))
      inspect(*source);
    for (const auto& effect : track.effects)
      if (const auto* plugin = std::get_if<project::Vst3PluginInstance>(&effect.processor))
        inspect(*plugin);
  }
  for (const auto& effect : snapshot().master_effects)
    if (const auto* plugin = std::get_if<project::Vst3PluginInstance>(&effect.processor))
      inspect(*plugin);
  return next_number(greatest);
}

std::pair<std::uint64_t, std::uint64_t> SessionService::next_automation_numbers() const {
  std::uint64_t greatest_lane = snapshot().revision;
  std::uint64_t greatest_point = snapshot().revision;
  const auto scan = [&](const std::vector<project::AutomationLane>& lanes) {
    for (const auto& lane : lanes) {
      if (lane.id.bits().high == 0) {
        greatest_lane = std::max(greatest_lane, lane.id.bits().low);
      }
      for (const auto& point : lane.points) {
        if (point.id.bits().high == 0) {
          greatest_point = std::max(greatest_point, point.id.bits().low);
        }
      }
    }
  };
  for (const auto& track : snapshot().tracks) {
    scan(track.automation);
    for (const auto& clip : track.clips) {
      scan(clip.automation);
    }
    for (const auto& clip : track.midi_clips) {
      scan(clip.automation);
    }
  }
  return {next_number(greatest_lane), next_number(greatest_point)};
}

} // namespace oscillate::application

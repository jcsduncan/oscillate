#include "model_internal.hpp"

#include "oscillate/foundation/midi.hpp"
#include "oscillate/foundation/trace.hpp"
#include "oscillate/project/clay_limits.hpp"
#include "oscillate/project/insert_projection.hpp"
#include "oscillate/project/lookup.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace oscillate::project::detail {

namespace {

template <typename T> Result<void> apply_send_edit(ApplyContext& context, const T& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  for (auto& track : candidate.tracks) {
    auto found = std::ranges::find_if(track.sends,
                                      [&](const SendRoute& send) { return send.id == command.id; });
    if (found == track.sends.end()) {
      continue;
    }
    add_unique(changes.tracks, track.id);
    add_unique(changes.tracks, found->destination);
    if constexpr (std::is_same_v<T, RemoveSendRoute>) {
      track.sends.erase(found);
    } else {
      found->gain = command.gain;
    }
    changes.routing = true;
    return Result<void>::success();
  }
  return Result<void>::failure({ErrorCode::not_found, "send route not found"});
}

} // namespace

Result<void> apply_command(ApplyContext& context, const SetTrackMainOutput& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto* track = find_track(candidate, command.track);
  if (track == nullptr) {
    return Result<void>::failure({ErrorCode::not_found, "routing source track not found"});
  }
  if (track->main_output != command.destination) {
    if (track->main_output) {
      add_unique(changes.tracks, *track->main_output);
    }
    if (command.destination) {
      add_unique(changes.tracks, *command.destination);
    }
    track->main_output = command.destination;
    add_unique(changes.tracks, command.track);
    changes.routing = true;
  } else {
    return Result<void>::failure(invalid("track main output is unchanged"));
  }
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const AddSendRoute& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto* track = find_track(candidate, command.track);
  if (track == nullptr) {
    return Result<void>::failure({ErrorCode::not_found, "send source track not found"});
  }
  for (const auto& existing : candidate.tracks) {
    if (std::ranges::any_of(existing.sends,
                            [&](const SendRoute& send) { return send.id == command.send.id; })) {
      return Result<void>::failure(conflict("send route ID already exists"));
    }
  }
  track->sends.push_back(command.send);
  add_unique(changes.tracks, command.track);
  add_unique(changes.tracks, command.send.destination);
  changes.routing = true;
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const RemoveSendRoute& command) {
  return apply_send_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const SetSendGain& command) {
  return apply_send_edit(context, command);
}

} // namespace oscillate::project::detail

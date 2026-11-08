#include "oscillate/application/session/routing_service.hpp"

#include "edit_support.hpp"
#include "midi_clips.hpp"
#include "oscillate/application/browser_preview.hpp"
#include "oscillate/application/key_cache.hpp"
#include "oscillate/application/midi_note_tools.hpp"
#include "oscillate/application/tempo_cache.hpp"
#include "oscillate/midi_file/smf.hpp"
#include "session_state.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oscillate::application {
namespace {

using ChannelKey = project::TrackControlTarget::Key;
using detail::find_midi_clip;
using detail::midi_anchor_tick;

// Words in a track's name that make it a drum, for naming a new bus.
constexpr std::array<std::string_view, 17> drum_words{
    "kick",  "snare", "hat",    "clap", "perc",  "top",   "tom",   "cymbal", "ride",
    "crash", "rim",   "shaker", "drum", "break", "conga", "bongo", "tamb"};

bool reads_as_drum(const project::Track& track) {
  if (looks_like_kick(track, {}))
    return true;
  std::string lower;
  lower.reserve(track.name.size());
  for (const char c : track.name)
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return std::ranges::any_of(
      drum_words, [&](std::string_view word) { return lower.find(word) != std::string::npos; });
}

bool is_bus(const project::Track& track) noexcept {
  return track.kind == project::TrackKind::group_bus ||
         track.kind == project::TrackKind::return_bus;
}

} // namespace

foundation::Result<void>
RoutingService::set_main_output(project::TrackId track,
                                std::optional<project::TrackId> destination) {
  auto applied = model().apply({project::SetTrackMainOutput{track, destination}});
  if (!applied) {
    return foundation::Result<void>::failure(applied.error());
  }
  return foundation::Result<void>::success();
}

foundation::Result<project::TrackId>
RoutingService::route_tracks_to_bus(std::span<const project::TrackId> tracks,
                                    std::optional<project::TrackId> bus) {
  using Result = foundation::Result<project::TrackId>;
  const auto& current = snapshot();
  std::vector<const project::Track*> chosen;
  std::optional<std::size_t> first;
  for (const auto id : tracks) {
    const auto found = std::ranges::find(current.tracks, id, &project::Track::id);
    if (found == current.tracks.end())
      return Result::failure(
          {foundation::ErrorCode::not_found, "A selected track no longer exists"});
    if (is_bus(*found) || (bus && found->id == *bus) ||
        std::ranges::find(chosen, &*found) != chosen.end())
      continue;
    chosen.push_back(&*found);
    const auto index = static_cast<std::size_t>(found - current.tracks.begin());
    first = first ? std::min(*first, index) : index;
  }
  if (chosen.empty())
    return Result::failure({foundation::ErrorCode::invalid_argument,
                            "Select audio or instrument tracks to send to a bus"});
  std::vector<project::Operation> operations;
  if (bus) {
    const auto found = std::ranges::find(current.tracks, *bus, &project::Track::id);
    if (found == current.tracks.end() || found->kind != project::TrackKind::group_bus)
      return Result::failure({foundation::ErrorCode::invalid_argument, "Choose a group bus"});
  }
  auto id = bus ? foundation::Result<project::TrackId>::success(*bus)
                : numbered_id<project::TrackId>(next_track_number());
  if (!id)
    return Result::failure(id.error());
  const auto destination = id.value();
  if (!bus) {
    const bool drums =
        std::ranges::all_of(chosen, [](const project::Track* t) { return reads_as_drum(*t); });
    const auto groups =
        std::ranges::count(current.tracks, project::TrackKind::group_bus, &project::Track::kind);
    operations.emplace_back(project::AddTrack{destination, project::TrackKind::group_bus, first});
    operations.emplace_back(project::SetTrackName{
        destination, drums ? std::string{"Drums"} : "Bus " + std::to_string(groups + 1)});
  }
  for (const auto* track : chosen)
    if (track->main_output != destination)
      operations.emplace_back(project::SetTrackMainOutput{track->id, destination});
  if (operations.empty())
    return Result::success(destination);
  auto applied = model().apply(operations);
  return applied ? Result::success(destination) : Result::failure(applied.error());
}

foundation::Result<void>
RoutingService::route_tracks_to_master(std::span<const project::TrackId> tracks) {
  std::vector<project::Operation> operations;
  for (const auto id : tracks) {
    const auto found = std::ranges::find(snapshot().tracks, id, &project::Track::id);
    if (found != snapshot().tracks.end() && found->main_output)
      operations.emplace_back(project::SetTrackMainOutput{id, std::nullopt});
  }
  if (operations.empty())
    return foundation::Result<void>::success();
  auto applied = model().apply(operations);
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

foundation::Result<void> RoutingService::place_tracks(std::span<const project::TrackId> tracks,
                                                      std::optional<project::TrackId> bus,
                                                      std::optional<project::TrackId> before) {
  using Result = foundation::Result<void>;
  const auto& current = snapshot();
  const auto find = [&](project::TrackId id) {
    return std::ranges::find(current.tracks, id, &project::Track::id);
  };
  if (bus) {
    const auto found = find(*bus);
    if (found == current.tracks.end() || found->kind != project::TrackKind::group_bus)
      return Result::failure({foundation::ErrorCode::invalid_argument, "Choose a group bus"});
  }
  if (before && find(*before) == current.tracks.end())
    return Result::failure(
        {foundation::ErrorCode::not_found, "The track to drop beside no longer exists"});
  std::vector<project::TrackId> chosen;
  for (const auto id : tracks) {
    const auto found = find(id);
    if (found == current.tracks.end())
      return Result::failure(
          {foundation::ErrorCode::not_found, "A selected track no longer exists"});
    if (bus && is_bus(*found))
      return Result::failure({foundation::ErrorCode::invalid_argument,
                              "A bus can't go inside a group bus; it stays on the master"});
    if (std::ranges::find(chosen, id) == chosen.end())
      chosen.push_back(id);
  }
  if (chosen.empty())
    return Result::failure({foundation::ErrorCode::invalid_argument, "Select tracks to move"});
  const auto is_chosen = [&](project::TrackId id) {
    return std::ranges::find(chosen, id) != chosen.end();
  };

  // The order after the drop: the others keep theirs, and the chosen tracks,
  // in their saved order, go in at the drop point.
  std::vector<project::TrackId> order;
  std::vector<project::TrackId> moving;
  for (const auto& track : current.tracks)
    (is_chosen(track.id) ? moving : order).push_back(track.id);
  auto at = order.end();
  if (before) {
    // A chosen track as the anchor stands for its own place.
    std::size_t kept = 0;
    for (const auto& track : current.tracks) {
      if (track.id == *before)
        break;
      kept += is_chosen(track.id) ? 0 : 1;
    }
    at = order.begin() + static_cast<std::ptrdiff_t>(kept);
  } else if (bus) {
    // After the bus's last member, or the bus itself.
    at = std::ranges::find(order, *bus) + 1;
    for (auto item = at; item != order.end(); ++item) {
      if (find(*item)->main_output == bus)
        at = item + 1;
    }
  }
  order.insert(at, moving.begin(), moving.end());

  std::vector<project::Operation> operations;
  for (const auto id : chosen) {
    if (find(id)->main_output != bus)
      operations.emplace_back(project::SetTrackMainOutput{id, bus});
  }
  std::vector<project::TrackId> now;
  now.reserve(current.tracks.size());
  for (const auto& track : current.tracks)
    now.push_back(track.id);
  for (std::size_t index = 0; index < order.size(); ++index) {
    if (now[index] == order[index])
      continue;
    const auto from = std::ranges::find(now, order[index]);
    const auto id = *from;
    now.erase(from);
    now.insert(now.begin() + static_cast<std::ptrdiff_t>(index), id);
    operations.emplace_back(project::MoveTrack{id, index});
  }
  if (operations.empty())
    return Result::success();
  auto applied = model().apply(operations);
  return applied ? Result::success() : Result::failure(applied.error());
}

foundation::Result<project::SendId>
RoutingService::add_send(project::TrackId track, project::TrackId destination, double gain) {
  auto id = numbered_id<project::SendId>(next_send_number());
  if (!id) {
    return foundation::Result<project::SendId>::failure(id.error());
  }
  auto applied = model().apply({project::AddSendRoute{track, {id.value(), destination, gain}}});
  if (!applied) {
    return foundation::Result<project::SendId>::failure(applied.error());
  }
  return foundation::Result<project::SendId>::success(id.value());
}

foundation::Result<project::SendId> RoutingService::add_sidechain(project::TrackId track,
                                                                  project::TrackId destination,
                                                                  project::InsertSlotId compressor,
                                                                  double gain) {
  auto id = numbered_id<project::SendId>(next_send_number());
  if (!id)
    return foundation::Result<project::SendId>::failure(id.error());
  auto applied = model().apply({project::AddSendRoute{
      track, {id.value(), destination, gain, project::SendPurpose::sidechain, compressor}}});
  if (!applied)
    return foundation::Result<project::SendId>::failure(applied.error());
  return foundation::Result<project::SendId>::success(id.value());
}

foundation::Result<void>
RoutingService::set_sidechain_source(project::TrackId destination, project::InsertSlotId effect,
                                     std::optional<project::TrackId> source, double gain) {
  using Result = foundation::Result<void>;
  std::vector<project::Operation> operations;
  bool kept = false;
  for (const auto& track : snapshot().tracks) {
    for (const auto& send : track.sends) {
      if (send.purpose != project::SendPurpose::sidechain || send.destination != destination ||
          send.target_insert != effect)
        continue;
      if (source && track.id == *source && !kept) {
        kept = true;
        if (send.gain != gain)
          operations.push_back(project::SetSendGain{send.id, gain});
      } else {
        operations.push_back(project::RemoveSendRoute{send.id});
      }
    }
  }
  if (source && !kept) {
    auto id = numbered_id<project::SendId>(next_send_number());
    if (!id)
      return Result::failure(id.error());
    operations.push_back(project::AddSendRoute{
        *source, {id.value(), destination, gain, project::SendPurpose::sidechain, effect}});
  }
  if (operations.empty())
    return Result::success();
  auto applied = model().apply(operations);
  if (!applied)
    return Result::failure(applied.error());
  return Result::success();
}

foundation::Result<void> RoutingService::set_send_gain(project::SendId send, double gain) {
  auto applied = model().apply({project::SetSendGain{send, gain}});
  if (!applied) {
    return foundation::Result<void>::failure(applied.error());
  }
  return foundation::Result<void>::success();
}

foundation::Result<void> RoutingService::remove_send(project::SendId send) {
  auto applied = model().apply({project::RemoveSendRoute{send}});
  if (!applied) {
    return foundation::Result<void>::failure(applied.error());
  }
  return foundation::Result<void>::success();
}

foundation::Result<void> RoutingService::set_send_level(project::TrackId track,
                                                        project::TrackId return_track, double gain,
                                                        std::optional<std::uint64_t> gesture) {
  using Result = foundation::Result<void>;
  const auto source = std::ranges::find(snapshot().tracks, track, &project::Track::id);
  if (source == snapshot().tracks.end())
    return Result::failure({foundation::ErrorCode::not_found, "Send track not found"});
  const auto send = std::ranges::find_if(source->sends, [&](const project::SendRoute& route) {
    return route.destination == return_track && route.purpose == project::SendPurpose::audible;
  });
  if (send != source->sends.end()) {
    if (send->gain == gain)
      return Result::success();
    auto applied = model().apply({project::SetSendGain{send->id, gain}}, gesture);
    return applied ? Result::success() : Result::failure(applied.error());
  }
  auto id = numbered_id<project::SendId>(next_send_number());
  if (!id)
    return Result::failure(id.error());
  auto applied =
      model().apply({project::AddSendRoute{track, {id.value(), return_track, gain}}}, gesture);
  return applied ? Result::success() : Result::failure(applied.error());
}

} // namespace oscillate::application

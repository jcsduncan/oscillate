#include "oscillate/project/frozen_view.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <variant>

namespace oscillate::project {
namespace {

// "FROZEN" in the high bits keeps the render's clip id clear of the
// session's numbered clip ids.
constexpr std::uint64_t render_clip_tag = 0x46'52'4f'5a'45'4e'00'00ULL;

// Lanes a frozen track keeps: what happens after its render (the channel and
// its sends). Everything before the fader is in the render.
[[nodiscard]] bool plays_after_render(const AutomationLane& lane, const Track& track) {
  if (const auto* control = std::get_if<TrackControlTarget>(&lane.target))
    return is_channel_control(control->key);
  if (const auto* send = std::get_if<SendGainTarget>(&lane.target))
    return std::ranges::any_of(track.sends,
                               [&](const SendRoute& route) { return route.id == send->send; });
  return false;
}

} // namespace

bool has_frozen_track(const Snapshot& snapshot) noexcept {
  return std::ranges::any_of(snapshot.tracks,
                             [](const Track& track) { return track.frozen.has_value(); });
}

// NOLINTNEXTLINE(bugprone-exception-escape): render_clip_tag is nonzero, so no ID here is nil.
ClipId frozen_render_clip(TrackId track) noexcept {
  auto bits = track.bits();
  bits.high ^= render_clip_tag;
  if (auto id = ClipId::from_bits(bits))
    return id.value();
  return ClipId::from_bits({render_clip_tag, 1}).value();
}

std::optional<Snapshot> frozen_playback_view(const Snapshot& snapshot) {
  if (!has_frozen_track(snapshot))
    return std::nullopt;
  auto view = snapshot;
  std::vector<InsertSlotId> frozen_slots;
  for (auto& track : view.tracks) {
    if (!track.frozen)
      continue;
    const auto frozen = *track.frozen;
    for (const auto& slot : track.effects)
      frozen_slots.push_back(slot.id);
    track.frozen.reset();
    track.kind = TrackKind::audio;
    track.instrument_source = std::monostate{};
    track.midi_clips.clear();
    track.note_effects.clear();
    track.effects.clear();
    track.macros.clear();
    std::erase_if(track.automation,
                  [&](const AutomationLane& lane) { return !plays_after_render(lane, track); });
    const auto first = static_cast<std::int64_t>(frozen.first_frame);
    const auto frames = static_cast<std::int64_t>(frozen.audio.frames);
    Clip render{frozen_render_clip(track.id),
                frozen.audio.source,
                {{0}, {frames}},
                ElapsedInterval{{first}, {first + frames}}};
    track.clips.clear();
    track.clips.push_back(std::move(render));
  }
  // A key into a frozen track's inserts was heard when the render was made.
  for (auto& track : view.tracks) {
    std::vector<SendId> dropped;
    for (const auto& send : track.sends) {
      if (send.purpose == SendPurpose::sidechain && send.target_insert &&
          std::ranges::find(frozen_slots, *send.target_insert) != frozen_slots.end())
        dropped.push_back(send.id);
    }
    if (dropped.empty())
      continue;
    std::erase_if(track.sends, [&](const SendRoute& send) {
      return std::ranges::find(dropped, send.id) != dropped.end();
    });
    std::erase_if(track.automation, [&](const AutomationLane& lane) {
      const auto* send = std::get_if<SendGainTarget>(&lane.target);
      return send != nullptr && std::ranges::find(dropped, send->send) != dropped.end();
    });
  }
  return view;
}

Snapshot audible_snapshot(Snapshot snapshot) {
  if (auto view = frozen_playback_view(snapshot))
    return std::move(*view);
  return snapshot;
}

} // namespace oscillate::project

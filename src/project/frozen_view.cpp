#include "oscillate/project/frozen_view.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::project {
namespace {

// "FROZEN" in the high bits keeps the render's clip id clear of the
// session's numbered clip ids.
constexpr std::uint64_t render_clip_tag = 0x46'52'4f'5a'45'4e'00'00ULL;

// The bits a slice index may take in a render clip's id, below the tag.
constexpr std::uint64_t render_slice_mask = 0xFF'FFULL;

using foundation::ErrorCode;
using Span = std::pair<std::uint64_t, std::uint64_t>;

// Lanes a frozen track keeps: what happens after its render (the channel and
// its sends). Everything before the fader is in the render.
[[nodiscard]] bool keeps_after_render(const AutomationLane& lane, const Track& track) {
  if (const auto* send = std::get_if<SendGainTarget>(&lane.target))
    return std::ranges::any_of(track.sends,
                               [&](const SendRoute& route) { return route.id == send->send; });
  return plays_after_render(lane);
}

// A clip of the track with lanes that play after its render, and where it
// sounds.
struct LanedClip {
  const Placement* placement;
  const std::vector<AutomationLane>* automation;
  Span span;
};

[[nodiscard]] foundation::Result<Span>
clip_span(const Placement& placement, const timeline::TempoMap& tempo, std::uint32_t sample_rate) {
  using R = foundation::Result<Span>;
  std::int64_t begin = 0;
  std::int64_t end = 0;
  if (const auto* musical = std::get_if<MusicalInterval>(&placement)) {
    auto first = tempo.tick_to_sample(musical->start, sample_rate);
    auto last = tempo.tick_to_sample(musical->end, sample_rate);
    if (!first || !last)
      return R::failure(!first ? first.error() : last.error());
    begin = first.value().value;
    end = last.value().value;
  } else {
    const auto& elapsed = std::get<ElapsedInterval>(placement);
    begin = elapsed.start.value;
    end = elapsed.end.value;
  }
  if (begin < 0 || end <= begin)
    return R::failure({ErrorCode::invalid_argument, "A clip renders to no frames"});
  return R::success({static_cast<std::uint64_t>(begin), static_cast<std::uint64_t>(end)});
}

[[nodiscard]] foundation::Result<std::vector<LanedClip>>
laned_clips(const Track& track, const timeline::TempoMap& tempo, std::uint32_t sample_rate) {
  using R = foundation::Result<std::vector<LanedClip>>;
  std::vector<LanedClip> found;
  const auto add = [&](const Placement& placement, const std::vector<AutomationLane>& lanes) {
    if (std::ranges::none_of(lanes, plays_after_render))
      return foundation::Result<void>::success();
    auto span = clip_span(placement, tempo, sample_rate);
    if (!span)
      return foundation::Result<void>::failure(span.error());
    found.push_back({&placement, &lanes, span.value()});
    return foundation::Result<void>::success();
  };
  for (const auto& clip : track.clips)
    if (auto added = add(clip.placement, clip.automation); !added)
      return R::failure(added.error());
  for (const auto& clip : track.midi_clips)
    if (auto added = add(clip.placement, clip.automation); !added)
      return R::failure(added.error());
  std::ranges::sort(found, {}, [](const LanedClip& clip) { return clip.span.first; });
  return R::success(std::move(found));
}

} // namespace

bool plays_after_render(const AutomationLane& lane) noexcept {
  if (const auto* control = std::get_if<TrackControlTarget>(&lane.target))
    return is_channel_control(control->key);
  return std::holds_alternative<SendGainTarget>(lane.target);
}

foundation::Result<std::optional<Span>> after_render_clip_span(const Track& track,
                                                               const timeline::TempoMap& tempo,
                                                               std::uint32_t sample_rate) {
  using R = foundation::Result<std::optional<Span>>;
  auto clips = laned_clips(track, tempo, sample_rate);
  if (!clips)
    return R::failure(clips.error());
  if (clips.value().empty())
    return R::success(std::nullopt);
  Span span = clips.value().front().span;
  for (const auto& clip : clips.value())
    span.second = std::max(span.second, clip.span.second);
  return R::success(span);
}

foundation::Result<std::vector<RenderSlice>>
render_slices(const Track& track, const timeline::TempoMap& tempo, std::uint32_t sample_rate,
              std::uint64_t first, std::uint64_t frames) {
  using R = foundation::Result<std::vector<RenderSlice>>;
  if (frames == 0 ||
      first > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - frames)
    return R::failure({ErrorCode::invalid_argument, "A render has no frames"});
  auto clips = laned_clips(track, tempo, sample_rate);
  if (!clips)
    return R::failure(clips.error());
  const auto last = first + frames;
  std::vector<RenderSlice> slices;
  std::uint64_t cursor = first;
  const auto plain = [&](std::uint64_t until) {
    if (until <= cursor)
      return;
    slices.push_back(
        {{{static_cast<std::int64_t>(cursor - first)}, {static_cast<std::int64_t>(until - first)}},
         ElapsedInterval{{static_cast<std::int64_t>(cursor)}, {static_cast<std::int64_t>(until)}},
         {}});
    cursor = until;
  };
  for (const auto& clip : clips.value()) {
    const auto [begin, end] = clip.span;
    if (end <= first || begin >= last)
      continue;
    if (begin < first || end > last)
      return R::failure(
          {ErrorCode::conflict, "A clip's channel envelope crosses the edge of the render"});
    if (begin < cursor)
      return R::failure({ErrorCode::conflict,
                         "Clips with channel envelopes overlap; the render cannot keep them"});
    plain(begin);
    RenderSlice slice{
        {{static_cast<std::int64_t>(begin - first)}, {static_cast<std::int64_t>(end - first)}},
        *clip.placement,
        {}};
    std::ranges::copy_if(*clip.automation, std::back_inserter(slice.automation),
                         plays_after_render);
    slices.push_back(std::move(slice));
    cursor = end;
  }
  plain(last);
  return R::success(std::move(slices));
}

bool has_frozen_track(const Snapshot& snapshot) noexcept {
  return std::ranges::any_of(snapshot.tracks,
                             [](const Track& track) { return track.frozen.has_value(); });
}

// NOLINTNEXTLINE(bugprone-exception-escape): render_clip_tag is nonzero, so no ID here is nil.
ClipId frozen_render_clip(TrackId track, std::size_t slice) noexcept {
  auto bits = track.bits();
  bits.high ^= render_clip_tag | (static_cast<std::uint64_t>(slice) & render_slice_mask);
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
    // The render was taken before the channel: its clips' channel lanes
    // still play over it (ADR-0102). A freeze is refused unless its render
    // slices, so a failure here keeps the render whole.
    auto slices =
        render_slices(track, view.tempo, view.sample_rate, frozen.first_frame, frozen.audio.frames);
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
                  [&](const AutomationLane& lane) { return !keeps_after_render(lane, track); });
    track.clips.clear();
    if (!slices || slices.value().size() > render_slice_mask) {
      const auto first = static_cast<std::int64_t>(frozen.first_frame);
      const auto frames = static_cast<std::int64_t>(frozen.audio.frames);
      track.clips.emplace_back(frozen_render_clip(track.id), frozen.audio.source,
                               SourceRange{{0}, {frames}},
                               ElapsedInterval{{first}, {first + frames}});
      continue;
    }
    for (auto& slice : slices.value()) {
      std::erase_if(slice.automation,
                    [&](const AutomationLane& lane) { return !keeps_after_render(lane, track); });
      track.clips.emplace_back(frozen_render_clip(track.id, track.clips.size()),
                               frozen.audio.source, slice.range, slice.placement, 1,
                               std::move(slice.automation));
    }
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

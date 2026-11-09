#include "oscillate/application/session/midi_clip_service.hpp"

#include "edit_support.hpp"
#include "oscillate/project/lookup.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <span>
#include <utility>
#include <variant>
#include <vector>

// Consolidating and merging MIDI clips into one.
namespace oscillate::application {
namespace {
using foundation::ErrorCode;
using project::find_track;
template <typename T> using Result = foundation::Result<T>;

} // namespace

Result<project::ClipId> MidiClipService::consolidate_midi_clips(project::TrackId track,
                                                                timeline::TickPosition start,
                                                                timeline::TickPosition end) {
  using R = Result<project::ClipId>;
  const auto& state = snapshot();
  const auto* owner = find_track(state, track);
  if (owner == nullptr)
    return R::failure({ErrorCode::not_found, "Track to consolidate not found"});
  if (end <= start)
    return R::failure({ErrorCode::invalid_argument, "Select a time range to consolidate"});
  std::vector<const project::MidiClip*> chosen;
  for (const auto& clip : owner->midi_clips) {
    const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement);
    if (musical != nullptr && musical->start < end && musical->end > start)
      chosen.push_back(&clip);
  }
  if (chosen.size() == 1 && !chosen.front()->loop)
    return R::failure({ErrorCode::invalid_argument,
                       "Select more than one clip, or a looped clip, to consolidate"});
  return merge_chosen_midi_clips(track, chosen);
}

Result<project::ClipId> MidiClipService::merge_midi_clips(std::span<const project::ClipId> clips) {
  using R = Result<project::ClipId>;
  if (clips.size() < 2)
    return R::failure({ErrorCode::invalid_argument, "Select two or more MIDI clips to merge"});
  const auto& state = snapshot();
  for (const auto& track : state.tracks) {
    std::vector<const project::MidiClip*> chosen;
    for (const auto& clip : track.midi_clips)
      if (std::ranges::find(clips, clip.id) != clips.end())
        chosen.push_back(&clip);
    if (chosen.empty())
      continue;
    if (chosen.size() != clips.size())
      return R::failure({ErrorCode::invalid_argument, "Merge joins MIDI clips on one track"});
    return merge_chosen_midi_clips(track.id, chosen);
  }
  return R::failure({ErrorCode::not_found, "No MIDI clips in the selection"});
}

Result<project::ClipId>
MidiClipService::merge_chosen_midi_clips(project::TrackId track,
                                         const std::vector<const project::MidiClip*>& clips) {
  using R = Result<project::ClipId>;
  const auto& state = snapshot();
  struct Span {
    const project::MidiClip* clip;
    std::int64_t first;
    std::int64_t last;
  };
  std::vector<Span> chosen;
  for (const auto* clip : clips) {
    const auto* musical = std::get_if<project::MusicalInterval>(&clip->placement);
    if (musical == nullptr)
      return R::failure(
          {ErrorCode::unsupported, "Switch the clips to bars and beats before joining them"});
    chosen.push_back({clip, musical->start.value, musical->end.value});
  }
  if (chosen.empty())
    return R::failure({ErrorCode::not_found, "No MIDI clips in the selection"});
  if (std::ranges::any_of(chosen, [](const Span& span) { return !span.clip->automation.empty(); }))
    return R::failure({ErrorCode::unsupported,
                       "Consolidate keeps notes and pedals; clear the clips' automation first"});
  const auto first = std::ranges::min(chosen, {}, &Span::first).first;
  const auto last = std::ranges::max(chosen, {}, &Span::last).last;

  // Every repeat written out where it sounds, as compiled playback reads it:
  // a note past the pattern is silent, one crossing it or the clip's end is cut.
  // Overlapping clips can repeat a note: one note per start and key, the
  // longest and loudest.
  const auto merge_repeated = [](std::vector<project::MidiNote> notes) {
    std::ranges::sort(notes, [](const project::MidiNote& a, const project::MidiNote& b) {
      return a.start != b.start ? a.start < b.start : a.key < b.key;
    });
    std::vector<project::MidiNote> merged;
    for (const auto& note : notes) {
      if (!merged.empty() && merged.back().start == note.start && merged.back().key == note.key) {
        merged.back().end = std::max(merged.back().end, note.end);
        merged.back().velocity = std::max(merged.back().velocity, note.velocity);
        continue;
      }
      merged.push_back(note);
    }
    return merged;
  };
  std::vector<project::MidiNote> notes;
  std::array<std::vector<project::MidiPedalPoint>, project::midi_pedal_count> pedals;
  for (const auto& span : chosen) {
    const auto& clip = *span.clip;
    const auto length = clip.pattern_length.value;
    if (length <= 0)
      return R::failure({ErrorCode::corrupt_data, "A MIDI clip has no pattern"});
    // The pattern's audible notes, merged as the whole result is: each repeat
    // then writes notes no other repeat of this clip writes, so a clip that
    // would write more than a clip holds fails at once rather than expanding
    // a short pattern over a long clip without bound.
    std::vector<project::MidiNote> pattern;
    for (const auto& note : clip.notes)
      if (project::note_plays(clip, note))
        pattern.push_back(note);
    pattern = merge_repeated(std::move(pattern));
    const bool pedalled = std::ranges::any_of(clip.pedals, [length](const auto& lane) {
      return !lane.points.empty() && lane.points.front().position.value < length;
    });
    const bool silent = pattern.empty() && !pedalled;
    std::size_t written = 0;
    std::array<std::size_t, project::midi_pedal_count> pedal_written{};
    for (std::int64_t base = span.first; !silent && base < span.last; base += length) {
      for (const auto& note : pattern) {
        const auto note_first = base + note.start.value;
        const auto note_last = std::min(base + std::min(note.end.value, length), span.last);
        if (note_first >= span.last || note_last <= note_first)
          continue;
        if (++written > project::max_midi_notes_per_clip)
          return R::failure(
              {ErrorCode::resource_exhausted, "The consolidated clip would have too many notes"});
        // Fresh ids replace the originals once the notes are merged.
        notes.push_back(
            {note.id, {note_first - first}, {note_last - first}, note.key, note.velocity});
      }
      for (const auto& lane : clip.pedals) {
        const auto index = static_cast<std::size_t>(lane.pedal);
        auto& points = pedals[index];
        // Points trimmed off the clip's left edge are silent; the last of
        // them holds from the repeat's start, as playback plays it.
        const auto inside = std::ranges::find_if(
            lane.points, [](const auto& point) { return point.position.value >= 0; });
        std::vector<project::MidiPedalPoint> played;
        if (inside != lane.points.begin() &&
            (inside == lane.points.end() || inside->position.value != 0))
          played.push_back({{0}, std::prev(inside)->value});
        played.insert(played.end(), inside, lane.points.end());
        for (const auto& point : played) {
          if (point.position.value >= length || base + point.position.value >= span.last)
            break;
          if (++pedal_written[index] > project::max_midi_pedal_points_per_lane)
            return R::failure({ErrorCode::resource_exhausted,
                               "The consolidated clip would have too many pedal points"});
          points.push_back({{base + point.position.value - first}, point.value});
        }
      }
      if (!clip.loop)
        break;
    }
    // A pedal down at a clip's end is let go there, as playback does.
    for (const auto& lane : clip.pedals) {
      auto& points = pedals[static_cast<std::size_t>(lane.pedal)];
      if (!points.empty() && points.back().value != 0.0 && span.last < last)
        points.push_back({{span.last - first}, 0.0});
    }
  }
  auto merged = merge_repeated(std::move(notes));
  if (merged.size() > project::max_midi_notes_per_clip)
    return R::failure(
        {ErrorCode::resource_exhausted, "The consolidated clip would have too many notes"});
  auto ids = fresh_identities(state.revision);
  auto clip_id = numbered_id<project::ClipId>(next_clip_number());
  if (!ids || !clip_id)
    return R::failure(!ids ? ids.error() : clip_id.error());
  for (auto& note : merged)
    note.id = project::NoteId::from_bits({ids.value().space, ids.value().next_note++}).value();
  std::vector<project::MidiPedalLane> lanes;
  for (std::size_t index = 0; index < pedals.size(); ++index) {
    auto& points = pedals[index];
    if (points.empty())
      continue;
    // The latest point at a position wins; positions strictly increase.
    std::ranges::stable_sort(points, {}, &project::MidiPedalPoint::position);
    std::vector<project::MidiPedalPoint> kept;
    for (const auto& point : points) {
      if (!kept.empty() && kept.back().position == point.position)
        kept.back() = point;
      else
        kept.push_back(point);
    }
    if (kept.size() > project::max_midi_pedal_points_per_lane)
      return R::failure({ErrorCode::resource_exhausted,
                         "The consolidated clip would have too many pedal points"});
    lanes.push_back({static_cast<project::MidiPedal>(index), std::move(kept)});
  }
  project::MidiClip consolidated{clip_id.value(),
                                 project::MusicalInterval{{first}, {last}},
                                 {last - first},
                                 false,
                                 std::move(merged),
                                 1,
                                 {},
                                 std::move(lanes)};
  consolidated.color = chosen.front().clip->color;
  std::vector<project::Operation> operations;
  operations.reserve(chosen.size());
  for (const auto& span : chosen)
    operations.emplace_back(project::RemoveClip{span.clip->id});
  operations.emplace_back(project::AddMidiClip{track, std::move(consolidated)});
  auto applied = model().apply(operations);
  return applied ? R::success(clip_id.value()) : R::failure(applied.error());
}

} // namespace oscillate::application

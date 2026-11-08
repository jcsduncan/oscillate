#include "oscillate/application/session/clipboard_service.hpp"

#include "edit_support.hpp"
#include "midi_clips.hpp"
#include "oscillate/application/key_cache.hpp"
#include "oscillate/application/midi_note_tools.hpp"
#include "oscillate/application/tempo_cache.hpp"
#include "oscillate/midi_file/smf.hpp"
#include "session_state.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::application {
namespace {

using ChannelKey = project::TrackControlTarget::Key;
using detail::find_midi_clip;
using detail::midi_anchor_tick;

// Clip lanes that name their track follow the clip to `to`; lanes bound to
// one track's own sends, plugins or effect slots cannot move and are dropped.
void retarget_lanes(std::vector<project::AutomationLane>& lanes, project::TrackId from,
                    project::TrackId to) {
  if (from == to)
    return;
  std::erase_if(lanes, [](const project::AutomationLane& lane) {
    return std::holds_alternative<project::SendGainTarget>(lane.target) ||
           std::holds_alternative<project::Vst3ParameterTarget>(lane.target) ||
           std::holds_alternative<project::NativeEffectParameterTarget>(lane.target) ||
           std::holds_alternative<project::MacroTarget>(lane.target);
  });
  for (auto& lane : lanes) {
    if (auto* control = std::get_if<project::TrackControlTarget>(&lane.target))
      control->track = to;
    else if (auto* native = std::get_if<project::NativeParameterTarget>(&lane.target))
      native->track = to;
  }
}

[[nodiscard]] foundation::Result<timeline::TickPosition>
placement_tick(const project::Snapshot& state, const project::Placement& placement, bool end) {
  if (const auto* musical = std::get_if<project::MusicalInterval>(&placement))
    return foundation::Result<timeline::TickPosition>::success(end ? musical->end : musical->start);
  const auto& elapsed = std::get<project::ElapsedInterval>(placement);
  return state.tempo.sample_to_tick(end ? elapsed.end : elapsed.start, state.sample_rate);
}

[[nodiscard]] const project::Placement&
clip_placement(const std::variant<project::Clip, project::MidiClip>& clip) {
  return std::visit([](const auto& item) -> const project::Placement& { return item.placement; },
                    clip);
}

} // namespace

foundation::Result<void> ClipboardService::copy_clips(std::span<const project::ClipId> clips) {
  using Result = foundation::Result<void>;
  if (clips.empty())
    return Result::failure({foundation::ErrorCode::invalid_argument, "Nothing to copy"});
  Clipboard copied;
  std::optional<std::size_t> first_row;
  std::optional<timeline::TickPosition> origin;
  const auto& state = snapshot();
  struct Found {
    std::size_t row;
    std::variant<project::Clip, project::MidiClip> clip;
    project::TrackId track;
  };
  std::vector<Found> found;
  for (std::size_t row = 0; row < state.tracks.size(); ++row) {
    const auto& track = state.tracks[row];
    for (const auto& clip : track.clips) {
      if (std::ranges::find(clips, clip.id) != clips.end())
        found.push_back({row, clip, track.id});
    }
    for (const auto& clip : track.midi_clips) {
      if (std::ranges::find(clips, clip.id) != clips.end())
        found.push_back({row, clip, track.id});
    }
  }
  if (found.size() != clips.size())
    return Result::failure({foundation::ErrorCode::not_found, "A clip to copy was not found"});
  for (auto& item : found) {
    auto start = placement_tick(state, clip_placement(item.clip), false);
    if (!start)
      return Result::failure(start.error());
    first_row = first_row ? std::min(*first_row, item.row) : item.row;
    origin = origin ? std::min(*origin, start.value()) : start.value();
    copied.clips.push_back({item.row, std::move(item.clip), start.value(), item.track});
  }
  // `found` holds every requested clip and there is at least one.
  if (!first_row || !origin)
    return Result::failure({foundation::ErrorCode::internal, "No clip was copied"});
  for (auto& entry : copied.clips)
    entry.track_offset -= *first_row;
  copied.clips_origin = *origin;
  clipboard_.clips = std::move(copied.clips);
  clipboard_.clips_origin = copied.clips_origin;
  return Result::success();
}

foundation::Result<void> ClipboardService::cut_clips(std::span<const project::ClipId> clips) {
  auto copied = copy_clips(clips);
  if (!copied)
    return copied;
  return remove_clips(clips);
}

foundation::Result<void> ClipboardService::remove_clips(std::span<const project::ClipId> clips) {
  if (clips.empty())
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::invalid_argument, "Select clips to delete"});
  std::vector<project::Operation> operations;
  for (const auto clip : clips)
    operations.emplace_back(project::RemoveClip{clip});
  auto applied = model().apply(operations);
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

foundation::Result<std::vector<project::ClipId>>
ClipboardService::paste_clips(project::TrackId track, timeline::TickPosition position) {
  using Result = foundation::Result<std::vector<project::ClipId>>;
  if (clipboard_.clips.empty())
    return Result::failure({foundation::ErrorCode::not_found, "The clipboard has no clips"});
  const auto& state = snapshot();
  const auto target = std::ranges::find(state.tracks, track, &project::Track::id);
  if (target == state.tracks.end())
    return Result::failure({foundation::ErrorCode::not_found, "Paste track not found"});
  const auto base_row = static_cast<std::size_t>(target - state.tracks.begin());
  auto ids = fresh_identities(state.revision);
  if (!ids)
    return Result::failure(ids.error());
  auto next_clip = next_clip_number();
  std::vector<project::Operation> operations;
  std::vector<project::ClipId> pasted;
  for (const auto& entry : clipboard_.clips) {
    const auto row = base_row + entry.track_offset;
    if (row >= state.tracks.size())
      return Result::failure(
          {foundation::ErrorCode::invalid_argument, "Not enough tracks below to paste onto"});
    const auto& destination = state.tracks[row];
    // Checked: a negative or near-maximum position must not wrap the start.
    const auto offset = entry.start.value - clipboard_.clips_origin.value;
    if (position.value < 0 || offset > std::numeric_limits<std::int64_t>::max() - position.value)
      return Result::failure(
          {foundation::ErrorCode::invalid_argument, "Cannot paste clips at that position"});
    const timeline::TickPosition start{position.value + offset};
    auto clip_id = numbered_id<project::ClipId>(next_clip);
    if (!clip_id)
      return Result::failure(clip_id.error());
    next_clip = next_number(next_clip);
    auto placed = audio_placement_at(clip_placement(entry.clip), start, state);
    if (!placed)
      return Result::failure(placed.error());
    if (const auto* audio = std::get_if<project::Clip>(&entry.clip)) {
      if (destination.kind != project::TrackKind::audio)
        return Result::failure(
            {foundation::ErrorCode::invalid_argument, "Audio clips paste onto audio tracks"});
      project::Clip copy = *audio;
      copy.id = clip_id.value();
      copy.placement = std::move(placed).value();
      retarget_lanes(copy.automation, entry.source_track, destination.id);
      refresh_lanes(copy.automation, ids.value());
      operations.emplace_back(project::AddClip{destination.id, std::move(copy)});
    } else {
      if (destination.kind != project::TrackKind::instrument)
        return Result::failure(
            {foundation::ErrorCode::invalid_argument, "MIDI clips paste onto instrument tracks"});
      project::MidiClip copy = std::get<project::MidiClip>(entry.clip);
      copy.id = clip_id.value();
      copy.placement = std::move(placed).value();
      retarget_lanes(copy.automation, entry.source_track, destination.id);
      refresh_lanes(copy.automation, ids.value());
      for (auto& note : copy.notes)
        note.id = project::NoteId::from_bits({ids.value().space, ids.value().next_note++}).value();
      operations.emplace_back(project::AddMidiClip{destination.id, std::move(copy)});
    }
    pasted.push_back(clip_id.value());
  }
  auto applied = model().apply(operations);
  return applied ? Result::success(std::move(pasted)) : Result::failure(applied.error());
}

foundation::Result<std::vector<project::ClipId>>
ClipboardService::duplicate_clips(std::span<const project::ClipId> clips) {
  using Result = foundation::Result<std::vector<project::ClipId>>;
  const auto saved = clipboard_;
  auto copied = copy_clips(clips);
  if (!copied)
    return Result::failure(copied.error());
  const auto& state = snapshot();
  // The selection spans its earliest start to its latest end.
  std::optional<timeline::TickPosition> end;
  for (const auto& entry : clipboard_.clips) {
    auto clip_end = placement_tick(state, clip_placement(entry.clip), true);
    if (!clip_end) {
      clipboard_ = saved;
      return Result::failure(clip_end.error());
    }
    end = end ? std::max(*end, clip_end.value()) : clip_end.value();
  }
  // Paste onto the first copied track's row, which is the topmost selected.
  std::optional<std::size_t> top;
  for (std::size_t row = 0; row < state.tracks.size() && !top; ++row) {
    const auto& track = state.tracks[row];
    const auto selected = [&](project::ClipId id) {
      return std::ranges::find(clips, id) != clips.end();
    };
    if (std::ranges::any_of(track.clips, [&](const auto& clip) { return selected(clip.id); }) ||
        std::ranges::any_of(track.midi_clips, [&](const auto& clip) { return selected(clip.id); }))
      top = row;
  }
  // The copy succeeded, so a selected clip sits on some row and has an end.
  if (!top || !end) {
    clipboard_ = saved;
    return Result::failure({foundation::ErrorCode::internal, "No copied clip to duplicate"});
  }
  auto pasted = paste_clips(state.tracks[*top].id, *end);
  clipboard_ = saved;
  return pasted;
}

foundation::Result<void> ClipboardService::move_clips(std::span<const project::ClipId> clips,
                                                      std::int64_t delta_ticks, int track_delta) {
  using foundation::ErrorCode;
  using Result = foundation::Result<void>;
  if (clips.empty())
    return Result::failure({ErrorCode::invalid_argument, "Select clips to move"});
  if (delta_ticks == 0 && track_delta == 0)
    return Result::success();
  const auto& state = snapshot();
  const auto rows = static_cast<std::ptrdiff_t>(state.tracks.size());
  std::vector<project::Operation> removals;
  std::vector<project::Operation> placements;
  std::size_t found = 0;
  for (std::ptrdiff_t row = 0; row < rows; ++row) {
    const auto& track = state.tracks[static_cast<std::size_t>(row)];
    const auto selected = [&](project::ClipId id) {
      return std::ranges::find(clips, id) != clips.end();
    };
    const auto destination_row = row + track_delta;
    const auto destination = [&]() -> const project::Track* {
      return destination_row < 0 || destination_row >= rows
                 ? nullptr
                 : &state.tracks[static_cast<std::size_t>(destination_row)];
    };
    // Each clip's new placement at its own start plus the delta.
    const auto moved_placement =
        [&](const project::Placement& placement) -> foundation::Result<project::Placement> {
      auto start = placement_tick(state, placement, false);
      if (!start)
        return foundation::Result<project::Placement>::failure(start.error());
      const timeline::TickPosition new_start{start.value().value + delta_ticks};
      if (new_start.value < 0)
        return foundation::Result<project::Placement>::failure(
            {ErrorCode::invalid_argument, "The clips cannot start before bar 1"});
      return audio_placement_at(placement, new_start, state);
    };
    for (const auto& clip : track.clips) {
      if (!selected(clip.id))
        continue;
      ++found;
      const auto* target = destination();
      if (target == nullptr || target->kind != project::TrackKind::audio)
        return Result::failure(
            {ErrorCode::invalid_argument, "Audio clips can only move onto audio tracks"});
      auto placement = moved_placement(clip.placement);
      if (!placement)
        return Result::failure(placement.error());
      if (track_delta == 0) {
        placements.emplace_back(project::SetClipPlacement{clip.id, std::move(placement).value()});
        continue;
      }
      auto copy = clip;
      copy.placement = std::move(placement).value();
      retarget_lanes(copy.automation, track.id, target->id);
      removals.emplace_back(project::RemoveClip{clip.id});
      placements.emplace_back(project::AddClip{target->id, std::move(copy)});
    }
    for (const auto& clip : track.midi_clips) {
      if (!selected(clip.id))
        continue;
      ++found;
      const auto* target = destination();
      if (target == nullptr || target->kind != project::TrackKind::instrument)
        return Result::failure(
            {ErrorCode::invalid_argument, "MIDI clips can only move onto instrument tracks"});
      auto placement = moved_placement(clip.placement);
      if (!placement)
        return Result::failure(placement.error());
      if (track_delta == 0) {
        placements.emplace_back(
            project::SetMidiClipPlacement{clip.id, std::move(placement).value()});
        continue;
      }
      auto copy = clip;
      copy.placement = std::move(placement).value();
      retarget_lanes(copy.automation, track.id, target->id);
      removals.emplace_back(project::RemoveClip{clip.id});
      placements.emplace_back(project::AddMidiClip{target->id, std::move(copy)});
    }
  }
  if (found != clips.size())
    return Result::failure({ErrorCode::not_found, "A selected clip was not found"});
  // Every clip leaves before any arrives, so clips swapping rows never meet.
  removals.insert(removals.end(), std::make_move_iterator(placements.begin()),
                  std::make_move_iterator(placements.end()));
  auto applied = model().apply(removals);
  return applied ? Result::success() : Result::failure(applied.error());
}

foundation::Result<void> ClipboardService::copy_notes(project::ClipId clip,
                                                      std::span<const project::NoteId> notes) {
  using Result = foundation::Result<void>;
  for (const auto& track : snapshot().tracks) {
    const auto found = std::ranges::find(track.midi_clips, clip, &project::MidiClip::id);
    if (found == track.midi_clips.end())
      continue;
    std::vector<project::MidiNote> copied;
    for (const auto& note : found->notes) {
      if (std::ranges::find(notes, note.id) != notes.end())
        copied.push_back(note);
    }
    if (copied.empty() || copied.size() != notes.size())
      return Result::failure({foundation::ErrorCode::not_found, "A note to copy was not found"});
    const auto origin = std::ranges::min(copied, {}, [](const project::MidiNote& note) {
                          return note.start;
                        }).start;
    for (auto& note : copied) {
      note.start = {note.start.value - origin.value};
      note.end = {note.end.value - origin.value};
    }
    clipboard_.notes = std::move(copied);
    return Result::success();
  }
  return Result::failure({foundation::ErrorCode::not_found, "MIDI clip not found"});
}

foundation::Result<std::vector<project::NoteId>>
ClipboardService::paste_notes(project::ClipId clip, timeline::TickPosition position) {
  using Result = foundation::Result<std::vector<project::NoteId>>;
  if (clipboard_.notes.empty())
    return Result::failure({foundation::ErrorCode::not_found, "The clipboard has no notes"});
  const auto* found = find_midi_clip(snapshot(), clip);
  if (!found)
    return Result::failure({foundation::ErrorCode::not_found, "MIDI clip not found"});
  auto ids = fresh_identities(snapshot().revision);
  if (!ids)
    return Result::failure(ids.error());
  std::vector<project::Operation> operations;
  std::vector<project::NoteId> pasted;
  for (auto note : clipboard_.notes) {
    // Pasted notes land inside the pattern, where they play.
    if (position.value < 0 || note.end.value > found->pattern_length.value - position.value)
      return Result::failure(
          {foundation::ErrorCode::invalid_argument, "Pasted notes would end past the MIDI clip"});
    note.id = project::NoteId::from_bits({ids.value().space, ids.value().next_note++}).value();
    note.start = {note.start.value + position.value};
    note.end = {note.end.value + position.value};
    pasted.push_back(note.id);
    operations.emplace_back(project::AddMidiNote{clip, note});
  }
  auto applied = model().apply(operations);
  return applied ? Result::success(std::move(pasted)) : Result::failure(applied.error());
}

foundation::Result<std::vector<project::NoteId>>
ClipboardService::duplicate_notes(project::ClipId clip, std::span<const project::NoteId> notes) {
  using Result = foundation::Result<std::vector<project::NoteId>>;
  const auto saved = clipboard_.notes;
  std::optional<timeline::TickPosition> start;
  std::optional<timeline::TickPosition> end;
  for (const auto& track : snapshot().tracks) {
    const auto found = std::ranges::find(track.midi_clips, clip, &project::MidiClip::id);
    if (found == track.midi_clips.end())
      continue;
    for (const auto& note : found->notes) {
      if (std::ranges::find(notes, note.id) == notes.end())
        continue;
      start = start ? std::min(*start, note.start) : note.start;
      end = end ? std::max(*end, note.end) : note.end;
    }
  }
  if (!start || !end)
    return Result::failure({foundation::ErrorCode::not_found, "No notes to duplicate"});
  auto copied = copy_notes(clip, notes);
  if (!copied) {
    clipboard_.notes = saved;
    return Result::failure(copied.error());
  }
  auto pasted = paste_notes(clip, *end);
  clipboard_.notes = saved;
  return pasted;
}

} // namespace oscillate::application

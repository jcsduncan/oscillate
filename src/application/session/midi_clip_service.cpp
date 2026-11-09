#include "oscillate/application/session/midi_clip_service.hpp"

#include "edit_support.hpp"
#include "midi_clips.hpp"
#include "oscillate/application/midi_note_tools.hpp"
#include "oscillate/foundation/midi.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::application {
namespace {
using detail::find_midi_clip;
using detail::midi_anchor_tick;
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

// The clip's length in ticks from its anchor. An elapsed clip's end is a
// sample, which maps back to a tick only to within a sample; when its
// pattern already ends exactly there, that pattern is the length.
[[nodiscard]] foundation::Result<std::int64_t> midi_clip_ticks(const project::Snapshot& state,
                                                               const project::MidiClip& clip) {
  using Result = foundation::Result<std::int64_t>;
  auto anchor = midi_anchor_tick(state, clip);
  if (!anchor)
    return Result::failure(anchor.error());
  std::int64_t length = 0;
  if (const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement)) {
    length = musical->end.value - musical->start.value;
  } else {
    const auto& elapsed = std::get<project::ElapsedInterval>(clip.placement);
    const auto end = state.tempo.sample_to_tick(elapsed.end, state.sample_rate);
    if (!end)
      return Result::failure(end.error());
    length = end.value().value - anchor.value().value;
    const auto anchor_sample = state.tempo.tick_to_sample(anchor.value(), state.sample_rate);
    if (anchor.value().value <=
        std::numeric_limits<std::int64_t>::max() - clip.pattern_length.value) {
      const auto pattern_end = state.tempo.tick_to_sample(
          {anchor.value().value + clip.pattern_length.value}, state.sample_rate);
      if (anchor_sample && pattern_end &&
          pattern_end.value().value - anchor_sample.value().value ==
              elapsed.end.value - elapsed.start.value)
        length = clip.pattern_length.value;
    }
  }
  if (length <= 0)
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "MIDI clip has no musical length"});
  return Result::success(length);
}

} // namespace

foundation::Result<void> MidiClipService::add_midi_clip(project::TrackId track,
                                                        timeline::TickPosition start) {
  constexpr auto length = timeline::ticks_per_quarter * 4;
  if (start.value < 0 || start.value > std::numeric_limits<std::int64_t>::max() - length) {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::invalid_argument, "Clip position is out of range"});
  }
  auto id = numbered_id<project::ClipId>(next_clip_number());
  if (!id) {
    return foundation::Result<void>::failure(id.error());
  }
  project::MidiClip clip{
      id.value(), project::MusicalInterval{start, {start.value + length}}, {length}, false, {}, 1};
  auto applied = model().apply({project::AddMidiClip{track, std::move(clip)}});
  if (!applied) {
    return foundation::Result<void>::failure(applied.error());
  }
  return foundation::Result<void>::success();
}

foundation::Result<project::ClipId>
MidiClipService::add_midi_clip_with_notes(project::TrackId track, timeline::TickPosition start,
                                          timeline::TickPosition length,
                                          std::vector<project::MidiNote> notes) {
  using Result = foundation::Result<project::ClipId>;
  if (start.value < 0 || length.value <= 0 ||
      start.value > std::numeric_limits<std::int64_t>::max() - length.value) {
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Clip position or length is out of range"});
  }
  if (notes.size() > project::max_midi_notes_per_clip) {
    return Result::failure(
        {foundation::ErrorCode::resource_exhausted, "Too many notes for a clip"});
  }
  auto id = numbered_id<project::ClipId>(next_clip_number());
  if (!id) {
    return Result::failure(id.error());
  }
  const auto first_note = next_note_number();
  if (first_note == 0 || first_note > std::numeric_limits<std::uint64_t>::max() - notes.size()) {
    return Result::failure(
        {foundation::ErrorCode::resource_exhausted, "Project ID space exhausted"});
  }
  for (std::size_t index = 0; index < notes.size(); ++index) {
    auto note_id = numbered_id<project::NoteId>(first_note + index);
    if (!note_id) {
      return Result::failure(note_id.error());
    }
    notes[index].id = note_id.value();
  }
  project::MidiClip clip{
      id.value(),       project::MusicalInterval{start, {start.value + length.value}},
      length,           false,
      std::move(notes), 1};
  auto applied = model().apply({project::AddMidiClip{track, std::move(clip)}});
  if (!applied) {
    return Result::failure(applied.error());
  }
  return Result::success(id.value());
}

foundation::Result<void> MidiClipService::quantize_midi_clip(project::ClipId clip,
                                                             int steps_per_bar) {
  using Result = foundation::Result<void>;
  if (!supported_midi_grid(steps_per_bar)) {
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Choose a supported MIDI grid"});
  }
  const auto grid = midi_grid_ticks(steps_per_bar);
  for (const auto& track : snapshot().tracks) {
    const auto found = std::ranges::find(track.midi_clips, clip, &project::MidiClip::id);
    if (found == track.midi_clips.end())
      continue;
    std::vector<project::Operation> changes;
    std::set<std::pair<std::int64_t, std::uint8_t>> destinations;
    const auto pattern = found->pattern_length.value;
    for (const auto& note : found->notes) {
      // Notes kept before tick 0 or after the pattern's end stay where they are.
      if (!project::note_plays(*found, note))
        continue;
      const auto duration = note.end.value - note.start.value;
      // A note inside the pattern stays inside; one crossing its end may.
      const auto latest = note.end.value <= pattern ? pattern - duration : pattern - 1;
      const auto quotient = note.start.value / grid;
      const auto remainder = note.start.value % grid;
      auto snapped = quotient * grid;
      if (remainder >= grid / 2 && snapped <= latest - grid)
        snapped += grid;
      snapped = std::min(snapped, latest);
      if (!destinations.emplace(snapped, note.key).second) {
        return Result::failure({foundation::ErrorCode::conflict,
                                "Quantise would place two notes at the same pitch and time"});
      }
      if (snapped != note.start.value) {
        auto changed = note;
        changed.start = {snapped};
        changed.end = {snapped + duration};
        changes.emplace_back(project::SetMidiNote{clip, changed});
      }
    }
    if (changes.empty())
      return Result::success();
    auto applied = model().apply(changes);
    return applied ? Result::success() : Result::failure(applied.error());
  }
  return Result::failure({foundation::ErrorCode::not_found, "MIDI clip not found"});
}

foundation::Result<void> MidiClipService::move_midi_clip(project::ClipId clip,
                                                         timeline::TickPosition new_start) {
  // Any position on the timeline: the arrangement snaps to its grid.
  if (new_start.value < 0) {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::invalid_argument, "MIDI clip cannot start before the timeline"});
  }
  for (const auto& track : snapshot().tracks) {
    const auto found = std::ranges::find_if(track.midi_clips,
                                            [clip](const auto& item) { return item.id == clip; });
    if (found == track.midi_clips.end()) {
      continue;
    }
    auto placement = audio_placement_at(found->placement, new_start, snapshot());
    if (!placement)
      return foundation::Result<void>::failure(placement.error());
    if (placement.value() == found->placement) {
      return foundation::Result<void>::success();
    }
    auto applied = model().apply({project::SetMidiClipPlacement{clip, placement.value()}});
    if (!applied) {
      return foundation::Result<void>::failure(applied.error());
    }
    return foundation::Result<void>::success();
  }
  return foundation::Result<void>::failure(
      {foundation::ErrorCode::not_found, "MIDI clip not found: " + clip.to_string()});
}

foundation::Result<void> MidiClipService::resize_midi_clip(project::ClipId clip,
                                                           timeline::TickPosition new_end) {
  using foundation::ErrorCode;
  const auto& state = snapshot();
  const auto* found = find_midi_clip(state, clip);
  if (!found)
    return foundation::Result<void>::failure({ErrorCode::not_found, "MIDI clip not found"});
  project::Placement placement = found->placement;
  if (auto* musical = std::get_if<project::MusicalInterval>(&placement)) {
    if (new_end <= musical->start)
      return foundation::Result<void>::failure(
          {ErrorCode::invalid_argument, "MIDI clip must end after it starts"});
    musical->end = new_end;
  } else {
    auto& elapsed = std::get<project::ElapsedInterval>(placement);
    auto end_sample = state.tempo.tick_to_sample(new_end, state.sample_rate);
    if (!end_sample)
      return foundation::Result<void>::failure(end_sample.error());
    if (end_sample.value() <= elapsed.start)
      return foundation::Result<void>::failure(
          {ErrorCode::invalid_argument, "MIDI clip must end after it starts"});
    elapsed.end = end_sample.value();
  }
  // A looping clip repeats its pattern to the new end; otherwise the
  // pattern is the clip, so lengthening it makes room for notes and
  // shortening it silences, but keeps, the notes after the new end.
  auto pattern = found->pattern_length;
  if (!found->loop) {
    auto anchor = midi_anchor_tick(state, *found);
    if (!anchor)
      return foundation::Result<void>::failure(anchor.error());
    if (new_end <= anchor.value())
      return foundation::Result<void>::failure(
          {ErrorCode::invalid_argument, "MIDI clip must end after it starts"});
    pattern = {new_end.value - anchor.value().value};
  }
  if (placement == found->placement && pattern == found->pattern_length)
    return foundation::Result<void>::success();
  std::vector<project::Operation> operations{project::SetMidiClipPlacement{clip, placement}};
  if (pattern != found->pattern_length)
    operations.emplace_back(project::SetMidiPattern{clip, pattern, found->loop});
  auto applied = model().apply(operations);
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

foundation::Result<void> MidiClipService::trim_midi_clip_start(project::ClipId clip,
                                                               timeline::TickPosition new_start) {
  using Result = foundation::Result<void>;
  const auto& state = snapshot();
  const auto* found = find_midi_clip(state, clip);
  if (!found)
    return Result::failure({ErrorCode::not_found, "MIDI clip not found: " + clip.to_string()});
  if (new_start.value < 0)
    return Result::failure({ErrorCode::invalid_argument, "MIDI clip cannot start before bar 1"});
  auto anchor = midi_anchor_tick(state, *found);
  if (!anchor)
    return Result::failure(anchor.error());
  project::Placement placement = found->placement;
  // The tick the notes will count from: an elapsed clip's new start sample
  // maps back to it through the tempo map, as playback maps its notes.
  auto counted_from = new_start;
  if (auto* musical = std::get_if<project::MusicalInterval>(&placement)) {
    if (new_start >= musical->end)
      return Result::failure({ErrorCode::invalid_argument, "MIDI clip must end after it starts"});
    musical->start = new_start;
  } else {
    auto& elapsed = std::get<project::ElapsedInterval>(placement);
    auto start_sample = state.tempo.tick_to_sample(new_start, state.sample_rate);
    if (!start_sample)
      return Result::failure(start_sample.error());
    if (start_sample.value() >= elapsed.end)
      return Result::failure({ErrorCode::invalid_argument, "MIDI clip must end after it starts"});
    elapsed.start = start_sample.value();
    auto mapped = state.tempo.sample_to_tick(elapsed.start, state.sample_rate);
    if (!mapped)
      return Result::failure(mapped.error());
    counted_from = mapped.value();
  }
  // Everything inside the clip shifts against the start, so it stays put on
  // the timeline.
  const auto shift = counted_from.value - anchor.value().value;
  if (shift == 0)
    return Result::success();
  // Each repeat of a loop starts a pattern after the last, so a loop that
  // repeats cannot move its start without moving every repeat after it.
  if (found->loop) {
    auto ticks = midi_clip_ticks(state, *found);
    if (!ticks)
      return Result::failure(ticks.error());
    if (ticks.value() > found->pattern_length.value)
      return Result::failure(
          {ErrorCode::unsupported, "Turn off the clip's loop to trim the start of its repeats"});
  }
  const auto pattern = found->pattern_length.value - shift;
  if (pattern <= 0)
    return Result::failure(
        {ErrorCode::invalid_argument, "Trim a looping MIDI clip inside its first pattern"});
  std::vector<project::Operation> operations{project::SetMidiClipPlacement{clip, placement},
                                             project::SetMidiPattern{clip, {pattern}, found->loop}};
  operations.reserve(operations.size() + found->notes.size() + found->pedals.size());
  // Positions come from the project file, so the shift is checked.
  const auto shifted = [shift](std::int64_t value) -> std::optional<std::int64_t> {
    if ((shift > 0 && value < std::numeric_limits<std::int64_t>::min() + shift) ||
        (shift < 0 && value > std::numeric_limits<std::int64_t>::max() + shift))
      return std::nullopt;
    return value - shift;
  };
  const foundation::Error out_of_range{ErrorCode::invalid_argument,
                                       "A note or pedal point is too far from the clip"};
  for (auto note : found->notes) {
    const auto start = shifted(note.start.value);
    const auto end = shifted(note.end.value);
    if (!start || !end)
      return Result::failure(out_of_range);
    note.start.value = *start;
    note.end.value = *end;
    operations.emplace_back(project::SetMidiNote{clip, note});
  }
  for (const auto& lane : found->pedals) {
    auto points = lane.points;
    for (auto& point : points) {
      const auto position = shifted(point.position.value);
      if (!position)
        return Result::failure(out_of_range);
      point.position.value = *position;
    }
    operations.emplace_back(project::SetMidiPedalLane{clip, lane.pedal, std::move(points)});
  }
  auto applied = model().apply(operations);
  return applied ? Result::success() : Result::failure(applied.error());
}

foundation::Result<void> MidiClipService::set_midi_clip_loop(project::ClipId clip, bool loop) {
  const auto& state = snapshot();
  const auto* found = find_midi_clip(state, clip);
  if (!found)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::not_found, "MIDI clip not found: " + clip.to_string()});
  if (found->loop == loop)
    return foundation::Result<void>::success();
  // Either way the pattern becomes the clip as it plays now: switching loop
  // on repeats everything visible; switching it off stops the repeats and
  // leaves the first pass's notes where they are.
  auto length = midi_clip_ticks(state, *found);
  if (!length)
    return foundation::Result<void>::failure(length.error());
  auto applied = model().apply({project::SetMidiPattern{clip, {length.value()}, loop}});
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

foundation::Result<void>
MidiClipService::replace_midi_clip_notes(project::ClipId clip, timeline::TickPosition length,
                                         std::vector<project::MidiNote> notes,
                                         std::optional<std::uint64_t> gesture) {
  using foundation::ErrorCode;
  using Result = foundation::Result<void>;
  const auto& state = snapshot();
  const auto* found = find_midi_clip(state, clip);
  if (!found)
    return Result::failure({ErrorCode::not_found, "MIDI clip not found: " + clip.to_string()});
  if (length.value <= 0)
    return Result::failure({ErrorCode::invalid_argument, "The pattern needs a length"});
  if (notes.size() > project::max_midi_notes_per_clip)
    return Result::failure({ErrorCode::resource_exhausted, "Too many notes for a clip"});
  for (const auto& note : notes) {
    if (note.start.value < 0 || note.end <= note.start || note.end > length ||
        !foundation::valid_midi_key(note.key) || !foundation::valid_note_velocity(note.velocity))
      return Result::failure({ErrorCode::invalid_argument, "A note is outside the pattern"});
  }
  const auto first_note = next_note_number();
  if (first_note == 0 || first_note > std::numeric_limits<std::uint64_t>::max() - notes.size())
    return Result::failure({ErrorCode::resource_exhausted, "Project ID space exhausted"});
  for (std::size_t index = 0; index < notes.size(); ++index) {
    auto note_id = numbered_id<project::NoteId>(first_note + index);
    if (!note_id)
      return Result::failure(note_id.error());
    notes[index].id = note_id.value();
  }
  std::vector<project::Operation> operations;
  operations.reserve(found->notes.size() + notes.size() + 2);
  for (const auto& note : found->notes)
    operations.emplace_back(project::RemoveMidiNote{clip, note.id});
  // A clip that does not loop is its pattern, so it ends where the new one
  // does; a looping clip keeps its length and repeats the new pattern.
  if (!found->loop) {
    auto anchor = midi_anchor_tick(state, *found);
    if (!anchor)
      return Result::failure(anchor.error());
    if (anchor.value().value > std::numeric_limits<std::int64_t>::max() - length.value)
      return Result::failure({ErrorCode::invalid_argument, "The pattern runs past the timeline"});
    const timeline::TickPosition new_end{anchor.value().value + length.value};
    project::Placement placement = found->placement;
    if (auto* musical = std::get_if<project::MusicalInterval>(&placement)) {
      musical->end = new_end;
    } else {
      auto& elapsed = std::get<project::ElapsedInterval>(placement);
      auto end_sample = state.tempo.tick_to_sample(new_end, state.sample_rate);
      if (!end_sample)
        return Result::failure(end_sample.error());
      if (end_sample.value() <= elapsed.start)
        return Result::failure({ErrorCode::invalid_argument, "The pattern is too short to place"});
      elapsed.end = end_sample.value();
    }
    if (placement != found->placement)
      operations.emplace_back(project::SetMidiClipPlacement{clip, placement});
  }
  if (length != found->pattern_length)
    operations.emplace_back(project::SetMidiPattern{clip, length, found->loop});
  for (auto& note : notes)
    operations.emplace_back(project::AddMidiNote{clip, note});
  if (operations.empty())
    return Result::success();
  auto applied = model().apply(operations, gesture);
  return applied ? Result::success() : Result::failure(applied.error());
}

foundation::Result<project::ClipId>
MidiClipService::duplicate_midi_clip_after(project::ClipId clip) {
  using Result = foundation::Result<project::ClipId>;
  for (const auto& track : snapshot().tracks) {
    const auto found = std::ranges::find(track.midi_clips, clip, &project::MidiClip::id);
    if (found == track.midi_clips.end())
      continue;
    auto id = numbered_id<project::ClipId>(next_clip_number());
    if (!id)
      return Result::failure(id.error());
    project::MidiClip copy = *found;
    copy.id = id.value();
    if (const auto* musical = std::get_if<project::MusicalInterval>(&found->placement)) {
      const auto length = musical->end.value - musical->start.value;
      if (musical->end.value > std::numeric_limits<std::int64_t>::max() - length)
        return Result::failure(
            {foundation::ErrorCode::invalid_argument, "Duplicated MIDI clip exceeds timeline"});
      copy.placement = project::MusicalInterval{musical->end, {musical->end.value + length}};
    } else {
      const auto& elapsed = std::get<project::ElapsedInterval>(found->placement);
      const auto length = elapsed.end.value - elapsed.start.value;
      if (elapsed.end.value > std::numeric_limits<std::int64_t>::max() - length)
        return Result::failure(
            {foundation::ErrorCode::invalid_argument, "Duplicated MIDI clip exceeds timeline"});
      copy.placement = project::ElapsedInterval{elapsed.end, {elapsed.end.value + length}};
    }
    const auto note_namespace = next_number(snapshot().revision);
    if (!copy.notes.empty() && note_namespace == 0)
      return Result::failure(
          {foundation::ErrorCode::resource_exhausted, "Project MIDI note ID space exhausted"});
    std::uint64_t note_number = 1;
    for (auto& note : copy.notes)
      note.id = project::NoteId::from_bits({note_namespace, note_number++}).value();
    auto automation_ids = assign_fresh_automation_ids(copy.automation, snapshot().revision);
    if (!automation_ids)
      return Result::failure(automation_ids.error());
    auto applied = model().apply({project::AddMidiClip{track.id, std::move(copy)}});
    return applied ? Result::success(id.value()) : Result::failure(applied.error());
  }
  return Result::failure(
      {foundation::ErrorCode::not_found, "MIDI clip not found: " + clip.to_string()});
}

foundation::Result<project::ClipId>
MidiClipService::split_midi_clip_at_tick(project::ClipId clip, timeline::TickPosition boundary) {
  using foundation::ErrorCode;
  using Result = foundation::Result<project::ClipId>;
  const auto& state = snapshot();
  for (const auto& track : state.tracks) {
    const auto found = std::ranges::find(track.midi_clips, clip, &project::MidiClip::id);
    if (found == track.midi_clips.end())
      continue;
    const auto* musical = std::get_if<project::MusicalInterval>(&found->placement);
    if (musical == nullptr)
      return Result::failure(
          {ErrorCode::unsupported, "Switch the clip to musical time to split it"});
    if (found->loop)
      return Result::failure({ErrorCode::unsupported, "Turn off the clip's loop to split it"});
    if (boundary.value <= musical->start.value || boundary.value >= musical->end.value)
      return Result::failure({ErrorCode::invalid_argument, "Split must be inside the MIDI clip"});
    const auto first = state.tempo.tick_to_sample(musical->start, state.sample_rate);
    const auto cut = state.tempo.tick_to_sample(boundary, state.sample_rate);
    if (!first || !cut)
      return Result::failure(!first ? first.error() : cut.error());
    const auto local = boundary.value - musical->start.value;
    const auto right_length = musical->end.value - boundary.value;
    auto id = numbered_id<project::ClipId>(next_clip_number());
    if (!id)
      return Result::failure(id.error());
    auto automation = split_clip_automation(state, found->automation, local,
                                            cut.value().value - first.value().value);
    if (!automation)
      return Result::failure(automation.error());
    const auto note_namespace = next_number(state.revision);
    if (note_namespace == 0)
      return Result::failure(
          {ErrorCode::resource_exhausted, "Project MIDI note ID space exhausted"});

    // A pattern shorter than the clip leaves silence after it; the right half
    // then holds only that silence.
    const auto left_pattern = std::min(found->pattern_length.value, local);
    const auto right_pattern =
        found->pattern_length.value > local ? found->pattern_length.value - local : right_length;
    // Notes and pedal points kept after the pattern's end stay, silent, in
    // the left half; the right half takes only what played.
    const auto pattern = found->pattern_length.value;
    std::vector<project::Operation> operations;
    std::vector<project::MidiNote> right_notes;
    std::uint64_t note_number = 1;
    for (const auto& note : found->notes) {
      if (note.start.value >= pattern)
        continue;
      if (note.start.value >= local) {
        operations.emplace_back(project::RemoveMidiNote{clip, note.id});
        auto moved = note;
        moved.id = project::NoteId::from_bits({note_namespace, note_number++}).value();
        moved.start.value -= local;
        moved.end.value -= local;
        right_notes.push_back(moved);
      } else if (note.end.value > local) {
        auto cropped = note;
        cropped.end.value = local;
        operations.emplace_back(project::SetMidiNote{clip, cropped});
      }
    }
    std::vector<project::MidiPedalLane> right_pedals;
    for (const auto& lane : found->pedals) {
      std::vector<project::MidiPedalPoint> left_points;
      std::vector<project::MidiPedalPoint> moved;
      double held = 0.0;
      for (const auto& point : lane.points) {
        if (point.position.value < local) {
          left_points.push_back(point);
          if (point.position.value < pattern)
            held = point.value;
        } else if (point.position.value < pattern) {
          moved.push_back({{point.position.value - local}, point.value});
        } else {
          left_points.push_back(point);
        }
      }
      if (local >= pattern)
        held = 0.0; // The right half is the silence after the pattern.
      if (left_points.size() != lane.points.size())
        operations.emplace_back(project::SetMidiPedalLane{clip, lane.pedal, left_points});
      project::MidiPedalLane right{lane.pedal, {}};
      if (held > 0.0 && (moved.empty() || moved.front().position.value != 0))
        right.points.push_back({{0}, held});
      right.points.insert(right.points.end(), moved.begin(), moved.end());
      if (!right.points.empty())
        right_pedals.push_back(std::move(right));
    }
    for (std::size_t index = 0; index < found->automation.size(); ++index) {
      auto& lane = automation.value().left[index];
      if (lane != found->automation[index])
        operations.emplace_back(project::ReplaceAutomationLane{lane.id, std::move(lane)});
    }
    operations.emplace_back(project::SetMidiPattern{clip, {left_pattern}, false});
    operations.emplace_back(
        project::SetMidiClipPlacement{clip, project::MusicalInterval{musical->start, boundary}});
    project::MidiClip right{id.value(),
                            project::MusicalInterval{boundary, musical->end},
                            {right_pattern},
                            false,
                            std::move(right_notes),
                            1,
                            std::move(automation.value().right),
                            std::move(right_pedals)};
    right.color = found->color;
    operations.emplace_back(project::AddMidiClip{track.id, std::move(right)});
    auto applied = model().apply(operations);
    return applied ? Result::success(id.value()) : Result::failure(applied.error());
  }
  return Result::failure({ErrorCode::not_found, "MIDI clip not found: " + clip.to_string()});
}

foundation::Result<void> MidiClipService::remove_midi_clip(project::ClipId clip) {
  const auto found = std::ranges::any_of(snapshot().tracks, [clip](const auto& track) {
    return std::ranges::any_of(track.midi_clips,
                               [clip](const auto& item) { return item.id == clip; });
  });
  if (!found)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::not_found, "MIDI clip not found: " + clip.to_string()});
  auto applied = model().apply({project::RemoveClip{clip}});
  if (!applied)
    return foundation::Result<void>::failure(applied.error());
  return foundation::Result<void>::success();
}

foundation::Result<void> MidiClipService::toggle_midi_clip_timebase(project::ClipId clip) {
  const auto& state = snapshot();
  for (const auto& track : state.tracks) {
    const auto found = std::ranges::find(track.midi_clips, clip, &project::MidiClip::id);
    if (found == track.midi_clips.end())
      continue;
    auto placement = convert_clip_timebase(state, found->placement);
    if (!placement)
      return foundation::Result<void>::failure(placement.error());
    auto applied = model().apply({project::SetMidiClipPlacement{clip, placement.value()}});
    return applied ? foundation::Result<void>::success()
                   : foundation::Result<void>::failure(applied.error());
  }
  return foundation::Result<void>::failure(
      {foundation::ErrorCode::not_found, "MIDI clip not found: " + clip.to_string()});
}

foundation::Result<void>
MidiClipService::move_midi_clip_to_track(project::ClipId clip, project::TrackId destination,
                                         timeline::TickPosition new_start) {
  const auto& state = snapshot();
  const auto target = std::ranges::find(state.tracks, destination, &project::Track::id);
  if (target == state.tracks.end())
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::not_found, "Destination track not found"});
  if (target->kind != project::TrackKind::instrument)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::invalid_argument, "MIDI clips go on instrument tracks"});
  for (const auto& track : state.tracks) {
    const auto found = std::ranges::find(track.midi_clips, clip, &project::MidiClip::id);
    if (found == track.midi_clips.end())
      continue;
    if (track.id == destination)
      return move_midi_clip(clip, new_start);
    if (new_start.value < 0)
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::invalid_argument, "MIDI clip cannot start before the timeline"});
    auto placement = audio_placement_at(found->placement, new_start, state);
    if (!placement)
      return foundation::Result<void>::failure(placement.error());
    auto moved = *found;
    moved.placement = std::move(placement).value();
    // Clip-owned lanes must target the track that holds the clip. Channel
    // lanes follow the clip; instrument lanes follow it onto another native
    // instrument. Anything else would be lost, so the move is refused.
    const auto* destination_patch =
        std::get_if<project::NativeInstrumentPatch>(&target->instrument_source);
    // An instrument lane moves only onto an engine with the same parameter
    // and domain, so synth preparation accepts it.
    const auto fits_destination = [&](const project::AutomationLane& lane,
                                      const project::NativeParameterTarget& native) {
      if (destination_patch == nullptr)
        return false;
      auto resolved = native_inserts::resolve_native_instrument(*destination_patch);
      if (!resolved)
        return false;
      auto domain =
          native_inserts::native_automation_domain(*resolved.value().engine, native.parameter_id);
      return domain && domain.value() == lane.domain;
    };
    for (auto& lane : moved.automation) {
      if (auto* control = std::get_if<project::TrackControlTarget>(&lane.target)) {
        control->track = destination;
      } else if (auto* native = std::get_if<project::NativeParameterTarget>(&lane.target);
                 native != nullptr && fits_destination(lane, *native)) {
        native->track = destination;
      } else {
        return foundation::Result<void>::failure(
            {foundation::ErrorCode::invalid_argument,
             "The clip's automation cannot move to that track"});
      }
    }
    auto applied = model().apply(
        {project::RemoveClip{clip}, project::AddMidiClip{destination, std::move(moved)}});
    return applied ? foundation::Result<void>::success()
                   : foundation::Result<void>::failure(applied.error());
  }
  return foundation::Result<void>::failure(
      {foundation::ErrorCode::not_found, "MIDI clip not found: " + clip.to_string()});
}

} // namespace oscillate::application

#include "oscillate/application/session/note_service.hpp"

#include "edit_support.hpp"
#include "midi_clips.hpp"
#include "oscillate/application/comp_patterns.hpp"
#include "oscillate/application/key_cache.hpp"
#include "oscillate/application/midi_note_tools.hpp"
#include "oscillate/application/tempo_cache.hpp"
#include "oscillate/foundation/midi.hpp"
#include "oscillate/midi_file/smf.hpp"
#include "session_state.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::application {
namespace {

using ChannelKey = project::TrackControlTarget::Key;
using detail::find_midi_clip;
using detail::midi_anchor_tick;
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

// A note for a clip, in its ticks, before it has an id.
struct LocalNote {
  std::int64_t start = 0;
  std::int64_t end = 0;
  std::uint8_t key = 0;
  std::uint8_t velocity = 0;
};

// A clip's place on the track in ticks, and what this take adds to it.
struct Span {
  project::ClipId id;
  std::int64_t start = 0;
  std::int64_t end = 0;
  std::int64_t pattern = 0;
  bool loop = false;
  bool take = false;    // the take's own clip, which may grow
  bool created = false; // made by this call
  std::int64_t grown_end = 0;
  std::set<std::pair<std::int64_t, std::uint8_t>> taken; // start, key
  std::vector<LocalNote> added;
};

} // namespace

foundation::Result<void> NoteService::add_note(project::ClipId clip, timeline::TickPosition start,
                                               std::uint8_t key) {
  return add_note_with_duration(clip, start, {timeline::ticks_per_quarter / 4}, key);
}

foundation::Result<void> NoteService::add_note_with_duration(project::ClipId clip,
                                                             timeline::TickPosition start,
                                                             timeline::TickPosition duration,
                                                             std::uint8_t key) {
  const project::MidiClip* found = nullptr;
  for (const auto& track : snapshot().tracks) {
    const auto item = std::ranges::find(track.midi_clips, clip, &project::MidiClip::id);
    if (item != track.midi_clips.end())
      found = &*item;
  }
  if (!found)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::not_found, "MIDI clip not found: " + clip.to_string()});
  // New notes go inside the pattern, the part of the clip that plays.
  if (start.value < 0 || duration.value <= 0 || duration > found->pattern_length ||
      start.value > found->pattern_length.value - duration.value ||
      key > foundation::max_midi_key) {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::invalid_argument, "Note is outside the MIDI clip"});
  }
  auto id = numbered_id<project::NoteId>(next_note_number());
  if (!id) {
    return foundation::Result<void>::failure(id.error());
  }
  project::MidiNote note{
      id.value(), start, {start.value + duration.value}, key, new_note_velocity_};
  auto applied = model().apply({project::AddMidiNote{clip, note}});
  if (!applied) {
    return foundation::Result<void>::failure(applied.error());
  }
  return foundation::Result<void>::success();
}

foundation::Result<void> NoteService::remove_note(project::ClipId clip, project::NoteId note) {
  auto applied = model().apply({project::RemoveMidiNote{clip, note}});
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

foundation::Result<void> NoteService::set_note_velocities(
    project::ClipId clip, std::span<const std::pair<project::NoteId, std::uint8_t>> velocities) {
  const auto* found = find_midi_clip(snapshot(), clip);
  if (!found)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::not_found, "MIDI clip not found: " + clip.to_string()});
  std::vector<project::Operation> operations;
  for (const auto& [id, velocity] : velocities) {
    const auto note = std::ranges::find(found->notes, id, &project::MidiNote::id);
    if (note == found->notes.end())
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::not_found, "MIDI note not found: " + id.to_string()});
    if (velocity < foundation::min_note_velocity || velocity > foundation::max_note_velocity)
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::invalid_argument,
           "Velocity must be " + std::to_string(foundation::min_note_velocity) + " to " +
               std::to_string(foundation::max_note_velocity)});
    if (note->velocity == velocity)
      continue;
    auto changed = *note;
    changed.velocity = velocity;
    operations.emplace_back(project::SetMidiNote{clip, changed});
  }
  if (operations.empty())
    return foundation::Result<void>::success();
  auto applied = model().apply(std::move(operations));
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

foundation::Result<void> NoteService::move_note(project::ClipId clip, project::NoteId note,
                                                timeline::TickPosition start, std::uint8_t key) {
  const auto invalid =
      foundation::Error{foundation::ErrorCode::invalid_argument, "Note is outside the MIDI clip"};
  if (start.value < 0 || key > foundation::max_midi_key)
    return foundation::Result<void>::failure(invalid);
  for (const auto& track : snapshot().tracks) {
    const auto found = std::ranges::find(track.midi_clips, clip, &project::MidiClip::id);
    if (found == track.midi_clips.end())
      continue;
    const auto source = std::ranges::find(found->notes, note, &project::MidiNote::id);
    if (source == found->notes.end())
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::not_found, "MIDI note not found"});
    if (source->start == start && source->key == key)
      return foundation::Result<void>::success();
    // A note moves within the pattern: one that fits keeps fitting, and one
    // crossing the pattern's end (or kept past it) must start inside.
    const auto duration = source->end.value - source->start.value;
    const auto pattern = found->pattern_length.value;
    const auto latest = source->end.value <= pattern ? pattern - duration : pattern - 1;
    if (start.value > latest)
      return foundation::Result<void>::failure(invalid);
    if (std::ranges::any_of(found->notes, [note, start, key](const auto& other) {
          return other.id != note && other.start == start && other.key == key;
        }))
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::conflict, "MIDI note destination is occupied"});
    auto moved = *source;
    moved.start = start;
    moved.end = {start.value + duration};
    moved.key = key;
    auto applied = model().apply({project::SetMidiNote{clip, moved}});
    return applied ? foundation::Result<void>::success()
                   : foundation::Result<void>::failure(applied.error());
  }
  return foundation::Result<void>::failure(
      {foundation::ErrorCode::not_found, "MIDI clip not found"});
}

foundation::Result<void> NoteService::remove_notes(project::ClipId clip,
                                                   std::span<const project::NoteId> notes) {
  const auto* found = find_midi_clip(snapshot(), clip);
  if (!found)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::not_found, "MIDI clip not found"});
  std::vector<project::Operation> operations;
  for (const auto note : notes) {
    if (std::ranges::find(found->notes, note, &project::MidiNote::id) == found->notes.end())
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::not_found, "MIDI note not found"});
    operations.emplace_back(project::RemoveMidiNote{clip, note});
  }
  if (operations.empty())
    return foundation::Result<void>::success();
  auto applied = model().apply(std::move(operations));
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

foundation::Result<void> NoteService::move_notes(project::ClipId clip,
                                                 std::span<const project::NoteId> notes,
                                                 std::int64_t delta_ticks, int delta_keys) {
  const auto invalid = foundation::Error{foundation::ErrorCode::invalid_argument,
                                         "Notes would move outside the MIDI clip"};
  const auto* found = find_midi_clip(snapshot(), clip);
  if (!found)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::not_found, "MIDI clip not found"});
  if (notes.empty() || (delta_ticks == 0 && delta_keys == 0))
    return foundation::Result<void>::success();
  const auto pattern = found->pattern_length.value;
  std::vector<project::MidiNote> moved;
  for (const auto id : notes) {
    const auto source = std::ranges::find(found->notes, id, &project::MidiNote::id);
    if (source == found->notes.end())
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::not_found, "MIDI note not found"});
    // The same rule as one note: one that fits keeps fitting, and one
    // crossing the pattern's end (or kept past it) must start inside.
    const auto duration = source->end.value - source->start.value;
    const auto start = source->start.value + delta_ticks;
    const auto key = static_cast<int>(source->key) + delta_keys;
    const auto latest = source->end.value <= pattern ? pattern - duration : pattern - 1;
    if (start < 0 || start > latest || key < 0 || key > foundation::max_midi_key)
      return foundation::Result<void>::failure(invalid);
    auto note = *source;
    note.start = {start};
    note.end = {start + duration};
    note.key = static_cast<std::uint8_t>(key);
    moved.push_back(note);
  }
  // Nothing may land on a note that stays where it is.
  for (const auto& note : moved) {
    if (std::ranges::any_of(found->notes, [&](const auto& other) {
          return std::ranges::find(notes, other.id) == notes.end() && other.start == note.start &&
                 other.key == note.key;
        }))
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::conflict, "MIDI note destination is occupied"});
  }
  std::vector<project::Operation> operations;
  for (const auto& note : moved)
    operations.emplace_back(project::SetMidiNote{clip, note});
  auto applied = model().apply(std::move(operations));
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

foundation::Result<void> NoteService::resize_note(project::ClipId clip, project::NoteId note,
                                                  timeline::TickPosition end) {
  using Result = foundation::Result<void>;
  for (const auto& track : snapshot().tracks) {
    const auto found = std::ranges::find(track.midi_clips, clip, &project::MidiClip::id);
    if (found == track.midi_clips.end())
      continue;
    const auto source = std::ranges::find(found->notes, note, &project::MidiNote::id);
    if (source == found->notes.end())
      return Result::failure({foundation::ErrorCode::not_found, "MIDI note not found"});
    if (end <= source->start || end > found->pattern_length) {
      return Result::failure(
          {foundation::ErrorCode::invalid_argument, "Note end is outside the MIDI clip"});
    }
    if (end == source->end)
      return Result::success();
    auto changed = *source;
    changed.end = end;
    auto applied = model().apply({project::SetMidiNote{clip, changed}});
    return applied ? Result::success() : Result::failure(applied.error());
  }
  return Result::failure({foundation::ErrorCode::not_found, "MIDI clip not found"});
}

foundation::Result<std::vector<project::NoteId>>
NoteService::copy_notes_by(project::ClipId clip, std::span<const project::NoteId> notes,
                           std::int64_t delta_ticks, int delta_keys) {
  using Result = foundation::Result<std::vector<project::NoteId>>;
  const auto* found = find_midi_clip(snapshot(), clip);
  if (!found)
    return Result::failure({foundation::ErrorCode::not_found, "MIDI clip not found"});
  if (notes.empty())
    return Result::failure({foundation::ErrorCode::not_found, "No notes to copy"});
  if (delta_ticks == 0 && delta_keys == 0)
    return Result::failure(
        {foundation::ErrorCode::conflict, "A copy may not land on its original"});
  auto ids = fresh_identities(snapshot().revision);
  if (!ids)
    return Result::failure(ids.error());
  const auto pattern = found->pattern_length.value;
  std::vector<project::MidiNote> copies;
  for (const auto id : notes) {
    const auto source = std::ranges::find(found->notes, id, &project::MidiNote::id);
    if (source == found->notes.end())
      return Result::failure({foundation::ErrorCode::not_found, "MIDI note not found"});
    const auto duration = source->end.value - source->start.value;
    const auto start = source->start.value + delta_ticks;
    const auto key = static_cast<int>(source->key) + delta_keys;
    const auto latest = source->end.value <= pattern ? pattern - duration : pattern - 1;
    if (start < 0 || start > latest || key < 0 || key > foundation::max_midi_key)
      return Result::failure(
          {foundation::ErrorCode::invalid_argument, "Copies would land outside the MIDI clip"});
    auto copy = *source;
    copy.id = project::NoteId::from_bits({ids.value().space, ids.value().next_note++}).value();
    copy.start = {start};
    copy.end = {start + duration};
    copy.key = static_cast<std::uint8_t>(key);
    copies.push_back(copy);
  }
  for (const auto& copy : copies) {
    if (std::ranges::any_of(found->notes, [&](const auto& other) {
          return other.start == copy.start && other.key == copy.key;
        }))
      return Result::failure(
          {foundation::ErrorCode::conflict, "A copy would land on an existing note"});
  }
  std::vector<project::Operation> operations;
  std::vector<project::NoteId> created;
  for (const auto& copy : copies) {
    created.push_back(copy.id);
    operations.emplace_back(project::AddMidiNote{clip, copy});
  }
  auto applied = model().apply(std::move(operations));
  return applied ? Result::success(std::move(created)) : Result::failure(applied.error());
}

Result<std::vector<project::MidiNote>>
NoteService::comp_rhythm_notes(project::ClipId clip,
                               std::span<const project::NativeInstrumentParameter> parameters,
                               std::span<const project::MidiNote> held,
                               timeline::TickPosition start, timeline::TickPosition end) const {
  using R = Result<std::vector<project::MidiNote>>;
  const auto& state = snapshot();
  const auto* found = detail::find_midi_clip(state, clip);
  if (found == nullptr)
    return R::failure({ErrorCode::not_found, "MIDI clip not found: " + clip.to_string()});
  // The range may run past the pattern's end: a caller lengthening the clip
  // to hold a whole phrase renders it first.
  if (start.value < 0 || end <= start)
    return R::failure(
        {ErrorCode::invalid_argument, "Apply the rhythm to a range that starts before it ends"});
  auto anchor = detail::midi_anchor_tick(state, *found);
  if (!anchor)
    return R::failure(anchor.error());
  return comp::render_notes(parameters, held, state.tempo, state.sample_rate, anchor.value(), start,
                            end);
}

Result<void>
NoteService::apply_comp_rhythm(project::ClipId clip,
                               std::span<const project::NativeInstrumentParameter> parameters,
                               std::span<const project::MidiNote> held,
                               timeline::TickPosition start, timeline::TickPosition end) {
  const auto* found = detail::find_midi_clip(snapshot(), clip);
  if (found == nullptr)
    return Result<void>::failure(
        {ErrorCode::not_found, "MIDI clip not found: " + clip.to_string()});
  if (end > found->pattern_length)
    return Result<void>::failure(
        {ErrorCode::invalid_argument, "Apply the rhythm to a range inside the clip's pattern"});
  auto rendered = comp_rhythm_notes(clip, parameters, held, start, end);
  if (!rendered)
    return Result<void>::failure(rendered.error());
  auto notes = std::move(rendered).value();
  std::vector<project::Operation> operations;
  std::size_t kept = 0;
  for (const auto& note : found->notes) {
    if (note.start >= start && note.start < end) {
      operations.emplace_back(project::RemoveMidiNote{clip, note.id});
      continue;
    }
    ++kept;
    // A note held into the range stops where Comp starts.
    if (note.start < start && note.end > start) {
      auto cut = note;
      cut.end = start;
      operations.emplace_back(project::SetMidiNote{clip, cut});
    }
  }
  if (kept + notes.size() > project::max_midi_notes_per_clip)
    return Result<void>::failure({ErrorCode::resource_exhausted, "Too many notes for a clip"});
  const auto first_note = next_note_number();
  if (first_note == 0 || first_note > std::numeric_limits<std::uint64_t>::max() - notes.size())
    return Result<void>::failure({ErrorCode::resource_exhausted, "Project ID space exhausted"});
  for (std::size_t index = 0; index < notes.size(); ++index) {
    auto note_id = numbered_id<project::NoteId>(first_note + index);
    if (!note_id)
      return Result<void>::failure(note_id.error());
    notes[index].id = note_id.value();
    operations.emplace_back(project::AddMidiNote{clip, notes[index]});
  }
  if (operations.empty())
    return Result<void>::failure({ErrorCode::invalid_argument, "The rhythm changes nothing there"});
  auto applied = model().apply(std::move(operations));
  return applied ? Result<void>::success() : Result<void>::failure(applied.error());
}

foundation::Result<std::vector<project::NoteId>>
NoteService::apply_note_edit(project::ClipId clip, std::span<const project::MidiNote> changed,
                             std::span<const project::MidiNote> added) {
  using Result = foundation::Result<std::vector<project::NoteId>>;
  const project::MidiClip* found = nullptr;
  for (const auto& track : snapshot().tracks) {
    const auto at = std::ranges::find(track.midi_clips, clip, &project::MidiClip::id);
    if (at != track.midi_clips.end())
      found = &*at;
  }
  if (found == nullptr)
    return Result::failure({foundation::ErrorCode::not_found, "MIDI clip not found"});
  const auto valid = [](const project::MidiNote& note) {
    return note.start.value >= 0 && note.end.value > note.start.value &&
           note.key <= foundation::max_midi_key && note.velocity >= foundation::min_note_velocity &&
           note.velocity <= foundation::max_note_velocity;
  };
  for (const auto& note : changed) {
    if (std::ranges::find(found->notes, note.id, &project::MidiNote::id) == found->notes.end())
      return Result::failure({foundation::ErrorCode::not_found, "MIDI note not found"});
    if (!valid(note))
      return Result::failure({foundation::ErrorCode::invalid_argument, "A note is out of range"});
  }
  for (const auto& note : added) {
    if (!valid(note))
      return Result::failure({foundation::ErrorCode::invalid_argument, "A note is out of range"});
  }
  // The clip as it would be: no two notes on one start and key.
  std::set<std::pair<std::int64_t, std::uint8_t>> taken;
  for (const auto& note : found->notes) {
    const auto replaced = std::ranges::find(changed, note.id, &project::MidiNote::id);
    const auto& placed = replaced != changed.end() ? *replaced : note;
    if (!taken.emplace(placed.start.value, placed.key).second)
      return Result::failure(
          {foundation::ErrorCode::conflict, "Two notes would share a pitch and time"});
  }
  for (const auto& note : added) {
    if (!taken.emplace(note.start.value, note.key).second)
      return Result::failure(
          {foundation::ErrorCode::conflict, "A new note would land on an existing note"});
  }
  std::vector<project::Operation> operations;
  for (const auto& note : changed) {
    const auto current = std::ranges::find(found->notes, note.id, &project::MidiNote::id);
    if (*current != note)
      operations.emplace_back(project::SetMidiNote{clip, note});
  }
  std::vector<project::NoteId> created;
  auto number = next_note_number();
  for (auto note : added) {
    if (number == 0)
      return Result::failure(
          {foundation::ErrorCode::resource_exhausted, "Project ID space exhausted"});
    auto id = project::NoteId::from_bits({0, number++});
    if (!id)
      return Result::failure(id.error());
    note.id = id.value();
    created.push_back(note.id);
    operations.emplace_back(project::AddMidiNote{clip, note});
  }
  if (operations.empty())
    return Result::success(std::move(created));
  auto applied = model().apply(std::move(operations));
  return applied ? Result::success(std::move(created)) : Result::failure(applied.error());
}

foundation::Result<std::optional<project::ClipId>>
NoteService::record_notes(project::TrackId track, std::span<const RecordedNote> notes,
                          std::optional<project::ClipId> take_clip, std::uint64_t gesture) {
  using Result = foundation::Result<std::optional<project::ClipId>>;
  const auto& state = snapshot();
  const auto found = std::ranges::find(state.tracks, track, &project::Track::id);
  if (found == state.tracks.end())
    return Result::failure({ErrorCode::not_found, "Track not found: " + track.to_string()});
  // The bar starts around a tick.
  const auto bar_floor = [&](std::int64_t tick) -> std::optional<std::int64_t> {
    const auto where = state.meter.locate({tick});
    if (!where)
      return std::nullopt;
    const auto at = state.meter.tick_at({where.value().bar, 1, 0});
    return at ? std::optional{at.value().value} : std::nullopt;
  };
  const auto bar_ceiling = [&](std::int64_t tick) -> std::optional<std::int64_t> {
    const auto where = state.meter.locate({std::max<std::int64_t>(0, tick - 1)});
    if (!where)
      return std::nullopt;
    const auto at = state.meter.tick_at({where.value().bar + 1, 1, 0});
    return at ? std::optional{at.value().value} : std::nullopt;
  };

  std::vector<Span> spans;
  for (const auto& clip : found->midi_clips) {
    auto anchor = detail::midi_anchor_tick(state, clip);
    if (!anchor)
      return Result::failure(anchor.error());
    std::int64_t end = 0;
    if (const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement)) {
      end = musical->end.value;
    } else {
      auto tick = state.tempo.sample_to_tick(std::get<project::ElapsedInterval>(clip.placement).end,
                                             state.sample_rate);
      if (!tick)
        return Result::failure(tick.error());
      end = tick.value().value;
    }
    Span span{.id = clip.id,
              .start = anchor.value().value,
              .end = end,
              .pattern = clip.pattern_length.value,
              .loop = clip.loop,
              .take = take_clip && *take_clip == clip.id,
              .created = false,
              .grown_end = end,
              .taken = {},
              .added = {}};
    for (const auto& note : clip.notes)
      span.taken.emplace(note.start.value, note.key);
    spans.push_back(std::move(span));
  }
  const auto by_start = [](const Span& a, const Span& b) { return a.start < b.start; };
  std::ranges::sort(spans, by_start);
  // Where the next clip after `tick` starts (the take's clip may not grow
  // into it).
  const auto next_start = [&](std::int64_t tick, const Span* except) {
    std::int64_t limit = std::numeric_limits<std::int64_t>::max();
    for (const auto& span : spans)
      if (&span != except && span.start > tick)
        limit = std::min(limit, span.start);
    return limit;
  };
  const auto take_span = [&]() -> Span* {
    const auto at = std::ranges::find_if(spans, [](const Span& s) { return s.take; });
    return at != spans.end() ? &*at : nullptr;
  };

  std::vector<RecordedNote> ordered(notes.begin(), notes.end());
  std::ranges::sort(ordered, [](const RecordedNote& a, const RecordedNote& b) {
    return a.start.value < b.start.value;
  });
  for (const auto& note : ordered) {
    if (note.end.value <= note.start.value || note.key > foundation::max_midi_key ||
        note.velocity < foundation::min_note_velocity ||
        note.velocity > foundation::max_note_velocity)
      continue;
    const auto s = note.start.value;
    const auto e = note.end.value;
    auto* span = [&]() -> Span* {
      for (auto& candidate : spans)
        if (s >= candidate.start && s < candidate.grown_end)
          return &candidate;
      return nullptr;
    }();
    if (span == nullptr) {
      auto* take = take_span();
      if (take != nullptr && s >= take->grown_end) {
        // The take's clip grows to the bar after the note, short of the next clip.
        const auto limit = next_start(take->start, take);
        if (limit <= s)
          continue;
        const auto ceiling = bar_ceiling(e);
        if (!ceiling)
          continue;
        take->grown_end = std::min(*ceiling, limit);
        span = take;
      } else if (take == nullptr) {
        const auto floor = bar_floor(s);
        const auto ceiling = bar_ceiling(e);
        if (!floor || !ceiling)
          continue;
        // Never over the end of a clip before it.
        std::int64_t start = *floor;
        for (const auto& other : spans)
          if (other.start <= s && other.grown_end > start)
            start = other.grown_end;
        const auto end = std::min(std::max(*ceiling, start + 1), next_start(s, nullptr));
        if (end <= s)
          continue;
        const auto number = next_clip_number();
        auto id = project::ClipId::from_bits({0, number});
        if (!id)
          return Result::failure(id.error());
        Span made{.id = id.value(),
                  .start = start,
                  .end = end,
                  .pattern = end - start,
                  .loop = false,
                  .take = true,
                  .created = true,
                  .grown_end = end,
                  .taken = {},
                  .added = {}};
        spans.push_back(std::move(made));
        std::ranges::sort(spans, by_start);
        span = take_span();
      } else {
        continue;
      }
    }
    // Clip-local ticks: a looping clip folds the note into its pattern.
    if (span->take && e > span->grown_end) {
      const auto ceiling = bar_ceiling(e);
      if (ceiling)
        span->grown_end = std::min(*ceiling, next_start(span->start, span));
    }
    const auto pattern = span->take ? span->grown_end - span->start : span->pattern;
    if (pattern <= 0)
      continue;
    auto local = s - span->start;
    if (span->loop)
      local %= pattern;
    if (local >= pattern)
      continue;
    const auto local_end = std::min(local + (e - s), pattern);
    if (local_end <= local || !span->taken.emplace(local, note.key).second)
      continue;
    span->added.push_back({local, local_end, note.key, note.velocity});
  }

  std::vector<project::Operation> operations;
  auto number = next_note_number();
  const auto numbered = [&](const LocalNote& note) -> foundation::Result<project::MidiNote> {
    if (number == 0)
      return foundation::Result<project::MidiNote>::failure(
          {ErrorCode::resource_exhausted, "Project ID space exhausted"});
    auto id = project::NoteId::from_bits({0, number++});
    if (!id)
      return foundation::Result<project::MidiNote>::failure(id.error());
    return foundation::Result<project::MidiNote>::success(
        {id.value(), {note.start}, {note.end}, note.key, note.velocity});
  };
  std::optional<project::ClipId> take_id;
  for (auto& span : spans) {
    if (span.take)
      take_id = span.id;
    if (span.created) {
      std::vector<project::MidiNote> clip_notes;
      for (const auto& note : span.added) {
        auto made = numbered(note);
        if (!made)
          return Result::failure(made.error());
        clip_notes.push_back(made.value());
      }
      const timeline::TickPosition start{span.start};
      const timeline::TickPosition end{span.grown_end};
      operations.emplace_back(
          project::AddMidiClip{track, project::MidiClip{span.id,
                                                        project::MusicalInterval{start, end},
                                                        {end.value - start.value},
                                                        false,
                                                        std::move(clip_notes),
                                                        1}});
      continue;
    }
    if (span.take && span.grown_end != span.end) {
      const auto& clip = *detail::find_midi_clip(state, span.id);
      project::Placement placement = clip.placement;
      if (auto* musical = std::get_if<project::MusicalInterval>(&placement)) {
        musical->end = {span.grown_end};
      } else {
        auto end = state.tempo.tick_to_sample({span.grown_end}, state.sample_rate);
        if (!end)
          return Result::failure(end.error());
        std::get<project::ElapsedInterval>(placement).end = end.value();
      }
      operations.emplace_back(project::SetMidiClipPlacement{span.id, placement});
      operations.emplace_back(
          project::SetMidiPattern{span.id, {span.grown_end - span.start}, clip.loop});
    }
    for (const auto& note : span.added) {
      auto made = numbered(note);
      if (!made)
        return Result::failure(made.error());
      operations.emplace_back(project::AddMidiNote{span.id, made.value()});
    }
  }
  if (operations.empty())
    return Result::success(take_id);
  auto applied =
      model().apply(std::move(operations), gesture == 0 ? std::nullopt : std::optional{gesture});
  if (!applied)
    return Result::failure(applied.error());
  return Result::success(take_id);
}

} // namespace oscillate::application

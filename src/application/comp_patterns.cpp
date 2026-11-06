#include "oscillate/application/comp_patterns.hpp"
#include "oscillate/foundation/midi.hpp"

#include "oscillate/engine/midi_events.hpp"
#include "oscillate/native_dsp/note_effects/note_effect.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <utility>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

namespace oscillate::application::comp {
namespace {

using foundation::ErrorCode;
using foundation::IdBits;
namespace fx = native_dsp::note_effects;

constexpr std::array<CompPattern, 10> pattern_table{{
    {"Anjuna offbeats", "Offbeat 8ths with a 16th tuck-in, the Anjunadeep pluck"},
    {"Driving pluck", "Progressive trance 16ths; the gaps make it"},
    {"Rolling pluck", "Plucks that roll over the bar line"},
    {"Three-three-two", "1, the and of 2, 4: the lilt of melodic house piano"},
    {"Dotted pulse", "Every dotted 8th against the kick, melodic techno"},
    {"Techno stabs", "Off the beat on the and of 2 and 4, sometimes early"},
    {"Trance gate", "A 16th gate over the held chord, short-short-long"},
    {"Pads & swells", "Held a bar, slow in"},
    {"Anticipated", "Held, struck again just before the next beat (Sphinx)"},
    {"Piano eighths", "Repeated 8ths, the piano house pulse (Silhouettes, Epikur)"},
}};

// The parameters a pattern sets; the rest are the musician's.
[[nodiscard]] bool pattern_owned(std::string_view id) {
  constexpr std::array<std::string_view, 4> lanes{"comp.low.", "comp.mid.", "comp.top.",
                                                  "comp.length."};
  constexpr std::array<std::string_view, 4> settings{"comp.top_length", "comp.push_chance",
                                                     "comp.push_amount", "comp.groove"};
  return std::ranges::any_of(lanes, [&](std::string_view lane) { return id.starts_with(lane); }) ||
         std::ranges::find(settings, id) != settings.end();
}

// Blocks of the offline render: any size gives the same notes.
constexpr std::uint32_t render_block = 1'024;
constexpr IdBits render_clip{0x434f'4d50, 1};       // "COMP": what Comp plays.
constexpr IdBits render_input_clip{0x434f'4d50, 2}; // The held notes it plays over.
// The offline render is one transport run.
constexpr std::uint64_t render_generation = 1;

} // namespace

std::span<const CompPattern> patterns() noexcept { return pattern_table; }

foundation::Result<std::vector<project::NativeInstrumentParameter>>
pattern_parameters(std::size_t index, std::span<const project::NativeInstrumentParameter> current) {
  using R = foundation::Result<std::vector<project::NativeInstrumentParameter>>;
  const auto* engine = fx::find_engine(engine_id);
  if (engine == nullptr)
    return R::failure({ErrorCode::unavailable, "Comp is not available in this version"});
  const auto& descriptor = *engine->descriptor;
  if (index >= descriptor.presets.size() || descriptor.presets[index].name != pattern_table[index].name)
    return R::failure({ErrorCode::not_found, "No such Comp pattern"});
  auto resolved = fx::resolve_note_effect(engine_id, descriptor.version, current);
  if (!resolved)
    return R::failure(resolved.error());
  auto pattern = native_dsp::synth::preset_values(descriptor, descriptor.presets[index]);
  if (!pattern)
    return R::failure(pattern.error());
  auto values = std::move(resolved).value().values;
  for (std::size_t parameter = 0; parameter < descriptor.parameters.size(); ++parameter) {
    if (pattern_owned(descriptor.parameters[parameter].id))
      values[parameter] = pattern.value()[parameter];
  }
  return R::success(fx::sparse_note_effect_parameters(*engine, values));
}

foundation::Result<std::vector<project::MidiNote>>
render_notes(std::span<const project::NativeInstrumentParameter> parameters,
             std::span<const project::MidiNote> held, const timeline::TempoMap& tempo,
             std::uint32_t sample_rate, timeline::TickPosition clip_start,
             timeline::TickPosition start, timeline::TickPosition end) {
  using R = foundation::Result<std::vector<project::MidiNote>>;
  if (start.value < 0 || end <= start || clip_start.value < 0 ||
      clip_start.value > std::numeric_limits<std::int64_t>::max() / 2 - end.value)
    return R::failure({ErrorCode::invalid_argument, "Apply over a range that starts before it ends"});
  if (held.size() > project::max_midi_notes_per_clip)
    return R::failure({ErrorCode::resource_exhausted, "Too many notes to play Comp over"});
  const auto* engine = fx::find_engine(engine_id);
  if (engine == nullptr)
    return R::failure({ErrorCode::unavailable, "Comp is not available in this version"});
  auto resolved = fx::resolve_note_effect(engine_id, engine->descriptor->version, parameters);
  if (!resolved)
    return R::failure(resolved.error());
  const auto& values = resolved.value().values;

  // The held notes as playback schedules a clip's notes: song samples, in
  // the order instruments take them.
  auto input_clip = project::ClipId::from_bits(render_input_clip);
  auto input_track = project::TrackId::from_bits(render_input_clip);
  if (!input_clip || !input_track)
    return R::failure({ErrorCode::internal, "Comp's input ids are invalid"});
  std::vector<engine::ScheduledMidiEvent> scheduled;
  scheduled.reserve(2 * held.size());
  // Rendering starts where the first note still held at `start` begins, so
  // Comp holds what it would hold had playback run from there.
  auto first_tick = start.value;
  for (std::size_t index = 0; index < held.size(); ++index) {
    const auto& note = held[index];
    if (note.start.value < 0 || note.end <= note.start || !foundation::valid_midi_key(note.key) ||
        !foundation::valid_note_velocity(note.velocity))
      return R::failure({ErrorCode::invalid_argument, "A held note is not a valid note"});
    auto note_id = project::NoteId::from_bits({0, index + 1});
    if (!note_id)
      return R::failure(note_id.error());
    if (note.start < end && note.end > start)
      first_tick = std::min(first_tick, note.start.value);
    const auto on = tempo.tick_to_sample({clip_start.value + note.start.value}, sample_rate);
    const auto off = tempo.tick_to_sample({clip_start.value + note.end.value}, sample_rate);
    if (!on || !off)
      return R::failure({ErrorCode::invalid_argument, "A held note is outside the song"});
    if (off.value() <= on.value())
      continue; // Shorter than a sample: playback never hears it either.
    scheduled.push_back({on.value(), input_clip.value(), note_id.value(),
                         engine::MidiEventKind::note_on, note.key, note.velocity, false});
    scheduled.push_back({off.value(), input_clip.value(), note_id.value(),
                         engine::MidiEventKind::note_off, note.key, 0, false});
  }
  std::ranges::sort(scheduled, [](const engine::ScheduledMidiEvent& a,
                                  const engine::ScheduledMidiEvent& b) {
    return std::tie(a.sample.value, a.kind, a.note) < std::tie(b.sample.value, b.kind, b.note);
  });
  const engine::InstrumentEvents events(input_track.value(), std::move(scheduled));

  const auto tail_ticks = static_cast<std::int64_t>(
      std::ceil(engine->tail_beats(values) * static_cast<double>(timeline::ticks_per_quarter)));
  const auto first = tempo.tick_to_sample({clip_start.value + first_tick}, sample_rate);
  const auto last = tempo.tick_to_sample({clip_start.value + end.value + tail_ticks}, sample_rate);
  if (!first || !last)
    return R::failure({ErrorCode::invalid_argument, "The range is outside the song"});
  auto kernel = engine->create_kernel(render_clip);
  if (!kernel)
    return R::failure({ErrorCode::resource_exhausted, "Cannot prepare Comp"});
  engine::HeldNotes input_notes;
  std::vector<engine::BlockMidiEvent> input(engine::HeldNotes::block_capacity);
  std::vector<fx::AheadEvent> ahead(fx::ahead_event_capacity);
  std::vector<engine::BlockMidiEvent> output(fx::block_event_capacity);
  // Note-ons waiting for their note-off, by the kernel's note id: (start, key, velocity).
  struct Started {
    std::int64_t start = 0;
    std::uint8_t key = 0;
    std::uint8_t velocity = 0;
  };
  std::map<std::tuple<std::uint64_t, std::uint64_t>, Started> sounding;
  struct Made {
    std::int64_t start = 0;
    std::int64_t end = 0;
    std::uint8_t key = 0;
    std::uint8_t velocity = 0;
  };
  std::vector<Made> made;
  const auto first_frame = static_cast<std::uint64_t>(first.value().value);
  const auto last_frame = static_cast<std::uint64_t>(last.value().value);
  for (auto frame = first_frame; frame <= last_frame; frame += render_block) {
    const fx::NoteClock clock(tempo, sample_rate, frame, render_block);
    if (!clock.valid())
      return R::failure({ErrorCode::invalid_argument, "The tempo map cannot place the range"});
    const auto copied = input_notes.copy_block(events, {static_cast<std::int64_t>(frame)},
                                               render_block, render_generation, input);
    if (copied.status != engine::BlockMidiStatus::ready)
      return R::failure({ErrorCode::resource_exhausted, "Too many notes at once for Comp"});
    const auto seen = fx::copy_ahead(events.events(), clock, ahead);
    const fx::NoteParameters block_parameters(values, {}, std::span(ahead).first(seen));
    const auto count = kernel->process(std::span(input).first(copied.count), output, clock,
                                       block_parameters);
    for (std::size_t index = 0; index < count; ++index) {
      const auto& event = output[index];
      if (event.clip_id_bits != render_clip)
        continue; // A held note's own note-off passing through.
      const auto tick = clock.tick_at(event.offset) - clip_start.value;
      const auto id = std::tuple{event.note_id_bits.high, event.note_id_bits.low};
      if (event.kind == engine::MidiEventKind::note_on) {
        sounding[id] = {tick, event.key, event.velocity};
      } else if (auto found = sounding.find(id); found != sounding.end()) {
        const auto& note = found->second;
        // Only notes starting in the range are Comp's; one sounding at
        // `end` ends there.
        if (note.start >= start.value && note.start < end.value) {
          const auto stop = std::min(tick, end.value);
          if (stop > note.start)
            made.push_back({note.start, stop, note.key, note.velocity});
        }
        sounding.erase(found);
      }
    }
  }
  for (const auto& [id, note] : sounding) {
    (void)id;
    if (note.start >= start.value && note.start < end.value)
      made.push_back({note.start, end.value, note.key, note.velocity});
  }
  std::ranges::sort(made, [](const Made& a, const Made& b) {
    return std::tie(a.start, a.key, a.end) < std::tie(b.start, b.key, b.end);
  });
  if (made.size() > project::max_midi_notes_per_clip)
    return R::failure({ErrorCode::resource_exhausted, "Too many notes for one clip: apply less"});
  std::vector<project::MidiNote> notes;
  notes.reserve(made.size());
  for (std::size_t index = 0; index < made.size(); ++index) {
    auto id = project::NoteId::from_bits({0, index + 1});
    if (!id)
      return R::failure(id.error());
    const auto& note = made[index];
    notes.push_back({id.value(), {note.start}, {note.end}, note.key, note.velocity});
  }
  return R::success(std::move(notes));
}

} // namespace oscillate::application::comp
// clang-format on

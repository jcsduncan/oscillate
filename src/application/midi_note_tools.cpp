#include "oscillate/application/midi_note_tools.hpp"
#include "oscillate/foundation/midi.hpp"

#include "oscillate/application/groove_capture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <utility>

namespace oscillate::application {
namespace {

using foundation::ErrorCode;
using NoteResult = foundation::Result<std::vector<project::MidiNote>>;

constexpr std::array<int, 7> major_steps{0, 2, 4, 5, 7, 9, 11};
constexpr std::array<int, 7> minor_steps{0, 2, 3, 5, 7, 8, 10};

[[nodiscard]] bool in_scope(std::span<const project::NoteId> scope, project::NoteId id) {
  return std::ranges::find(scope, id) != scope.end();
}

[[nodiscard]] std::int64_t floor_div(std::int64_t value, std::int64_t divisor) noexcept {
  const auto quotient = value / divisor;
  return (value % divisor != 0 && ((value < 0) != (divisor < 0))) ? quotient - 1 : quotient;
}

[[nodiscard]] std::int64_t positive_mod(std::int64_t value, std::int64_t divisor) noexcept {
  const auto remainder = value % divisor;
  return remainder < 0 ? remainder + divisor : remainder;
}

// splitmix64, as the velocity tools use: small, well mixed, reproducible.
struct Stream {
  std::uint64_t state;
  [[nodiscard]] std::uint64_t next() noexcept {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  [[nodiscard]] double uniform() noexcept {
    return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0);
  }
};

// The latest start that keeps a note where quantise and move keep it: one
// inside the pattern stays inside; one crossing its end may start up to it.
[[nodiscard]] std::int64_t latest_start(const project::MidiNote& note,
                                        std::int64_t pattern) noexcept {
  const auto duration = note.end.value - note.start.value;
  return note.end.value <= pattern ? pattern - duration : pattern - 1;
}

// Moved notes must not share a start and key with each other or with notes
// that stay.
// A clip's notes and the ones an edit changes.
struct NoteEdit {
  std::span<const project::MidiNote> notes;
  std::span<const project::MidiNote> changed;
};
[[nodiscard]] bool collides(NoteEdit edit) {
  const auto notes = edit.notes;
  const auto changed = edit.changed;
  std::set<std::pair<std::int64_t, std::uint8_t>> taken;
  for (const auto& note : notes) {
    const auto moved = std::ranges::find(changed, note.id, &project::MidiNote::id);
    const auto& placed = moved != changed.end() ? *moved : note;
    if (!taken.emplace(placed.start.value, placed.key).second)
      return true;
  }
  return false;
}

[[nodiscard]] project::MidiNote moved_to(const project::MidiNote& note, std::int64_t start) {
  auto changed = note;
  const auto duration = note.end.value - note.start.value;
  changed.start = {start};
  changed.end = {start + duration};
  return changed;
}

[[nodiscard]] std::uint8_t clamp_velocity(double value) noexcept {
  if (!std::isfinite(value))
    return 100;
  return static_cast<std::uint8_t>(std::clamp(std::lround(value), 1L, 127L));
}

} // namespace

std::array<bool, foundation::semitones_per_octave>
scale_pitch_classes(project::MusicalKey key) noexcept {
  std::array<bool, foundation::semitones_per_octave> classes{};
  const auto& steps = key.mode == project::KeyMode::minor ? minor_steps : major_steps;
  for (const auto step : steps)
    classes[static_cast<std::size_t>((key.tonic % foundation::semitones_per_octave + step) %
                                     foundation::semitones_per_octave)] = true;
  return classes;
}

std::uint8_t snap_to_scale(int key, project::MusicalKey scale) noexcept {
  const auto classes = scale_pitch_classes(scale);
  const auto in = [&](int candidate) {
    return candidate >= 0 && candidate <= foundation::max_midi_key &&
           classes[static_cast<std::size_t>(candidate % foundation::semitones_per_octave)];
  };
  key = std::clamp(key, 0, foundation::max_midi_key);
  for (int distance = 0; distance < foundation::semitones_per_octave; ++distance) {
    if (in(key + distance))
      return static_cast<std::uint8_t>(key + distance);
    if (in(key - distance))
      return static_cast<std::uint8_t>(key - distance);
  }
  return static_cast<std::uint8_t>(key);
}

namespace {

// The index of an in-scale key among all in-scale keys from MIDI 0 up.
[[nodiscard]] int scale_index(int key, project::MusicalKey scale) noexcept {
  const auto classes = scale_pitch_classes(scale);
  int index = 0;
  for (int candidate = 0; candidate < key; ++candidate)
    index +=
        classes[static_cast<std::size_t>(candidate % foundation::semitones_per_octave)] ? 1 : 0;
  return index;
}

} // namespace

std::uint8_t transpose_in_scale(int key, project::MusicalKey scale, int degrees) noexcept {
  const auto classes = scale_pitch_classes(scale);
  int at = snap_to_scale(key, scale);
  const int direction = degrees < 0 ? -1 : 1;
  for (int moved = 0; moved != degrees;) {
    int next = at + direction;
    while (next >= 0 && next <= foundation::max_midi_key &&
           !classes[static_cast<std::size_t>(next % foundation::semitones_per_octave)])
      next += direction;
    if (next < 0 || next > foundation::max_midi_key)
      break;
    at = next;
    moved += direction;
  }
  return static_cast<std::uint8_t>(at);
}

int scale_degrees_between(int from, int to, project::MusicalKey scale) noexcept {
  return scale_index(snap_to_scale(to, scale), scale) -
         scale_index(snap_to_scale(from, scale), scale);
}

std::vector<std::uint8_t> chord_keys(int root, ChordShape shape, int inversion,
                                     std::optional<project::MusicalKey> key) {
  std::vector<int> tones;
  if (key) {
    // Scale degrees above the snapped root.
    const auto& steps = key->mode == project::KeyMode::minor ? minor_steps : major_steps;
    const int base = snap_to_scale(root, *key);
    const int offset = foundation::pitch_class(base - key->tonic);
    const auto degree0 = static_cast<int>(std::ranges::find(steps, offset) - steps.begin());
    const int octave_base = base - offset;
    const auto degree_key = [&](int degree) {
      const int index = degree0 + degree;
      return octave_base + foundation::semitones_per_octave * (index / 7) +
             steps[static_cast<std::size_t>(index % 7)];
    };
    std::vector<int> degrees;
    switch (shape) {
    case ChordShape::triad:
      degrees = {0, 2, 4};
      break;
    case ChordShape::seventh:
      degrees = {0, 2, 4, 6};
      break;
    case ChordShape::sus2:
      degrees = {0, 1, 4};
      break;
    case ChordShape::sus4:
      degrees = {0, 3, 4};
      break;
    case ChordShape::add9:
      degrees = {0, 2, 4, 8};
      break;
    }
    for (const auto degree : degrees)
      tones.push_back(degree_key(degree));
  } else {
    std::vector<int> intervals;
    switch (shape) {
    case ChordShape::triad:
      intervals = {0, 4, 7};
      break;
    case ChordShape::seventh:
      intervals = {0, 4, 7, 11};
      break;
    case ChordShape::sus2:
      intervals = {0, 2, 7};
      break;
    case ChordShape::sus4:
      intervals = {0, 5, 7};
      break;
    case ChordShape::add9:
      intervals = {0, 4, 7, 14};
      break;
    }
    for (const auto interval : intervals)
      tones.push_back(root + interval);
  }
  const auto lifted = std::clamp(inversion, 0, static_cast<int>(tones.size()) - 1);
  for (int index = 0; index < lifted; ++index)
    tones[static_cast<std::size_t>(index)] += foundation::semitones_per_octave;
  std::ranges::sort(tones);
  std::vector<std::uint8_t> keys;
  for (const auto tone : tones) {
    if (tone >= 0 && tone <= foundation::max_midi_key)
      keys.push_back(static_cast<std::uint8_t>(tone));
  }
  return keys;
}

std::vector<project::MidiNote> strum_notes(std::span<const project::MidiNote> notes,
                                           std::span<const project::NoteId> scope,
                                           std::int64_t step_ticks, bool upward,
                                           std::int64_t pattern_length) {
  std::map<std::int64_t, std::vector<const project::MidiNote*>> chords;
  for (const auto& note : notes) {
    if (in_scope(scope, note.id) && note.start.value < pattern_length)
      chords[note.start.value].push_back(&note);
  }
  std::vector<project::MidiNote> changed;
  if (step_ticks <= 0)
    return changed;
  for (auto& [start, tones] : chords) {
    if (tones.size() < 2)
      continue;
    std::ranges::sort(tones, [upward](const auto* a, const auto* b) {
      return upward ? a->key < b->key : a->key > b->key;
    });
    for (std::size_t index = 1; index < tones.size(); ++index) {
      const auto& tone = *tones[index];
      const auto last = std::min(tone.end.value, pattern_length) - 1;
      // Saturate: a strum offset past `last` lands on it, without overflow.
      const auto room = last - start;
      const auto index_ticks = static_cast<std::int64_t>(index);
      const auto offset = step_ticks > 0 && index_ticks > room / step_ticks
                              ? room
                              : std::min(room, step_ticks * index_ticks);
      const auto moved = start + offset;
      if (moved <= tone.start.value)
        continue;
      auto strummed = tone;
      strummed.start = {moved};
      changed.push_back(strummed);
    }
  }
  return changed;
}

std::vector<project::MidiNote> humanize_timing(std::span<const project::MidiNote> notes,
                                               std::span<const project::NoteId> scope,
                                               std::int64_t max_ticks, std::uint64_t seed,
                                               std::int64_t pattern_length) {
  std::vector<project::MidiNote> changed;
  if (max_ticks <= 0)
    return changed;
  std::set<std::pair<std::int64_t, std::uint8_t>> taken;
  for (const auto& note : notes)
    taken.emplace(note.start.value, note.key);
  Stream stream{seed};
  for (const auto& note : notes) {
    if (!in_scope(scope, note.id) || note.start.value >= pattern_length)
      continue;
    // Triangular: the sum of two uniforms leans toward small moves.
    const double offset =
        (stream.uniform() + stream.uniform() - 1.0) * static_cast<double>(max_ticks);
    const auto start =
        std::clamp(note.start.value + static_cast<std::int64_t>(std::llround(offset)),
                   std::int64_t{0}, latest_start(note, pattern_length));
    if (start == note.start.value || taken.contains({start, note.key}))
      continue;
    taken.erase({note.start.value, note.key});
    taken.emplace(start, note.key);
    changed.push_back(moved_to(note, start));
  }
  return changed;
}

NoteResult quantize_notes(std::span<const project::MidiNote> notes,
                          std::span<const project::NoteId> scope, const QuantizeSettings& settings,
                          ClipPattern pattern) {
  const std::int64_t clip_start = pattern.clip_start;
  const std::int64_t pattern_length = pattern.pattern_length;
  if (settings.grid_ticks <= 0 || !std::isfinite(settings.strength) ||
      !std::isfinite(settings.swing))
    return NoteResult::failure({ErrorCode::invalid_argument, "Choose a quantise grid"});
  const auto grid = settings.grid_ticks;
  const double strength = std::clamp(settings.strength, 0.0, 1.0);
  // Every second line of the song's grid sits late by the swing.
  const auto swing_offset = static_cast<std::int64_t>(std::llround(
      (std::clamp(settings.swing, 0.5, 0.75) - 0.5) * 2.0 * static_cast<double>(grid)));
  const auto line = [&](std::int64_t index) {
    return index * grid + (positive_mod(index, 2) == 1 ? swing_offset : 0);
  };
  const auto nearest = [&](std::int64_t song_tick) {
    const auto index = floor_div(song_tick, grid);
    std::int64_t best = line(index);
    // A tie goes to the later line, as the grid quantise always has.
    for (const auto candidate : {line(index - 1), line(index + 1), line(index + 2)}) {
      const auto distance = std::llabs(candidate - song_tick);
      const auto best_distance = std::llabs(best - song_tick);
      if (distance < best_distance || (distance == best_distance && candidate > best))
        best = candidate;
    }
    return best;
  };
  std::vector<project::MidiNote> changed;
  for (const auto& note : notes) {
    if (!in_scope(scope, note.id) || note.start.value >= pattern_length)
      continue;
    const auto song = clip_start + note.start.value;
    const auto target = nearest(song) - clip_start;
    auto start = note.start.value + static_cast<std::int64_t>(std::llround(
                                        strength * static_cast<double>(target - note.start.value)));
    start = std::clamp(start, std::int64_t{0}, latest_start(note, pattern_length));
    auto result = moved_to(note, start);
    if (settings.ends) {
      const auto end_target = nearest(clip_start + result.end.value) - clip_start;
      auto end = result.end.value +
                 static_cast<std::int64_t>(
                     std::llround(strength * static_cast<double>(end_target - result.end.value)));
      end = std::max(end, start + std::max<std::int64_t>(1, grid / 4));
      if (note.end.value <= pattern_length)
        end = std::min(end, pattern_length);
      result.end = {end};
    }
    if (result.start != note.start || result.end != note.end)
      changed.push_back(result);
  }
  if (collides({.notes = notes, .changed = changed}))
    return NoteResult::failure(
        {ErrorCode::conflict, "Quantise would place two notes at the same pitch and time"});
  return NoteResult::success(std::move(changed));
}

NoteResult apply_groove(std::span<const project::MidiNote> notes,
                        std::span<const project::NoteId> scope,
                        const persistence::GrooveTemplate& groove, const GrooveAmounts& amounts,
                        ClipPattern pattern) {
  const std::int64_t clip_start = pattern.clip_start;
  const std::int64_t pattern_length = pattern.pattern_length;
  if (auto valid = persistence::validate_groove(groove); !valid)
    return NoteResult::failure(valid.error());
  const auto step = timeline::ticks_per_quarter / groove.steps_per_quarter;
  const auto cycle = static_cast<std::int64_t>(groove.steps.size());
  const double quantize = std::clamp(amounts.quantize, 0.0, 1.0);
  const double timing = std::clamp(amounts.timing, 0.0, 1.0);
  const double accent = std::clamp(amounts.velocity, 0.0, 1.0);
  std::vector<project::MidiNote> changed;
  for (const auto& note : notes) {
    if (!in_scope(scope, note.id) || note.start.value >= pattern_length)
      continue;
    const auto song = clip_start + note.start.value;
    const auto index = floor_div(song + step / 2, step);
    const auto& feel = groove.steps[static_cast<std::size_t>(positive_mod(index, cycle))];
    const double target = static_cast<double>(index * step - song) * quantize +
                          feel.timing * static_cast<double>(step) * timing;
    const auto start =
        std::clamp(note.start.value + static_cast<std::int64_t>(std::llround(target)),
                   std::int64_t{0}, latest_start(note, pattern_length));
    auto result = moved_to(note, start);
    result.velocity =
        clamp_velocity(static_cast<double>(note.velocity) * (1.0 + accent * (feel.velocity - 1.0)));
    if (result.start != note.start || result.velocity != note.velocity)
      changed.push_back(result);
  }
  if (collides({.notes = notes, .changed = changed}))
    return NoteResult::failure(
        {ErrorCode::conflict, "The groove would place two notes at the same pitch and time"});
  return NoteResult::success(std::move(changed));
}

std::vector<persistence::GrooveTemplate> builtin_grooves() {
  std::vector<persistence::GrooveTemplate> grooves;
  // Swing moves every second grid line from half way to `swing` of a pair.
  for (const std::uint32_t steps_per_quarter : {2U, 4U}) {
    for (const int swing : {54, 58, 62, 66, 71, 75}) {
      const double late = (swing / 100.0 - 0.5) * 2.0;
      grooves.push_back({"Swing " + std::string(steps_per_quarter == 2 ? "8ths " : "16ths ") +
                             std::to_string(swing) + "%",
                         steps_per_quarter,
                         {{0.0, 1.0}, {late, 1.0}}});
    }
  }
  // MPC-style 16th swing: swung off-16ths, the beat accented, the "e" and
  // "a" softer.
  for (const int swing : {54, 58, 62, 66, 71}) {
    const double late = (swing / 100.0 - 0.5) * 2.0;
    grooves.push_back({"MPC 16ths " + std::to_string(swing) + "%",
                       4,
                       {{0.0, 1.0}, {late, 0.78}, {0.0, 0.9}, {late, 0.8}}});
  }
  // Laid back: the backbeat and off-8ths drag a little; pushed: they rush.
  grooves.push_back({"Laid Back 16ths",
                     4,
                     {{0.0, 1.0},
                      {0.08, 0.8},
                      {0.06, 0.9},
                      {0.1, 0.8},
                      {0.04, 1.05},
                      {0.1, 0.8},
                      {0.06, 0.9},
                      {0.12, 0.8}}});
  grooves.push_back({"Pushed 16ths", 4, {{0.0, 1.0}, {-0.06, 0.82}, {-0.04, 0.92}, {-0.08, 0.82}}});
  grooves.push_back({"Shuffle 8ths Triplet", 2, {{0.0, 1.0}, {1.0 / 3.0, 0.85}}});
  return grooves;
}

std::int64_t midi_clip_start_tick(const project::MidiClip& clip) noexcept {
  if (const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement))
    return musical->start.value;
  return 0;
}

} // namespace oscillate::application

#include "oscillate/clay_dsp/macros.hpp"
#include "oscillate/foundation/midi.hpp"
#include "oscillate/foundation/split_mix.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace oscillate::clay_dsp {
namespace {

constexpr std::array<int, 7> major_steps{0, 2, 4, 5, 7, 9, 11};
constexpr std::array<int, 7> minor_steps{0, 2, 3, 5, 7, 8, 10};
// Scale pitches are searched this many semitones either side.
constexpr int scale_search = pitch_classes;
// Velocity falls linearly over this many dB under the loudest note.
constexpr double velocity_range_db = 40.0;
constexpr double tiny = 1e-12;

[[nodiscard]] int wrap(int value, int modulus) noexcept {
  return ((value % modulus) + modulus) % modulus;
}

[[nodiscard]] bool selected(Selection selection, std::uint32_t id) {
  return selection.empty() || std::ranges::find(selection, id) != selection.end();
}

[[nodiscard]] std::vector<NoteEdit> copied(std::span<const NoteEdit> edits) {
  return {edits.begin(), edits.end()};
}

[[nodiscard]] double level_db(const Note& note, const NoteEdit& edit) {
  return 20.0 * std::log10(std::max(tiny, note.mean_amplitude())) + edit.gain_db;
}

[[nodiscard]] std::int64_t placed_start(const Note& note, const NoteEdit& edit) {
  return edit.start.value_or(note.start);
}

[[nodiscard]] std::int64_t placed_end(const Note& note, const NoteEdit& edit) {
  return edit.end.value_or(note.end);
}

// Whether a pitch shift stays inside validate()'s bound.
[[nodiscard]] bool shift_in_range(double cents) noexcept {
  return std::isfinite(cents) && std::abs(cents) <= max_edit_cents;
}

// Whether a placement of `note` is one validate() accepts: inside the clip,
// non-empty and stretched no further than max_time_scale.
[[nodiscard]] bool placement_valid(const Note& note, std::int64_t start, std::int64_t end) {
  if (start < 0 || end <= start)
    return false;
  const double scale = static_cast<double>(end - start) /
                       static_cast<double>(std::max<std::int64_t>(1, note.end - note.start));
  return scale <= max_time_scale && scale >= 1.0 / max_time_scale;
}

// The largest magnitude a double holds as an exact integer: grid steps and
// frames beyond it cannot round to a representable frame.
constexpr double exact_integer_limit = 9'007'199'254'740'992.0;

// The scale's pitches (in semitones) around `semitone`, ascending.
[[nodiscard]] std::vector<int> pitches_around(int semitone, const Scale& scale, int reach) {
  std::vector<int> out;
  for (int k = semitone - reach; k <= semitone + reach; ++k) {
    if (scale.contains(wrap(k, pitch_classes)))
      out.push_back(k);
  }
  return out;
}

// splitmix64: a well-mixed hash of the seed, note and channel.
[[nodiscard]] std::uint64_t mix(std::uint64_t x) noexcept {
  return foundation::split_mix(x + foundation::split_mix_increment);
}

[[nodiscard]] double unit(std::uint64_t seed, std::uint32_t note, std::uint64_t channel) noexcept {
  const auto bits = mix(seed ^ mix((static_cast<std::uint64_t>(note) << 8U) | channel));
  constexpr double scale = 1.0 / static_cast<double>(std::numeric_limits<std::uint64_t>::max());
  return 2.0 * static_cast<double>(bits) * scale - 1.0;
}

} // namespace

Scale Scale::chromatic() {
  Scale s;
  s.classes.fill(true);
  return s;
}

Scale Scale::major(int root) {
  Scale s;
  s.root = wrap(root, pitch_classes);
  for (const int step : major_steps)
    s.classes[static_cast<std::size_t>(wrap(s.root + step, pitch_classes))] = true;
  return s;
}

Scale Scale::minor(int root) {
  Scale s;
  s.root = wrap(root, pitch_classes);
  for (const int step : minor_steps)
    s.classes[static_cast<std::size_t>(wrap(s.root + step, pitch_classes))] = true;
  return s;
}

bool Scale::contains(int pitch_class) const noexcept {
  return classes[static_cast<std::size_t>(wrap(pitch_class, pitch_classes))];
}

std::vector<int> Scale::degrees() const {
  std::vector<int> out;
  for (int i = 0; i < pitch_classes; ++i) {
    if (contains(root + i))
      out.push_back(i);
  }
  return out;
}

NoteEdit edit_of(std::span<const NoteEdit> edits, std::uint32_t note) {
  for (const auto& e : edits) {
    if (!e.duplicate && e.note == note)
      return e;
  }
  NoteEdit fresh;
  fresh.note = note;
  return fresh;
}

void put_edit(std::vector<NoteEdit>& edits, const NoteEdit& edit) {
  for (auto& e : edits) {
    if (!e.duplicate && e.note == edit.note) {
      e = edit;
      return;
    }
  }
  edits.push_back(edit);
}

double edited_centre(const Note& note, const NoteEdit& edit) noexcept {
  return note.centre + edit.pitch_cents;
}

double nearest_in_scale(double cents, const Scale& scale) noexcept {
  const auto semitone = static_cast<int>(std::lround(cents / cents_per_semitone));
  double best = cents;
  double distance = std::numeric_limits<double>::infinity();
  for (int k = semitone - scale_search; k <= semitone + scale_search; ++k) {
    if (!scale.contains(wrap(k, pitch_classes)))
      continue;
    const double candidate = k * cents_per_semitone;
    if (std::abs(candidate - cents) < distance) {
      distance = std::abs(candidate - cents);
      best = candidate;
    }
  }
  return best;
}

std::vector<NoteEdit> correct_pitch(const Take& take, std::span<const NoteEdit> edits,
                                    double centre_amount, double drift_amount, const Scale& scale,
                                    Selection selection) {
  auto out = copied(edits);
  for (const auto& note : take.notes) {
    if (!selected(selection, note.id) || note.kind != NoteKind::sinusoidal)
      continue;
    auto e = edit_of(out, note.id);
    const double current = edited_centre(note, e);
    const double shifted = e.pitch_cents + (nearest_in_scale(current, scale) - current) *
                                               std::clamp(centre_amount, 0.0, 1.0);
    if (shift_in_range(shifted))
      e.pitch_cents = shifted;
    e.drift *= 1.0 - std::clamp(drift_amount, 0.0, 1.0);
    put_edit(out, e);
  }
  return out;
}

std::vector<NoteEdit> quantize_time(const Take& take, std::span<const NoteEdit> edits,
                                    const TimeGrid& grid, double amount, Selection selection) {
  auto out = copied(edits);
  if (!(grid.step_frames > 0.0) || !std::isfinite(grid.step_frames) ||
      !std::ranges::all_of(grid.offsets, [](double offset) { return std::isfinite(offset); }))
    return out;
  const auto position = [&](std::int64_t step) {
    double offset = 0.0;
    if (!grid.offsets.empty()) {
      const auto size = static_cast<std::int64_t>(grid.offsets.size());
      offset = grid.offsets[static_cast<std::size_t>(((step % size) + size) % size)];
    }
    return static_cast<double>(grid.origin) +
           (static_cast<double>(step) + offset) * grid.step_frames;
  };
  for (const auto& note : take.notes) {
    if (!selected(selection, note.id))
      continue;
    auto e = edit_of(out, note.id);
    const auto start = placed_start(note, e);
    const auto length = placed_end(note, e) - start;
    const double steps =
        (static_cast<double>(start) - static_cast<double>(grid.origin)) / grid.step_frames;
    if (!(std::abs(steps) < exact_integer_limit))
      continue;
    const auto guess = static_cast<std::int64_t>(std::llround(steps));
    double target = position(guess);
    for (const auto step : {guess - 1, guess + 1}) {
      if (std::abs(position(step) - static_cast<double>(start)) <
          std::abs(target - static_cast<double>(start)))
        target = position(step);
    }
    const double placed = static_cast<double>(start) +
                          (target - static_cast<double>(start)) * std::clamp(amount, 0.0, 1.0);
    if (!(std::abs(placed) < exact_integer_limit))
      continue;
    const auto moved = std::max<std::int64_t>(0, std::llround(placed));
    if (moved == start || !placement_valid(note, moved, moved + length))
      continue;
    e.start = moved;
    e.end = moved + length;
    put_edit(out, e);
  }
  return out;
}

std::vector<NoteEdit> level_amplitude(const Take& take, std::span<const NoteEdit> edits,
                                      double amount, Selection selection) {
  auto out = copied(edits);
  std::vector<double> levels;
  for (const auto& note : take.notes) {
    if (selected(selection, note.id))
      levels.push_back(level_db(note, edit_of(out, note.id)));
  }
  if (levels.empty())
    return out;
  auto sorted = levels;
  std::ranges::sort(sorted);
  const double median = sorted[sorted.size() / 2];
  for (const auto& note : take.notes) {
    if (!selected(selection, note.id))
      continue;
    auto e = edit_of(out, note.id);
    e.gain_db = std::clamp(e.gain_db + (median - level_db(note, e)) * std::clamp(amount, 0.0, 1.0),
                           min_edit_gain_db, max_edit_gain_db);
    put_edit(out, e);
  }
  return out;
}

MidiOut to_midi(const Take& take, std::span<const NoteEdit> edits) {
  MidiOut out;
  struct Voice {
    const Note* note;
    NoteEdit edit;
  };
  std::vector<Voice> voices;
  voices.reserve(take.notes.size());
  for (const auto& note : take.notes)
    voices.push_back({&note, edit_of(edits, note.id)});
  for (const auto& e : edits) {
    if (e.duplicate) {
      if (const auto* note = take.find(e.note))
        voices.push_back({note, e});
    }
  }
  const auto key_of = [](const Voice& v) {
    return static_cast<int>(std::lround(edited_centre(*v.note, v.edit) / cents_per_semitone));
  };
  // Voices that cannot be MIDI notes (a placement validate() refuses, or a
  // key outside the MIDI range) are left out before levels are compared.
  std::erase_if(voices, [&](const Voice& v) {
    return !placement_valid(*v.note, placed_start(*v.note, v.edit), placed_end(*v.note, v.edit)) ||
           !foundation::valid_midi_key(key_of(v));
  });
  double loudest = -std::numeric_limits<double>::infinity();
  for (const auto& v : voices)
    loudest = std::max(loudest, level_db(*v.note, v.edit));
  const auto step = std::max<std::int64_t>(
      1, std::llround(bend_step_seconds * static_cast<double>(take.sample_rate)));
  for (const auto& v : voices) {
    const auto& note = *v.note;
    const auto start = placed_start(note, v.edit);
    const auto end = placed_end(note, v.edit);
    const double centre = edited_centre(note, v.edit);
    const auto voice = out.notes.size();
    const int key = key_of(v);
    const double below = level_db(note, v.edit) - loudest;
    const int velocity =
        std::clamp(static_cast<int>(std::lround(foundation::max_note_velocity *
                                                (1.0 + below / velocity_range_db))),
                   foundation::min_note_velocity, foundation::max_note_velocity);
    out.notes.push_back({start, end, key, velocity});
    if (note.kind != NoteKind::sinusoidal || note.hops() == 0)
      continue;
    const double scale = static_cast<double>(note.end - note.start) /
                         static_cast<double>(std::max<std::int64_t>(1, end - start));
    // Advances only while the next step stays before the end, so a
    // placement near the frame limit cannot overflow.
    for (auto frame = start;; frame += step) {
      const double source =
          static_cast<double>(note.start) + static_cast<double>(frame - start) * scale;
      const double tau = std::clamp((source - static_cast<double>(note.first_hop * take.hop)) /
                                        static_cast<double>(take.hop),
                                    0.0, static_cast<double>(note.hops() - 1));
      const auto k = std::min(static_cast<std::size_t>(std::lround(tau)), note.hops() - 1);
      const double cents =
          centre + note.drift[k] * v.edit.drift + note.modulation[k] * v.edit.modulation;
      const double bend = (cents - key * cents_per_semitone) / cents_per_semitone;
      out.bends.push_back({frame, bend, voice});
      out.bend_range = std::max(out.bend_range, std::ceil(std::abs(bend)));
      if (end - frame <= step)
        break;
    }
  }
  // Sort the voices by start, then point each bend at its voice's new place.
  std::vector<std::size_t> order(out.notes.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::ranges::stable_sort(order, {}, [&](std::size_t index) { return out.notes[index].start; });
  std::vector<std::size_t> place(order.size());
  std::vector<MidiNoteOut> sorted;
  sorted.reserve(order.size());
  for (const auto index : order) {
    place[index] = sorted.size();
    sorted.push_back(out.notes[index]);
  }
  out.notes = std::move(sorted);
  for (auto& bend : out.bends)
    bend.note = place[bend.note];
  std::ranges::stable_sort(out.bends, {}, &BendPoint::frame);
  return out;
}

std::vector<NoteEdit> harmonise(const Take& take, std::span<const NoteEdit> edits, int degrees,
                                const Scale& scale, Selection selection) {
  auto out = copied(edits);
  if (degrees == 0)
    return out;
  const int per_octave = std::max(1, static_cast<int>(scale.degrees().size()));
  const int reach = (std::abs(degrees) / per_octave + 2) * pitch_classes;
  for (const auto& note : take.notes) {
    if (!selected(selection, note.id) || note.kind != NoteKind::sinusoidal)
      continue;
    const auto base = edit_of(edits, note.id);
    const double current = edited_centre(note, base);
    const auto semitone =
        static_cast<int>(std::lround(nearest_in_scale(current, scale) / cents_per_semitone));
    const auto pitches = pitches_around(semitone, scale, reach);
    const auto at = std::ranges::find(pitches, semitone);
    if (at == pitches.end())
      continue;
    const auto index = static_cast<std::ptrdiff_t>(at - pitches.begin()) + degrees;
    if (index < 0 || index >= static_cast<std::ptrdiff_t>(pitches.size()))
      continue;
    auto voice = base;
    voice.duplicate = true;
    voice.pitch_cents += pitches[static_cast<std::size_t>(index)] * cents_per_semitone - current;
    if (shift_in_range(voice.pitch_cents))
      out.push_back(voice);
  }
  return out;
}

std::vector<NoteEdit> fit_to_key(const Take& take, std::span<const NoteEdit> edits,
                                 const Scale& from, const Scale& to, bool follow_quality,
                                 Selection selection) {
  auto out = copied(edits);
  const auto from_degrees = from.degrees();
  const auto to_degrees = to.degrees();
  // Empty scales have no degrees to map; such a fit is a plain transposition.
  const bool map_degrees =
      follow_quality && !from_degrees.empty() && from_degrees.size() == to_degrees.size();
  int transpose = wrap(to.root - from.root, pitch_classes);
  if (transpose > pitch_classes / 2)
    transpose -= pitch_classes;
  for (const auto& note : take.notes) {
    if (!selected(selection, note.id))
      continue;
    auto e = edit_of(out, note.id);
    const auto semitone =
        static_cast<int>(std::lround(edited_centre(note, e) / cents_per_semitone));
    const int relative = wrap(semitone - from.root, pitch_classes);
    int target = relative;
    if (map_degrees) {
      // The nearest degree at or below, keeping any chromatic step above it.
      std::size_t degree = 0;
      for (std::size_t d = 0; d < from_degrees.size(); ++d) {
        if (from_degrees[d] <= relative)
          degree = d;
      }
      target = to_degrees[degree] + (relative - from_degrees[degree]);
    }
    const int shift = transpose + (target - relative);
    if (shift == 0 || !shift_in_range(e.pitch_cents + shift * cents_per_semitone))
      continue;
    e.pitch_cents += shift * cents_per_semitone;
    put_edit(out, e);
  }
  return out;
}

std::vector<NoteEdit> follow_notes(const Take& take, std::span<const NoteEdit> edits,
                                   std::span<const TargetNote> targets, double time_amount,
                                   Selection selection) {
  auto out = copied(edits);
  const double amount = std::clamp(time_amount, 0.0, 1.0);
  for (const auto& note : take.notes) {
    if (!selected(selection, note.id))
      continue;
    auto e = edit_of(out, note.id);
    const auto start = placed_start(note, e);
    const auto end = placed_end(note, e);
    const TargetNote* best = nullptr;
    std::int64_t most = 0;
    for (const auto& t : targets) {
      const auto overlap = std::min(end, t.end) - std::max(start, t.start);
      if (overlap > most) {
        most = overlap;
        best = &t;
      }
    }
    if (best == nullptr)
      continue;
    const double shifted = e.pitch_cents + best->key * cents_per_semitone - edited_centre(note, e);
    if (shift_in_range(shifted))
      e.pitch_cents = shifted;
    if (amount > 0.0) {
      const auto new_start = std::max<std::int64_t>(
          0, std::llround(static_cast<double>(start) +
                          static_cast<double>(best->start - start) * amount));
      const auto new_end =
          std::llround(static_cast<double>(end) + static_cast<double>(best->end - end) * amount);
      if ((new_start != start || new_end != end) && placement_valid(note, new_start, new_end)) {
        e.start = new_start;
        e.end = new_end;
      }
    }
    put_edit(out, e);
  }
  return out;
}

std::vector<NoteEdit> variation(const Take& take, std::span<const NoteEdit> edits,
                                std::uint64_t seed, double amount, Selection selection) {
  auto out = copied(edits);
  const double a = std::clamp(amount, 0.0, 1.0);
  enum Channel : std::uint8_t { pitch, timing, level };
  for (const auto& note : take.notes) {
    if (!selected(selection, note.id))
      continue;
    auto e = edit_of(out, note.id);
    e.pitch_cents = std::clamp(e.pitch_cents + unit(seed, note.id, pitch) * variation_cents * a,
                               -max_edit_cents, max_edit_cents);
    e.gain_db = std::clamp(e.gain_db + unit(seed, note.id, level) * variation_db * a,
                           min_edit_gain_db, max_edit_gain_db);
    const auto offset =
        static_cast<std::int64_t>(std::llround(unit(seed, note.id, timing) * variation_seconds * a *
                                               static_cast<double>(take.sample_rate)));
    if (offset != 0) {
      const auto start = std::max<std::int64_t>(0, placed_start(note, e) + offset);
      e.end = start + (placed_end(note, e) - placed_start(note, e));
      e.start = start;
    }
    put_edit(out, e);
  }
  return out;
}

} // namespace oscillate::clay_dsp

#include "oscillate/application/midi_velocity_edit.hpp"
#include "oscillate/foundation/midi.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace oscillate::application {
namespace {

[[nodiscard]] bool in_scope(std::span<const project::NoteId> scope, project::NoteId id) {
  return std::ranges::find(scope, id) != scope.end();
}

// splitmix64: a small, well-mixed, reproducible stream.
struct Stream {
  std::uint64_t state;
  [[nodiscard]] std::uint64_t next() noexcept {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  // Uniform in [0, 1).
  [[nodiscard]] double uniform() noexcept {
    return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0);
  }
};

} // namespace

std::uint8_t clamp_velocity(double value) noexcept {
  if (!std::isfinite(value)) {
    return 100;
  }
  return static_cast<std::uint8_t>(std::clamp(std::lround(value), 1L, 127L));
}

std::vector<VelocityChange> velocity_line(std::span<const project::MidiNote> notes,
                                          std::span<const project::NoteId> scope,
                                          std::int64_t from_tick, double from_velocity,
                                          std::int64_t to_tick, double to_velocity) {
  if (to_tick < from_tick) {
    std::swap(from_tick, to_tick);
    std::swap(from_velocity, to_velocity);
  }
  std::vector<VelocityChange> changes;
  for (const auto& note : notes) {
    const auto start = note.start.value;
    if (!in_scope(scope, note.id) || start < from_tick || start > to_tick) {
      continue;
    }
    const double t = to_tick == from_tick ? 0.0
                                          : static_cast<double>(start - from_tick) /
                                                static_cast<double>(to_tick - from_tick);
    changes.emplace_back(note.id,
                         clamp_velocity(from_velocity + t * (to_velocity - from_velocity)));
  }
  return changes;
}

std::vector<VelocityChange> humanize_velocities(std::span<const project::MidiNote> notes,
                                                std::span<const project::NoteId> scope,
                                                double strength, std::uint64_t seed) {
  strength = std::clamp(std::isfinite(strength) ? strength : 0.0, 0.0, 1.0);
  Stream stream{seed};
  std::vector<VelocityChange> changes;
  for (const auto& note : notes) {
    if (!in_scope(scope, note.id)) {
      continue;
    }
    // Sum of three uniforms, centred: a bell on [-1.5, 1.5].
    const double bell = stream.uniform() + stream.uniform() + stream.uniform() - 1.5;
    // Accents (loud notes) vary less, so they stay accents.
    const double headroom =
        1.0 - 0.5 * static_cast<double>(note.velocity) / foundation::max_note_velocity;
    const double offset = bell / 1.5 * 16.0 * strength * headroom;
    changes.emplace_back(note.id, clamp_velocity(note.velocity + offset));
  }
  return changes;
}

std::vector<VelocityChange> randomize_velocities(std::span<const project::MidiNote> notes,
                                                 std::span<const project::NoteId> scope,
                                                 int minimum, int maximum, std::uint64_t seed) {
  minimum = std::clamp(minimum, foundation::min_note_velocity, foundation::max_note_velocity);
  maximum = std::clamp(maximum, foundation::min_note_velocity, foundation::max_note_velocity);
  if (maximum < minimum) {
    std::swap(minimum, maximum);
  }
  Stream stream{seed};
  std::vector<VelocityChange> changes;
  for (const auto& note : notes) {
    if (!in_scope(scope, note.id)) {
      continue;
    }
    const double value = minimum + stream.uniform() * static_cast<double>(maximum - minimum + 1);
    changes.emplace_back(note.id,
                         clamp_velocity(std::min(std::floor(value), static_cast<double>(maximum))));
  }
  return changes;
}

} // namespace oscillate::application

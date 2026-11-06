#pragma once

#include <cstdint>

// MIDI value ranges and pitch arithmetic shared by every module that handles notes.
namespace oscillate::foundation {

// A MIDI data byte carries seven bits: keys, velocities and controller values.
inline constexpr int max_midi_value = 127;
inline constexpr int max_midi_key = max_midi_value;
// Velocity zero is a note-off in MIDI, so a sounding note has at least one.
inline constexpr int min_note_velocity = 1;
inline constexpr int max_note_velocity = max_midi_value;
inline constexpr int semitones_per_octave = 12;

[[nodiscard]] constexpr bool valid_midi_key(int key) noexcept {
  return key >= 0 && key <= max_midi_key;
}

[[nodiscard]] constexpr bool valid_note_velocity(int velocity) noexcept {
  return velocity >= min_note_velocity && velocity <= max_note_velocity;
}

// The pitch class 0..11 of any key or semitone offset, negative ones included.
[[nodiscard]] constexpr int pitch_class(int semitones) noexcept {
  return ((semitones % semitones_per_octave) + semitones_per_octave) % semitones_per_octave;
}

} // namespace oscillate::foundation

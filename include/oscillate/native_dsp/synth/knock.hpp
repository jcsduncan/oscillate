#pragma once

#include "oscillate/native_dsp/synth/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Knock (ADR-0068): eight pads of physically modelled percussion on keys
// C1..G1. Each hit is a striker in nonlinear contact with a resonator (a
// cloud of modes from a material, from pitched note to knock on a thing),
// a second resonator the first sets ringing, a body around both, and a
// finish (snap, gate, lift, crush) for modern, tight sounds. Hand varies
// each hit, seeded, so export equals playback. Width places each pad's
// modes in the stereo field and moves its hits; Swing plays the off-beat
// sixteenths late, from the song position. A pad following the key tunes
// its resonator to the project's key (in_key). Note-offs are ignored.
namespace oscillate::native_dsp::synth::knock {

inline constexpr std::uint8_t first_key = 36; // C1
inline constexpr std::size_t pad_count = 8;
// Voices per pad: a new hit takes the quieter, the other rings on.
inline constexpr std::size_t voices_per_pad = 2;

// Telemetry layout (ADR-0037).
namespace telemetry {
inline constexpr std::size_t last_pad = 0;    // Pad of the latest hit, 0..7; -1 before any.
inline constexpr std::size_t hits = 1;        // Hits since reset, counting up (wraps at 2^24).
inline constexpr std::size_t first_level = 2; // Then each pad's output peak this block, 0..1.
inline constexpr std::size_t output_peak_db = first_level + pad_count; // dBFS, >= -120.
// Then where each pad's latest hit landed, -1 (left) .. 1 (right), pan
// and Motion included; 0 before any.
inline constexpr std::size_t first_place = output_peak_db + 1;
inline constexpr std::size_t size = first_place + pad_count;
} // namespace telemetry

// What a mode belongs to, for the panel's ringing-modes view.
enum class ModePart : std::uint8_t { resonator, coupled, body };

// One designed mode of a pad as its next hit at velocity 100 would play it:
// frequency (Hz, before bend), initial level relative to the pad's
// strongest mode (0..1) and T60 (s), global Decay applied.
struct ModeView {
  double hz = 0.0;
  double level = 0.0;
  double t60 = 0.0;
  ModePart part = ModePart::resonator;
};

// Control only; allocates. values are dense, in table order.
[[nodiscard]] std::vector<ModeView> design_pad(std::span<const double> values, std::size_t pad,
                                               double sample_rate);

// A pad's part in a groove, from its sound: low thuds, mid knocks, high
// ticks and shakers (a brush, or a resonator above about 1.8 kHz). The
// pattern generator writes each its own kind of part.
enum class PadRole : std::uint8_t { low, mid, high };
[[nodiscard]] PadRole pad_role(std::span<const double> values, std::size_t pad) noexcept;

// Swing (knock.swing, 0..0.75): how late an off-beat sixteenth plays, in
// quarter notes. 0.5 lands it on the triplet (1/12 of a beat late); 0.75
// on the dotted position (1/8 late).
[[nodiscard]] double swing_delay_beats(double swing) noexcept;

// Key follow's tuning: equal temperament with A4 (MIDI key 69) at 440 Hz.
inline constexpr double tuning_a4_hz = 440.0;
inline constexpr double tuning_a4_key = 69.0;
inline constexpr int semitones_per_octave = 12;
// The frequency of MIDI key `key` (fractional keys between) in that tuning.
[[nodiscard]] double note_hz(double key) noexcept;

// The pitch (Hz) a pad following the key plays for its Pitch `hz`: the
// nearest note of the key's scale (major, or natural minor), equal
// tempered (note_hz); halfway between two goes down. `hz` itself
// without a key.
[[nodiscard]] double in_key(double hz, std::optional<project::MusicalKey> key) noexcept;

[[nodiscard]] const SynthEngine& engine();

} // namespace oscillate::native_dsp::synth::knock

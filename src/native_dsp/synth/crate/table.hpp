#pragma once

#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/crate.hpp"

#include <cstddef>
#include <optional>
#include <string_view>

// Crate v1's parameter table (spec 09 §2.1, §2.2): the globals, then each
// slot's chop controls in chop_controls order. Ids are persisted and
// automated: never rename or reuse one.
namespace oscillate::native_dsp::synth::crate::detail {

namespace global {
inline constexpr std::size_t layout = 0;
inline constexpr std::size_t keys_chop = 1;
inline constexpr std::size_t root = 2;
inline constexpr std::size_t time = 3;
inline constexpr std::size_t era = 4;
inline constexpr std::size_t bits = 5;
inline constexpr std::size_t rate = 6;
inline constexpr std::size_t drive = 7;
inline constexpr std::size_t vinyl = 8;
inline constexpr std::size_t wow = 9;
inline constexpr std::size_t output = 10;
inline constexpr std::size_t swing = 11;
inline constexpr std::size_t velocity = 12;
inline constexpr std::size_t voices = 13;
inline constexpr std::size_t count = 14;
} // namespace global

// Offsets within one slot's block, in chop_controls order.
namespace chop {
inline constexpr std::size_t pitch = 0;
inline constexpr std::size_t fine = 1;
inline constexpr std::size_t gain = 2;
inline constexpr std::size_t pan = 3;
inline constexpr std::size_t start = 4;
inline constexpr std::size_t length = 5;
inline constexpr std::size_t attack = 6;
inline constexpr std::size_t hold = 7;
inline constexpr std::size_t decay = 8;
inline constexpr std::size_t sustain = 9;
inline constexpr std::size_t release = 10;
inline constexpr std::size_t cutoff = 11;
inline constexpr std::size_t resonance = 12;
inline constexpr std::size_t reverse = 13;
inline constexpr std::size_t mode = 14;
inline constexpr std::size_t filter = 15;
inline constexpr std::size_t choke = 16;
inline constexpr std::size_t count = chop_controls.size();
} // namespace chop

[[nodiscard]] constexpr std::size_t chop_index(std::size_t slot, std::size_t control) noexcept {
  return global::count + slot * chop::count + control;
}
inline constexpr std::size_t parameter_count = global::count + max_chops * chop::count;

// Ranges the kernel and the tail bound share with the table.
inline constexpr double max_voices = 32.0;
inline constexpr double min_pitch_semitones = -48.0;
inline constexpr double max_pitch_semitones = 48.0;
inline constexpr double cents_per_semitone = 100.0;
inline constexpr double max_choke_group = 8.0;
inline constexpr double min_swing_percent = 50.0;
inline constexpr double max_swing_percent = 75.0;
inline constexpr double max_release_ms = 5'000.0;
inline constexpr double max_hold_ms = 5'000.0;
inline constexpr double max_decay_ms = 5'000.0;
inline constexpr double max_attack_ms = 2'000.0;

[[nodiscard]] const EngineDescriptor& descriptor();

// The built-in sample a factory preset loads as its one chop (slot 0), and
// that chop's tag; nullopt for a preset without a sample (Init).
struct PresetSample {
  std::string_view sample;
  std::string_view tag;
  std::string_view name;
};
[[nodiscard]] std::optional<PresetSample> preset_sample(std::size_t preset) noexcept;
// The preset whose document a new instrument starts with (no preset chosen).
inline constexpr std::size_t default_document_preset = 1;

} // namespace oscillate::native_dsp::synth::crate::detail

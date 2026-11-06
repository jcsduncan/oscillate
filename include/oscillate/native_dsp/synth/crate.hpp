#pragma once

#include "oscillate/native_dsp/synth/runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// Crate (ADR-0063, spec 09 §2): a chop sampler. One sample in the document's
// media slot `sample`, cut by the document's regions into up to 32 chops
// played from pads (key 36 upward) or across the keyboard, each with its own
// pitch, envelope and filter, through a vintage "era" character stage.
namespace oscillate::native_dsp::synth::crate {

inline constexpr std::string_view engine_id = "crate";

// The document (spec 09 §1.1, §2).
inline constexpr std::string_view sample_slot = "sample";
// The sample's reviewed tempo, in BPM, for Follow Tempo: a NativeLabel whose
// text is a decimal number.
inline constexpr std::string_view bpm_label = "crate.bpm";
// The sample's display name (its file's name when it was loaded): a
// NativeLabel, so the panel can show it after the project reopens.
inline constexpr std::string_view sample_name_label = "crate.sample";
inline constexpr double min_sample_bpm = 20.0;
inline constexpr double max_sample_bpm = 999.0;
inline constexpr std::size_t max_chops = 32;      // Region slots 0..31; at most 32 regions.
inline constexpr std::size_t bank_size = 16;      // Pads per bank (A, B).
inline constexpr std::uint8_t first_pad_key = 36; // C1 plays pad 1.
inline constexpr std::array<std::string_view, 4> tags{"kick", "snare", "hat", "other"};

// Sounds the engine ships itself (NativeBuiltinMedia names for the sample
// slot): the starter one-shots, regenerated exactly as
// scripts/generate-starter-sounds.py bundles them in assets/starter.
inline constexpr std::array<std::string_view, 4> builtin_samples{"kick", "snare", "hat", "pluck"};
inline constexpr std::uint32_t builtin_sample_rate = 48'000;
// A built-in sample's mono frames at builtin_sample_rate; empty for an
// unknown name. Built on first use (control side), then immutable.
[[nodiscard]] std::span<const float> builtin_sample(std::string_view name);

// Choice parameters' values.
enum class Layout : std::uint8_t { pads, keys };
enum class TimeMode : std::uint8_t { repitch, follow };
enum class Era : std::uint8_t { clean, sp12, mpc60, s950, tape };
enum class ChopMode : std::uint8_t { oneshot, gate, loop };
enum class ChopFilter : std::uint8_t { off, lowpass, highpass, bandpass, notch };

// What choosing an era sets (spec 09 §2.4): the panel writes these into
// crate.bits, crate.rate and crate.drive; the knobs stay editable after.
// Starting figures, placeholders until fitted to tools/crate_reference.
struct EraSettings {
  double bits = 24.0;
  double rate = 48'000.0;
  double drive = 0.0;
};
inline constexpr std::array<EraSettings, 5> era_settings{{
    {24.0, 48'000.0, 0.0},  // clean
    {12.0, 26'040.0, 0.15}, // sp12
    {12.0, 40'000.0, 0.1},  // mpc60
    {12.0, 31'250.0, 0.1},  // s950
    {24.0, 48'000.0, 0.25}, // tape
}};
// crate.rate at its maximum and crate.bits at theirs bypass those stages.
inline constexpr double bypass_rate = 48'000.0;
inline constexpr double bypass_bits = 24.0;

// Per-chop parameters, `crate.chop.NN.<control>` with NN the region's slot.
inline constexpr std::array<std::string_view, 17> chop_controls{
    "pitch",   "fine",    "gain",   "pan",       "start",   "length", "attack", "hold", "decay",
    "sustain", "release", "cutoff", "resonance", "reverse", "mode",   "filter", "choke"};
// "crate.chop.07.cutoff".
[[nodiscard]] std::string chop_parameter_id(std::size_t slot, std::string_view control);

// Telemetry layout (ADR-0037).
namespace telemetry {
// 0..31: each slot's newest sounding playhead, 0..1 of the chop; -1 idle.
inline constexpr std::size_t playheads = 0;
inline constexpr std::size_t last_slot = max_chops; // Slot of the last pad struck; -1 before any.
inline constexpr std::size_t last_velocity = max_chops + 1; // 1..127; 0 before any.
inline constexpr std::size_t output_peak_db =
    max_chops + 2; // The last block's peak, dBFS (>= -120).
inline constexpr std::size_t size = max_chops + 3;
} // namespace telemetry

[[nodiscard]] const SynthEngine& engine();

} // namespace oscillate::native_dsp::synth::crate

#include "table.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oscillate::native_dsp::synth::crate {

std::string chop_parameter_id(std::size_t slot, std::string_view control) {
  char number[8];
  std::snprintf(number, sizeof number, "%02zu", slot);
  return "crate.chop." + std::string(number) + "." + std::string(control);
}

namespace detail {
namespace {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

constexpr std::array layouts{"Pads"sv, "Keys"sv};
constexpr std::array times{"Repitch"sv, "Follow Tempo"sv};
constexpr std::array eras{"Clean"sv, "SP-12"sv, "MPC60"sv, "S950"sv, "Tape"sv};
constexpr std::array switches{"Off"sv, "On"sv};
constexpr std::array modes{"One-shot"sv, "Gate"sv, "Loop"sv};
constexpr std::array filters{"Off"sv, "Low-pass"sv, "High-pass"sv, "Band-pass"sv, "Notch"sv};

constexpr double glide = synth_glide_seconds; // Panel jumps on continuous controls.
constexpr double pitch_glide = 0.005;
// Linear knobs over ranges that start at zero but want log-like travel.
constexpr double time_skew = 3.0;

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }

ParameterDescriptor continuous(std::string_view id, std::string_view name, std::string_view group,
                               std::string_view unit, S scale, double min, double max, double def,
                               double glide_seconds = glide, double skew = 1.0) {
  return {id, name, group, unit, scale, min, max, def, {}, T::live, glide_seconds, true, skew};
}
ParameterDescriptor discrete(std::string_view id, std::string_view name, std::string_view group,
                             std::string_view unit, S scale, double min, double max, double def,
                             std::span<const std::string_view> choices = {}, T timing = T::live) {
  return {id, name, group, unit, scale, min, max, def, choices, timing, 0.0, false, 1.0};
}

// Built in place once: the descriptors view the generated ids and groups.
struct Built {
  Built();
  std::deque<std::string> strings; // Stable storage for generated ids and groups.
  std::vector<ParameterDescriptor> parameters;
  EngineDescriptor descriptor{};
};

// Factory presets (spec 09 §2.5). Each starter loads its bundled one-shot as
// one chop; the era showcases wait for a break sourced under a bundling licence.
using V = std::pair<std::string_view, double>;
constexpr std::array<V, 0> init_values{};
constexpr std::array kick_values{V{"crate.chop.00.release", 60.0}};
// The pluck rings at 220 Hz (A3) and plays across the keys, held by the note.
constexpr std::array pluck_values{V{"crate.layout", 1.0}, V{"crate.root", 57.0},
                                  V{"crate.chop.00.mode", 1.0}, V{"crate.chop.00.release", 250.0}};
constexpr std::array snare_values{V{"crate.chop.00.release", 60.0}};
constexpr std::array hat_values{V{"crate.chop.00.release", 40.0}};

const std::array<Preset, 5> presets{{
    {"Init", "Init", init_values},
    {"Kick", "Drums", kick_values},
    {"Pluck", "Melodic", pluck_values},
    {"Snare", "Drums", snare_values},
    {"Hat", "Drums", hat_values},
}};
// Index-aligned with presets.
constexpr std::array<std::optional<PresetSample>, 5> preset_samples{{
    std::nullopt,
    PresetSample{"kick", "kick", "Kick"},
    PresetSample{"pluck", "other", "Pluck"},
    PresetSample{"snare", "snare", "Snare"},
    PresetSample{"hat", "hat", "Hat"},
}};

Built::Built() {
  {
    Built& b = *this;
    auto& p = b.parameters;
    p.reserve(parameter_count);
    const auto& [bits_def, rate_def, drive_def] = era_settings.front();
    // Globals, in detail::global order.
    p.push_back(discrete("crate.layout", "Layout", "Play", "", S::choice, 0, last(layouts.size()),
                         0, layouts));
    p.push_back(
        discrete("crate.keys_chop", "Keys Chop", "Play", "", S::stepped, 0, last(max_chops), 0));
    p.push_back(discrete("crate.root", "Root", "Play", "", S::stepped, 0, 127, 60));
    p.push_back(discrete("crate.time", "Time", "Play", "", S::choice, 0, last(times.size()), 0,
                         times, T::next_note));
    p.push_back(
        discrete("crate.era", "Era", "Character", "", S::choice, 0, last(eras.size()), 0, eras));
    p.push_back(
        discrete("crate.bits", "Bits", "Character", "bit", S::stepped, 4, bypass_bits, bits_def));
    p.push_back(continuous("crate.rate", "Rate", "Character", "Hz", S::logarithmic, 4'000,
                           bypass_rate, rate_def));
    p.push_back(continuous("crate.drive", "Drive", "Character", "%", S::linear, 0, 1, drive_def));
    p.push_back(continuous("crate.vinyl", "Vinyl", "Character", "%", S::linear, 0, 1, 0));
    p.push_back(continuous("crate.wow", "Wow", "Character", "%", S::linear, 0, 1, 0));
    p.push_back(continuous("crate.output", "Output", "Character", "dB", S::linear, -24, 12, 0));
    p.push_back(continuous("crate.swing", "Swing", "Play", "", S::linear, min_swing_percent,
                           max_swing_percent, min_swing_percent));
    p.push_back(continuous("crate.velocity", "Velocity", "Play", "%", S::linear, 0, 1, 0.7));
    p.push_back(discrete("crate.voices", "Voices", "Play", "", S::stepped, 1, max_voices, 16, {},
                         T::next_note));
    // Each slot's chop controls, in detail::chop order.
    for (std::size_t slot = 0; slot < max_chops; ++slot) {
      const auto id = [&](std::size_t control) -> std::string_view {
        b.strings.push_back(chop_parameter_id(slot, chop_controls[control]));
        return b.strings.back();
      };
      char label[16];
      std::snprintf(label, sizeof label, "Chop %02zu", slot + 1);
      b.strings.emplace_back(label);
      const std::string_view g = b.strings.back();
      p.push_back(continuous(id(chop::pitch), "Pitch", g, "st", S::linear, min_pitch_semitones,
                             max_pitch_semitones, 0, pitch_glide));
      p.push_back(continuous(id(chop::fine), "Fine", g, "ct", S::linear, -cents_per_semitone,
                             cents_per_semitone, 0, pitch_glide));
      p.push_back(continuous(id(chop::gain), "Gain", g, "dB", S::linear, -48, 12, 0));
      p.push_back(continuous(id(chop::pan), "Pan", g, "%", S::linear, -1, 1, 0));
      p.push_back(continuous(id(chop::start), "Start", g, "%", S::linear, 0, 1, 0));
      p.push_back(continuous(id(chop::length), "Length", g, "%", S::linear, 0.01, 1, 1));
      // Attack and hold reach zero, so their travel is skewed rather than
      // logarithmic (a logarithmic scale needs a positive minimum).
      p.push_back(continuous(id(chop::attack), "Attack", g, "ms", S::linear, 0, max_attack_ms, 1,
                             glide, time_skew));
      p.push_back(continuous(id(chop::hold), "Hold", g, "ms", S::linear, 0, max_hold_ms, 0, glide,
                             time_skew));
      p.push_back(
          continuous(id(chop::decay), "Decay", g, "ms", S::logarithmic, 1, max_decay_ms, 5'000));
      p.push_back(continuous(id(chop::sustain), "Sustain", g, "%", S::linear, 0, 1, 1));
      p.push_back(
          continuous(id(chop::release), "Release", g, "ms", S::logarithmic, 1, max_release_ms, 30));
      p.push_back(
          continuous(id(chop::cutoff), "Cutoff", g, "Hz", S::logarithmic, 20, 20'000, 20'000));
      p.push_back(continuous(id(chop::resonance), "Resonance", g, "%", S::linear, 0, 1, 0));
      // The switches a sounding chop keeps; new notes take the new value.
      p.push_back(discrete(id(chop::reverse), "Reverse", g, "", S::choice, 0, last(switches.size()),
                           0, switches, T::next_note));
      p.push_back(discrete(id(chop::mode), "Mode", g, "", S::choice, 0, last(modes.size()), 0,
                           modes, T::next_note));
      p.push_back(discrete(id(chop::filter), "Filter", g, "", S::choice, 0, last(filters.size()), 0,
                           filters, T::next_note));
      p.push_back(discrete(id(chop::choke), "Choke", g, "", S::stepped, 0, max_choke_group, 0, {},
                           T::next_note));
    }
    b.descriptor = {engine_id, "Crate", 1, b.parameters, presets};
  }
}

const Built& built() {
  static const Built table;
  return table;
}

} // namespace

const EngineDescriptor& descriptor() { return built().descriptor; }

std::optional<PresetSample> preset_sample(std::size_t preset) noexcept {
  return preset < preset_samples.size() ? preset_samples[preset] : std::nullopt;
}

} // namespace detail
} // namespace oscillate::native_dsp::synth::crate

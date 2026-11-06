#pragma once

#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/project/model.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// Anvil (ADR-0060): one kick, drawn. The body's pitch, amplitude and drive
// are curves over the hit's length; a click, a sub and a tuned rumble layer
// sit beside it, into a master drive, tone, punch, glue and a ceiling.
namespace oscillate::native_dsp::synth::anvil {

// The document's curves (NativeSynthDocument.curves). x is 0..1 of
// anvil.length. A missing curve plays the engine's default shape.
inline constexpr std::string_view pitch_curve = "pitch"; // y: log frequency, below.
inline constexpr std::string_view amp_curve = "amp";     // y: gain 0..1.
inline constexpr std::string_view drive_curve = "drive"; // y: drive 0..1.
// The media slot holding a user transient for click type Sample.
inline constexpr std::string_view click_slot = "click";

// A pitch curve's y (0..1, as every document curve) is a logarithmic
// frequency from 20 Hz to 2 kHz, so a straight segment is a straight line on
// a log axis and segments blend in log frequency.
inline constexpr double pitch_min_hz = 20.0;
inline constexpr double pitch_max_hz = 2'000.0;
[[nodiscard]] inline double hz_of_y(double y) noexcept {
  return pitch_min_hz * std::pow(pitch_max_hz / pitch_min_hz, y);
}
[[nodiscard]] inline double y_of_hz(double hz) noexcept {
  const double y = std::log(hz / pitch_min_hz) / std::log(pitch_max_hz / pitch_min_hz);
  return y < 0.0 ? 0.0 : (y > 1.0 ? 1.0 : y);
}

// Telemetry layout (ADR-0037).
namespace telemetry {
inline constexpr std::size_t position = 0;      // In the sounding hit, 0..1 of its length; -1 idle.
inline constexpr std::size_t last_note = 1;     // MIDI key of the last hit; -1 before any.
inline constexpr std::size_t last_velocity = 2; // 1..127; 0 before any.
inline constexpr std::size_t output_peak_db = 3; // The last block's output peak, dBFS (>= -120).
inline constexpr std::size_t size = 4;
} // namespace telemetry

// One hit rendered offline through the engine's own kernel, for the panel.
// master is the mono sum (L+R)/2 of max(kick length, one beat at bpm) frames;
// rumble is the rumble layer alone after its duck, mono, before the master
// stage; kick_frames is the hit's length in frames; tail_hz the body's tail
// frequency as played (tune and key applied); peak_db master's peak, dBFS.
struct Preview {
  std::vector<float> master;
  std::vector<float> rumble;
  std::size_t kick_frames = 0;
  double tail_hz = 0.0;
  double peak_db = -120.0;
};

// values are dense, in table order; the document must be valid for Anvil
// (an invalid one renders the defaults' curves); media holds the decoded
// click slot, or null. Control only; allocates. It costs the kernel's
// render, about 0.4 us a frame: render at 24 kHz to redraw on every drag.
[[nodiscard]] Preview render_preview(std::span<const double> values,
                                     const project::NativeSynthDocument& document,
                                     const SynthMedia* media, std::uint32_t sample_rate, double bpm,
                                     std::uint8_t key = 36, std::uint8_t velocity = 127);

[[nodiscard]] const SynthEngine& engine();

} // namespace oscillate::native_dsp::synth::anvil

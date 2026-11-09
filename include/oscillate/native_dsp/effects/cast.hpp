#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/project/native_document.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace oscillate::native_dsp::effects::cast {

// Cast (ADR-0115): a convolution reverb. Zero latency: a direct-form head
// and four uniformly partitioned FFT tiers, each starting at least twice its
// block after tap zero, so the longer tiers spread their work over a whole
// block and the output equals the direct convolution sample for sample at
// any block split. Mono, stereo (one response per channel) and true stereo
// (four responses: left in to left and right out, then right in) IRs. The
// IR group (start, length, stretch, decay, fade in, reverse, normalise)
// reshapes the IR off the callback; a new IR crossfades in over one block of
// each tier. Pre-delay, width, tone (low cut, high cut, tilt), ducking by
// the input or a sidechain, wet gain and an equal-power mix are live.
[[nodiscard]] const EffectEngine& engine();

// The document's media slots: `ir` holds a bundled IR (a builtin name) or a
// user's file (mono or stereo, any rate); `ir.right`, a stereo file, makes
// `ir` the left input's response and itself the right input's (true stereo).
inline constexpr std::string_view ir_slot = "ir";
inline constexpr std::string_view ir_right_slot = "ir.right";

// The longest IR Cast plays, after stretching; longer IRs fade out here.
inline constexpr double max_ir_seconds = 10.0;
// The longest pre-delay (cast.predelay's maximum).
inline constexpr double max_predelay_seconds = 0.25;

// The bundled IRs: synthesised on first use at the session rate from
// physical models (ADR-0115), true stereo, cached for the process.
struct BuiltinIr {
  std::string_view id;   // The media name in the document.
  std::string_view name; // Shown in the browser.
  std::string_view category;
};
[[nodiscard]] std::span<const BuiltinIr> builtin_irs() noexcept;
// The IR an empty document plays.
inline constexpr std::string_view default_ir = "hall";

// A document playing bundled IR `id`.
[[nodiscard]] project::NativeSynthDocument builtin_document(std::string_view id);

// The bundled IR `id` at `sample_rate` as four responses (left in to left
// out, left in to right out, right in to left out, right in to right out),
// before any shaping; empty for an unknown id. Control only; cached.
struct Responses {
  std::array<std::span<const float>, 4> paths;
};
[[nodiscard]] Responses builtin_responses(std::string_view id, double sample_rate);

// Points in the IR overview the telemetry carries.
inline constexpr std::size_t overview_points = 128;

// Telemetry: the input's and the wet signal's peaks per channel (dBFS,
// held), the ducking (dB, positive), the IR as shaped (its length in
// seconds, its responses: 1 mono, 2 stereo, 4 true stereo, its RT60 in
// seconds), then the shaped IR's envelope (peak dBFS per slice, loudest
// response) and its energy decay curve (dB, 0 at the start), each over
// overview_points equal slices of its length.
namespace telemetry {
inline constexpr std::size_t input_peak = 0; // left, right
inline constexpr std::size_t wet_peak = 2;   // left, right
inline constexpr std::size_t duck = 4;
inline constexpr std::size_t length_seconds = 5;
inline constexpr std::size_t responses = 6;
inline constexpr std::size_t rt60_seconds = 7;
inline constexpr std::size_t envelope = 8;
inline constexpr std::size_t decay_curve = envelope + overview_points;
inline constexpr std::size_t size = decay_curve + overview_points;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::cast

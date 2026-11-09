#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

// Cast's IR preparation (ADR-0115), on control only: resampling to the
// session rate, the bundled IRs' physical models, and the IR group's
// shaping (start, length, stretch, decay, fade in, reverse, normalise).
namespace oscillate::native_dsp::effects::cast_ir {

// One, two or four equal-length responses (see cast_dsp::Routing).
using Responses = std::vector<std::vector<float>>;

// The rate the bundled IRs are modelled at; resampled from it to the
// session's.
inline constexpr double design_rate = 48'000.0;

// Band-limited resampling (Kaiser-windowed sinc, about 100 dB down in the
// stop band), at most `max_frames` out.
[[nodiscard]] std::vector<float> resample(std::span<const float> in, double from, double to,
                                          std::size_t max_frames);

// A bundled IR at design_rate as four responses; empty for an unknown id.
[[nodiscard]] Responses synthesise(std::string_view id);

struct Trim {
  double start_seconds = 0.0;
  double length = 1.0;  // Of what follows the start, 0..1.
  double stretch = 1.0; // Time scale; pitch is kept.
};
// Start, length and stretch: the costly part, cached by its caller. At
// most `max_frames` frames; a cut end fades out.
[[nodiscard]] Responses trim_and_stretch(const Responses& source, double rate, const Trim& trim,
                                         std::size_t max_frames);

struct Envelope {
  double decay = 1.0; // RT60 scale, 0..1.
  double fade_in_seconds = 0.0;
  bool reverse = false;
  bool normalise = true;
};
// The rest of the IR group, in place. Normalising sets the mean power each
// output receives from uncorrelated unit-power inputs to 1.
void shape(Responses& responses, double rate, const Envelope& envelope);

// The broadband RT60 by Schroeder integration (seconds; the length when it
// cannot be measured).
[[nodiscard]] double rt60(const Responses& responses, double rate);

// The overview the panel draws: peak dBFS of the loudest response and the
// energy decay curve (dB, 0 at the start), over `points` equal slices.
struct Overview {
  std::vector<float> envelope;
  std::vector<float> decay_curve;
};
[[nodiscard]] Overview overview(const Responses& responses, std::size_t points);

} // namespace oscillate::native_dsp::effects::cast_ir

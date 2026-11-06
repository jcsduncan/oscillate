#pragma once

#include "oscillate/test/audio_constants.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Synthetic sources for Clay's oracles: a sung vowel (a band-limited glottal
// source through three formant resonators, with vibrato and slides),
// harmonic tones (optionally stretched like a string) and percussive hits.
namespace clay_signals {

inline constexpr double two_pi = 6.283185307179586;
inline constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
inline constexpr double ceiling = 0.45; // harmonics stop at this share of the rate
inline constexpr double default_level = 0.05;

[[nodiscard]] inline double cents_to_hz(double cents) {
  return 440.0 * std::exp2((cents - 6'900.0) / 1'200.0);
}

[[nodiscard]] inline std::size_t frames(double seconds) {
  return static_cast<std::size_t>(std::llround(seconds * rate));
}

struct Formant {
  double hz;
  double bandwidth;
};
// The vowel /a/ (Peterson & Barney, adult male).
inline constexpr std::array<Formant, 3> vowel_a{
    {{700.0, 110.0}, {1'220.0, 120.0}, {2'600.0, 160.0}}};

[[nodiscard]] inline double formant_gain(double hz, std::span<const Formant> formants) {
  double gain = 1.0;
  for (const auto& f : formants) {
    const double f2 = f.hz * f.hz;
    gain *=
        f2 / std::sqrt((f2 - hz * hz) * (f2 - hz * hz) + (f.bandwidth * hz) * (f.bandwidth * hz));
  }
  return gain;
}

// A sung note: pitch in cents (from -> to over the slide at the end),
// vibrato depth (cents) and rate, and linear attack and release.
struct SungNote {
  double start = 0.0;
  double end = 0.0;
  double cents = 0.0;
  double vibrato_cents = 0.0;
  double vibrato_hz = 5.5;
  // Slide into the next pitch over the last `slide` seconds (legato).
  double slide = 0.0;
  double slide_to = 0.0;
  double attack = 0.01;
  double release = 0.02;
};

// Renders notes back to back; a note with a slide must be followed by one
// starting at its end (the slide is centred on that end).
[[nodiscard]] inline std::vector<float> sing(std::span<const SungNote> notes, double seconds,
                                             std::span<const Formant> formants = vowel_a,
                                             double level = default_level) {
  std::vector<float> out(frames(seconds), 0.0F);
  std::vector<double> f0(out.size(), 0.0);
  std::vector<double> amplitude(out.size(), 0.0);
  for (std::size_t i = 0; i < notes.size(); ++i) {
    const auto& n = notes[i];
    const bool legato_in = i > 0 && notes[i - 1].slide > 0.0 && notes[i - 1].end == n.start;
    const bool legato_out = n.slide > 0.0;
    for (auto k = frames(n.start); k < frames(n.end) && k < out.size(); ++k) {
      const double t = static_cast<double>(k) / rate;
      double cents = n.cents + n.vibrato_cents * std::sin(two_pi * n.vibrato_hz * (t - n.start));
      if (legato_out && t > n.end - n.slide / 2.0)
        cents = n.cents + (n.slide_to - n.cents) * (t - (n.end - n.slide / 2.0)) / n.slide;
      if (legato_in && t < n.start + notes[i - 1].slide / 2.0) {
        const auto& p = notes[i - 1];
        cents = p.cents + (n.cents - p.cents) * (t - (n.start - p.slide / 2.0)) / p.slide;
      }
      f0[k] = cents_to_hz(cents);
      double a = 1.0;
      if (!legato_in && t - n.start < n.attack)
        a = (t - n.start) / n.attack;
      if (!legato_out && n.end - t < n.release)
        a = std::min(a, (n.end - t) / n.release);
      amplitude[k] = a;
    }
  }
  double phase = 0.0;
  for (std::size_t k = 0; k < out.size(); ++k) {
    if (f0[k] <= 0.0) {
      phase = 0.0;
      continue;
    }
    phase = std::remainder(phase + two_pi * f0[k] / rate, two_pi);
    double sum = 0.0;
    for (int h = 1; h * f0[k] < ceiling * rate; ++h)
      sum += formant_gain(h * f0[k], formants) / h * std::sin(h * phase);
    out[k] = static_cast<float>(level * amplitude[k] * sum);
  }
  return out;
}

// A harmonic tone with partials 1/h, optionally stretched by
// inharmonicity B: f_h = h f0 sqrt(1 + B h^2).
inline void add_tone(std::vector<float>& out, double hz, double start, double end,
                     double inharmonicity = 0.0, int max_harmonics = 40, double level = 0.08,
                     double attack = 0.01, double release = 0.05) {
  for (int h = 1; h <= max_harmonics; ++h) {
    const double f = h * hz * std::sqrt(1.0 + inharmonicity * h * h);
    if (f >= ceiling * rate)
      break;
    const double phase0 = 0.37 * h; // fixed, so the sum has no big peak
    for (auto k = frames(start); k < frames(end) && k < out.size(); ++k) {
      const double t = static_cast<double>(k) / rate - start;
      const double a = std::min(1.0, std::min(t / attack, (end - start - t) / release));
      out[k] += static_cast<float>(level * a / h * std::sin(two_pi * f * t + phase0));
    }
  }
}

// A struck hit: a click of seeded noise then a decaying sine.
inline void add_hit(std::vector<float>& out, double at, double hz, double decay_seconds,
                    std::uint32_t seed, double level = 0.4) {
  std::uint32_t state = seed;
  const auto noise = [&] {
    state = state * 1'664'525U + 1'013'904'223U;
    return static_cast<double>(state) / 4'294'967'296.0 * 2.0 - 1.0;
  };
  const auto click = frames(0.002);
  for (auto k = frames(at); k < out.size(); ++k) {
    const double t = static_cast<double>(k) / rate - at;
    if (t > 8.0 * decay_seconds)
      break;
    double value = std::exp(-t / decay_seconds) * std::sin(two_pi * hz * t);
    if (k - frames(at) < click)
      value += noise();
    out[k] += static_cast<float>(level * value);
  }
}

} // namespace clay_signals

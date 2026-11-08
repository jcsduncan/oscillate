#pragma once

// Kick and bass test material and measurements for Pocket (ADR-0041):
// kicks rendered by the Pulse drum synth, synthetic basses, a third-octave
// STFT analysis independent of Pocket's own filterbank, and helpers to run
// Pocket with a sidechain.

#include "effect_test_support.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/pocket.hpp"
#include "oscillate/native_dsp/synth/pulse.hpp"
#include "oscillate/test/audio_constants.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string_view>
#include <vector>

namespace pocket_test {

using namespace effect_test;
namespace pocket = oscillate::native_dsp::effects::pocket;

inline constexpr double rate = oscillate::test::sample_rate_48k_hz;
inline constexpr std::size_t second = 48'000;

// Kicks from a Pulse kit on key 36 at `onsets`, velocity 110.
inline Stereo kicks(std::string_view kit, const std::vector<std::size_t>& onsets,
                    std::size_t frames) {
  const auto& engine = oscillate::native_dsp::synth::pulse::engine();
  auto runtime = make_runtime(engine, preset(*engine.descriptor, kit));
  std::vector<Event> events;
  std::uint64_t id = 1;
  for (const auto onset : onsets) {
    auto hit = note(onset, 2'400, 36, id++, 110);
    events.insert(events.end(), hit.begin(), hit.end());
  }
  const auto out = render(*runtime, events, frames);
  return {out.left, out.right};
}

inline std::vector<std::size_t> four_on_the_floor(std::size_t hits, double bpm = 120.0,
                                                  std::size_t start = 2'400) {
  std::vector<std::size_t> onsets;
  const double beat = 60.0 / bpm * rate;
  onsets.reserve(hits);
  for (std::size_t i = 0; i < hits; ++i) {
    onsets.push_back(start + static_cast<std::size_t>(std::lround(beat * static_cast<double>(i))));
  }
  return onsets;
}

enum class Bass : std::uint8_t { sub, saw, line };

// Sub: a sustained 55 Hz sine. Saw: E1 with harmonics to 8 kHz. Line: A1,
// C2, E1, G1 a beat each at 120 BPM, sine plus second harmonic.
inline Stereo bass(Bass kind, std::size_t frames, double level = 0.4) {
  Stereo out{std::vector<double>(frames), std::vector<double>(frames)};
  const std::array<double, 4> line{55.0, 65.41, 41.2, 49.0};
  double phase = 0.0;
  for (std::size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / rate;
    double x = 0.0;
    if (kind == Bass::sub) {
      x = std::sin(2.0 * std::numbers::pi * 55.0 * t);
    } else if (kind == Bass::saw) {
      for (int n = 1; 41.2 * n < 8'000.0; ++n) {
        x += std::sin(2.0 * std::numbers::pi * 41.2 * n * t) / std::pow(n, 1.2) * 0.6;
      }
    } else {
      const auto beat = static_cast<std::size_t>(t * 2.0) % line.size();
      phase += line[beat] / rate;
      x = std::sin(2.0 * std::numbers::pi * phase) + 0.3 * std::sin(4.0 * std::numbers::pi * phase);
    }
    out.left[i] = level * x;
    out.right[i] = level * x;
  }
  return out;
}

inline Stereo pocket_bass(const std::vector<double>& values, const Stereo& bass_in,
                          const Stereo& kick, std::size_t block = 128) {
  auto runtime = make_effect(pocket::engine(), values);
  return process(*runtime, bass_in, {.block = block, .sidechain = &kick});
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the sum commutes.
inline Stereo sum(const Stereo& a, const Stereo& b) {
  Stereo out = a;
  for (std::size_t i = 0; i < a.size(); ++i) {
    out.left[i] += b.left[i];
    out.right[i] += b.right[i];
  }
  return out;
}

// Third-octave band energies per STFT frame (2048-point Hann, hop 512).
struct Bands {
  std::vector<double> centres;
  std::vector<std::vector<double>> energy; // [frame][band]
};

inline Bands third_octaves(const std::vector<double>& x, double low = 31.5, double high = 8'000.0) {
  constexpr std::size_t n = 2'048;
  constexpr std::size_t hop = 512;
  Bands out;
  // The sweep grid is the test input.
  // NOLINTNEXTLINE(bugprone-float-loop-counter,clang-analyzer-security.FloatLoopCounter)
  for (double f = low; f <= high * 1.01; f *= std::pow(2.0, 1.0 / 3.0)) {
    out.centres.push_back(f);
  }
  const double bin_hz = rate / static_cast<double>(n);
  for (std::size_t start = 0; start + n <= x.size(); start += hop) {
    std::vector<std::complex<double>> w(n);
    for (std::size_t i = 0; i < n; ++i) {
      const double hann =
          oscillate::native_dsp::dsp::hann(static_cast<double>(i), static_cast<double>(n));
      w[i] = x[start + i] * hann;
    }
    fft(w);
    std::vector<double> e(out.centres.size(), 0.0);
    for (std::size_t b = 0; b < out.centres.size(); ++b) {
      const double lo = out.centres[b] / std::pow(2.0, 1.0 / 6.0);
      const double hi = out.centres[b] * std::pow(2.0, 1.0 / 6.0);
      for (auto k = static_cast<std::size_t>(lo / bin_hz);
           k <= static_cast<std::size_t>(hi / bin_hz) && k < n / 2; ++k) {
        const double hz = static_cast<double>(k) * bin_hz;
        if (hz >= lo && hz < hi) {
          e[b] += std::norm(w[k]);
        }
      }
    }
    out.energy.push_back(e);
  }
  return out;
}

// Masking overlap: the energy both share, summed over frames and the bands
// between `low` and `high` Hz, in dB.
inline double overlap_db(const Bands& kick, const Bands& bass, double low, double high) {
  double total = 0.0;
  for (std::size_t f = 0; f < std::min(kick.energy.size(), bass.energy.size()); ++f) {
    for (std::size_t b = 0; b < kick.centres.size(); ++b) {
      if (kick.centres[b] >= low && kick.centres[b] <= high) {
        total += std::min(kick.energy[f][b], bass.energy[f][b]);
      }
    }
  }
  return 10.0 * std::log10(total + 1e-30);
}

inline double band_energy(const std::vector<double>& x, std::size_t from, std::size_t to,
                          double low, double high) {
  const std::size_t n = 8'192;
  std::vector<std::complex<double>> w(n);
  for (std::size_t i = from; i < to && i - from < n; ++i) {
    w[i - from] = x[i];
  }
  fft(w);
  double e = 0.0;
  for (std::size_t k = 1; k < n / 2; ++k) {
    const double hz = static_cast<double>(k) * rate / static_cast<double>(n);
    if (hz >= low && hz < high) {
      e += std::norm(w[k]);
    }
  }
  return e;
}

inline double correlation(const std::vector<double>& a, const std::vector<double>& b,
                          std::size_t begin, std::size_t end) {
  double ab = 0.0;
  double aa = 0.0;
  double bb = 0.0;
  for (std::size_t i = begin; i < end; ++i) {
    ab += a[i] * b[i];
    aa += a[i] * a[i];
    bb += b[i] * b[i];
  }
  return ab / std::sqrt(aa * bb + 1e-30);
}

// The zero-lag envelope of `x` in the third-octave around `hz` (or between
// `lo` and `hi`): an ideal band mask applied to the analytic signal
// (positive frequencies doubled), so the magnitude is the band's exact
// Hilbert envelope.
inline std::vector<double> band_envelope(const std::vector<double>& x, double hz, double lo = 0.0,
                                         double hi = 0.0) {
  std::size_t n = 1;
  while (n < x.size()) {
    n *= 2;
  }
  std::vector<std::complex<double>> w(n);
  for (std::size_t i = 0; i < x.size(); ++i) {
    w[i] = x[i];
  }
  fft(w);
  if (lo <= 0.0) {
    lo = hz / std::pow(2.0, 1.0 / 6.0);
    hi = hz * std::pow(2.0, 1.0 / 6.0);
  }
  for (std::size_t k = 0; k < n; ++k) {
    const double f = static_cast<double>(k) * rate / static_cast<double>(n);
    w[k] = (k > 0 && k < n / 2 && f >= lo && f < hi) ? 2.0 * w[k] : 0.0;
  }
  // Inverse by conjugation.
  for (auto& v : w) {
    v = std::conj(v);
  }
  fft(w);
  std::vector<double> out(x.size());
  for (std::size_t i = 0; i < x.size(); ++i) {
    out[i] = std::abs(w[i]) / static_cast<double>(n);
  }
  return out;
}

} // namespace pocket_test

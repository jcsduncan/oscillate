#include "oscillate/native_dsp/synth/nova/tables.hpp"

#include "dsp/offline_fft.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "vowels.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Nova's factory wavetables (spec 08 §3 and §12), generated in-house. Every
// generator describes one frame at morph position t in [0, 1] as harmonic
// amplitudes. Piecewise-linear shapes (saws, pulses, syncs, steps) are
// integrated exactly, so they carry no aliasing at all; other time-domain
// shapes are sampled 8x finer than a frame before analysis. The growl
// generators shape saws and sines through FM, folding, clipping and the
// sung vowels' formants.
namespace oscillate::native_dsp::synth::nova {
namespace {

using Complex = std::complex<double>;
constexpr double pi = std::numbers::pi;
constexpr double two_pi = 2.0 * pi;
constexpr std::size_t harmonic_count = table_frame_size / 2;
// Time-domain generators sample one cycle at this many points.
constexpr std::size_t sample_length = 16'384;

// c[h] for h = 1..harmonic_count; the cycle is sum Re(c[h] e^(i 2 pi h x)).
using Spectrum = std::vector<Complex>;

[[nodiscard]] Complex sine_partial(double amplitude) noexcept { return {0.0, -amplitude}; }
[[nodiscard]] double saw(double x) noexcept { return 2.0 * (x - std::floor(x)) - 1.0; }
[[nodiscard]] double frac(double x) noexcept { return x - std::floor(x); }
using native_dsp::dsp::db_to_gain;
[[nodiscard]] double smooth(double x) noexcept {
  x = std::clamp(x, 0.0, 1.0);
  return x * x * (3.0 - 2.0 * x);
}

// A small deterministic generator for seeded spectra.
using Random = native_dsp::dsp::Xorshift64Star;

// ---- Spectrum tools ------------------------------------------------------------------

struct Point {
  double x;
  double y;
};

// The exact harmonics of a piecewise-linear cycle through `points`, which
// run from x = 0 to x = 1. Two points at the same x are a jump. Per
// segment, the integral of (y0 + s (x - x0)) e^(-i w x) is
// [(y i / w + s / w^2) e^(-i w x)] between its ends, so each distinct x (a
// knot) contributes its jump in value and its change in slope.
void piecewise(std::span<const Point> points, Spectrum& s) {
  struct Knot {
    double x;
    double jump; // Value arriving minus value leaving.
    double bend; // Slope arriving minus slope leaving.
  };
  std::vector<Knot> knots;
  knots.reserve(points.size());
  const auto knot_at = [&](double x) -> Knot& {
    if (knots.empty() || knots.back().x != x) {
      knots.push_back({x, 0.0, 0.0});
    }
    return knots.back();
  };
  for (std::size_t p = 0; p + 1 < points.size(); ++p) {
    const auto& a = points[p];
    const auto& b = points[p + 1];
    const double width = b.x - a.x;
    if (width <= 0.0) {
      continue;
    }
    const double slope = (b.y - a.y) / width;
    auto& start = knot_at(a.x);
    start.jump -= a.y;
    start.bend -= slope;
    auto& end = knot_at(b.x);
    end.jump += b.y;
    end.bend += slope;
  }
  std::vector<Complex> step(knots.size());
  std::vector<Complex> phasor(knots.size(), Complex(1.0, 0.0));
  for (std::size_t k = 0; k < knots.size(); ++k) {
    step[k] = std::polar(1.0, -two_pi * knots[k].x);
  }
  s[0] = Complex{};
  for (std::size_t h = 1; h <= harmonic_count; ++h) {
    const double w = two_pi * static_cast<double>(h);
    Complex jumps{};
    Complex bends{};
    for (std::size_t k = 0; k < knots.size(); ++k) {
      phasor[k] *= step[k];
      jumps += phasor[k] * knots[k].jump;
      bends += phasor[k] * knots[k].bend;
    }
    s[h] = 2.0 * (Complex(0.0, 1.0 / w) * jumps + bends / (w * w));
  }
}

[[nodiscard]] double rms(const Spectrum& s) noexcept {
  double energy = 0.0;
  for (std::size_t h = 1; h <= harmonic_count; ++h) {
    energy += std::norm(s[h]);
  }
  return std::sqrt(energy / 2.0);
}

// Scales a frame to a steady loudness, so additive morphs do not swell.
void normalise_rms(Spectrum& s, double target = 0.35) noexcept {
  const double level = rms(s);
  if (level > 1e-12) {
    for (auto& c : s) {
      c *= target / level;
    }
  }
}

void blend(const Spectrum& a, const Spectrum& b, double mix, Spectrum& out) {
  for (std::size_t h = 0; h <= harmonic_count; ++h) {
    out[h] = a[h] * (1.0 - mix) + b[h] * mix;
  }
}

// A path through `count` anchors at t: the anchor index and the mix to the next.
struct Segment {
  std::size_t from;
  double mix;
};
[[nodiscard]] Segment segment(double t, std::size_t count) noexcept {
  const double position = std::clamp(t, 0.0, 1.0) * static_cast<double>(count - 1);
  const auto from = std::min(count - 2, static_cast<std::size_t>(position));
  return {from, position - static_cast<double>(from)};
}

// Scratch for one table's build: time-domain cycles and their transform.
class Context {
public:
  Context() : cycle_(sample_length), bins_(sample_length), scratch_(harmonic_count + 1) {}

  // Samples wave(x), x in [0, 1), and takes its harmonics.
  template <typename Wave> void sample(Spectrum& s, Wave&& wave) {
    for (std::size_t n = 0; n < sample_length; ++n) {
      cycle_[n] = wave(static_cast<double>(n) / static_cast<double>(sample_length));
    }
    analyse(s);
  }
  // The cycle of a spectrum, peak-normalised, then shaped sample by sample
  // and analysed again (a resample through a waveshaper).
  template <typename Shape> void reshape(Spectrum& s, Shape&& shape) {
    synthesise(s);
    double peak = 1e-12;
    for (const double y : cycle_) {
      peak = std::max(peak, std::fabs(y));
    }
    for (auto& y : cycle_) {
      y = shape(y / peak);
    }
    analyse(s);
  }
  Spectrum& scratch() noexcept { return scratch_; }

private:
  static const dsp::OfflineFft& transform() {
    static const dsp::OfflineFft fft(sample_length);
    return fft;
  }
  void analyse(Spectrum& s) {
    for (std::size_t n = 0; n < sample_length; ++n) {
      bins_[n] = cycle_[n];
    }
    transform().forward(bins_);
    s[0] = Complex{};
    for (std::size_t h = 1; h <= harmonic_count; ++h) {
      s[h] = 2.0 * bins_[h] / static_cast<double>(sample_length);
    }
  }
  void synthesise(const Spectrum& s) {
    std::ranges::fill(bins_, Complex{});
    const double half = static_cast<double>(sample_length) / 2.0;
    for (std::size_t h = 1; h <= harmonic_count; ++h) {
      bins_[h] = s[h] * half;
      bins_[sample_length - h] = std::conj(bins_[h]);
    }
    transform().inverse(bins_);
    for (std::size_t n = 0; n < sample_length; ++n) {
      cycle_[n] = bins_[n].real();
    }
  }

  std::vector<double> cycle_;
  std::vector<Complex> bins_;
  Spectrum scratch_;
};

// ---- Classic shapes (exact) -----------------------------------------------------------

// Saw rising through zero at x = 0, jumping at the half cycle.
void saw_shape(Spectrum& s) {
  const std::array<Point, 4> points{{{0.0, 0.0}, {0.5, 1.0}, {0.5, -1.0}, {1.0, 0.0}}};
  piecewise(points, s);
}
void triangle_shape(Spectrum& s) {
  const std::array<Point, 4> points{{{0.0, 0.0}, {0.25, 1.0}, {0.75, -1.0}, {1.0, 0.0}}};
  piecewise(points, s);
}
void pulse_shape(double width, Spectrum& s) {
  const std::array<Point, 4> points{{{0.0, 1.0}, {width, 1.0}, {width, -1.0}, {1.0, -1.0}}};
  piecewise(points, s);
}
void sine_shape(Spectrum& s) {
  std::ranges::fill(s, Complex{});
  s[1] = sine_partial(1.0);
}

// ---- Vowels ----------------------------------------------------------------------------

constexpr std::size_t vowel_a = 0;
constexpr std::size_t vowel_e = 1;
constexpr std::size_t vowel_i = 2;
constexpr std::size_t vowel_o = 3;
constexpr std::size_t vowel_u = 4;

// Growl tables place their formants for a note at this pitch; played higher
// or lower they shift with it, as a resampled vocal does.
constexpr double growl_root_hz = 65.0;
// The singing tables sit an octave and a half higher, for leads and pads.
constexpr double voice_root_hz = 110.0;

[[nodiscard]] Vowel blend_vowels(const Vowel& from, const Vowel& to, double mix) noexcept {
  mix = std::clamp(mix, 0.0, 1.0);
  Vowel out{};
  for (std::size_t f = 0; f < out.size(); ++f) {
    out[f] = {from[f].hz * std::pow(to[f].hz / from[f].hz, mix),
              from[f].bandwidth * std::pow(to[f].bandwidth / from[f].bandwidth, mix),
              from[f].db + mix * (to[f].db - from[f].db)};
  }
  return out;
}

template <std::size_t N>
[[nodiscard]] Vowel vowel_path(const std::array<std::size_t, N>& path, double t) noexcept {
  const auto at = segment(t, N);
  return blend_vowels(vowel_table[path[at.from]], vowel_table[path[at.from + 1]], at.mix);
}

// A formant's level for synthesis: the sung levels compressed, plus +3 dB
// per octave, so a bright source talks instead of passing only the first.
[[nodiscard]] double formant_level(const Formant& formant) noexcept {
  return db_to_gain(0.4 * formant.db) * std::sqrt(formant.hz / 500.0);
}

// The magnitude of a vowel's five resonances at f Hz, plus a floor so the
// harmonics between formants are never fully lost.
[[nodiscard]] double vowel_gain(const Vowel& vowel, double f, double width, double floor) noexcept {
  double sum = 0.0;
  for (const auto& formant : vowel) {
    const double q = formant.hz / (formant.bandwidth * width);
    const double detune = f / formant.hz - formant.hz / f;
    sum += formant_level(formant) / std::sqrt(1.0 + q * q * detune * detune);
  }
  return floor + sum;
}

void apply_vowel(Spectrum& s, const Vowel& vowel, double root_hz, double width, double floor) {
  for (std::size_t h = 1; h <= harmonic_count; ++h) {
    s[h] *= vowel_gain(vowel, root_hz * static_cast<double>(h), width, floor);
  }
}

// ---- Generators ------------------------------------------------------------------------
// Each takes t in [0, 1] and writes one frame.

// Sine, triangle, saw and square crossfade in turn; the last quarter narrows
// the square to a 12.5 % pulse.
void basic_shapes(double t, Spectrum& s, Context& context) {
  if (t >= 0.75) {
    pulse_shape(0.5 - 0.375 * (t - 0.75) / 0.25, s);
    return;
  }
  const auto at = segment(t / 0.75, 4);
  const auto shape = [](std::size_t which, Spectrum& out) {
    switch (which) {
    case 0:
      sine_shape(out);
      return;
    case 1:
      triangle_shape(out);
      return;
    case 2:
      saw_shape(out);
      return;
    default:
      pulse_shape(0.5, out);
      return;
    }
  };
  shape(at.from, s);
  auto& next = context.scratch();
  shape(at.from + 1, next);
  blend(s, next, at.mix, s);
}

// A capacitor-charge saw with a finite reset: warm, curved and soft at the
// start, straightening into a sharp modern saw.
void analog_saw(double t, Spectrum& s, Context& context) {
  const double reset = 0.04 * std::pow(0.025, t); // 4 % of the cycle down to 0.1 %
  const double curve = 1.6 * (1.0 - t) + 0.02;
  const double norm = 1.0 - std::exp(-curve);
  context.sample(s, [&](double x) {
    const double rise = 1.0 - reset;
    if (x < rise) {
      return 2.0 * (1.0 - std::exp(-curve * x / rise)) / norm - 1.0;
    }
    return 1.0 - 2.0 * (x - rise) / reset;
  });
}

// PWM from square to a 3 % needle, with an analog pulse's tilted tops and
// short slewed edges.
void analog_pulse(double t, Spectrum& s, Context&) {
  const double width = 0.5 - 0.47 * smooth(t * 1.05);
  const double edge = 0.0035;
  const double droop_high = 0.18 * width;
  const double droop_low = 0.18 * (1.0 - width);
  const std::array<Point, 5> points{{{0.0, -1.0 + droop_low},
                                     {edge, 1.0},
                                     {width, 1.0 - droop_high},
                                     {width + edge, -1.0},
                                     {1.0, -1.0 + droop_low}}};
  piecewise(points, s);
}

// Saws stacked at the octave, twelfth and two octaves, growing in.
void saw_stack(double t, Spectrum& s, Context& context) {
  auto& base = context.scratch();
  saw_shape(base);
  std::ranges::fill(s, Complex{});
  const std::array<std::pair<std::size_t, double>, 5> stack{
      {{1, 1.0}, {2, 0.75 * t}, {3, 0.55 * t * t}, {4, 0.45 * t * t}, {6, 0.3 * t * t * t}}};
  for (const auto& [multiple, level] : stack) {
    for (std::size_t h = 1; h * multiple <= harmonic_count; ++h) {
      s[h * multiple] += level * base[h];
    }
  }
  normalise_rms(s);
}

// Three saws at moving phase offsets: scanning the position (an LFO is the
// usual source) sweeps the comb between them, the beating of a Reese. The
// offsets wrap, so the first and last frames meet.
void reese_saw(double t, Spectrum& s, Context& context) {
  auto& base = context.scratch();
  saw_shape(base);
  const double offset = t;
  const double second = 0.381966 * t + 0.11;
  for (std::size_t h = 1; h <= harmonic_count; ++h) {
    const double n = static_cast<double>(h);
    const Complex comb =
        1.0 + std::polar(0.9, two_pi * n * offset) + std::polar(0.55, two_pi * n * second);
    s[h] = base[h] * comb;
  }
  normalise_rms(s);
}

// Hard-sync: a slave saw at 1x to 8x reset by every master cycle.
void sync_saw(double t, Spectrum& s, Context&) {
  const double ratio = std::exp2(3.0 * t);
  std::vector<Point> points{{0.0, -1.0}};
  for (int step = 1;; ++step) {
    const auto k = static_cast<double>(step); // Exact: every k is a small whole number.
    if (!(k / ratio < 1.0))
      break;
    const double x = k / ratio;
    points.push_back({x, 1.0});
    points.push_back({x, -1.0});
  }
  // The slave's phase at the master's reset, in (0, 1].
  const double resets = std::ceil(ratio) - 1.0;
  points.push_back({1.0, 2.0 * (ratio - resets) - 1.0});
  piecewise(points, s);
}

// Hard-sync of a square, 1x to 8x.
void sync_square(double t, Spectrum& s, Context&) {
  const double ratio = std::exp2(3.0 * t);
  std::vector<Point> points{{0.0, 1.0}};
  double level = 1.0;
  for (int step = 1;; ++step) {
    const double k = 0.5 * static_cast<double>(step); // Exact halves, as summed before.
    if (!(k / ratio < 1.0))
      break;
    const double x = k / ratio;
    points.push_back({x, level});
    level = -level;
    points.push_back({x, level});
  }
  points.push_back({1.0, level});
  piecewise(points, s);
}

// Window sync: a sine at 1x to 12x under a smooth window, a sweeping
// resonance with no edge at the reset.
void window_sync(double t, Spectrum& s, Context& context) {
  const double ratio = 1.0 + 11.0 * t * t;
  context.sample(s, [&](double x) {
    const double window = std::sin(pi * x);
    return window * window * std::sin(two_pi * ratio * x);
  });
}

// Harmonics added one by one, 1 to 1024 on an exponential scale.
void harmonic_sweep(double t, Spectrum& s, Context&) {
  const double count = std::exp2(10.0 * t);
  std::ranges::fill(s, Complex{});
  for (std::size_t h = 1; h <= harmonic_count; ++h) {
    const double edge = std::clamp(count - static_cast<double>(h) + 1.0, 0.0, 1.0);
    if (edge > 0.0) {
      s[h] = sine_partial(edge / std::pow(static_cast<double>(h), 0.9));
    }
  }
  normalise_rms(s);
}

// Odd harmonics brightening, from a near-sine to a hollow, buzzing square.
void odd_sweep(double t, Spectrum& s, Context&) {
  const double tilt = 3.0 - 2.3 * t;
  std::ranges::fill(s, Complex{});
  for (std::size_t h = 1; h <= harmonic_count; h += 2) {
    s[h] = sine_partial(std::pow(static_cast<double>(h), -tilt));
  }
  normalise_rms(s);
}

// Organ drawbars 8', 4', 2 2/3', 2', 1 3/5', 1 1/3' and 1', through six
// registrations from a flute to full organ. Each drawbar step is 3 dB.
void organ_drawbars(double t, Spectrum& s, Context&) {
  constexpr std::array<std::size_t, 7> harmonics{1, 2, 3, 4, 5, 6, 8};
  constexpr std::array<std::array<double, 7>, 6> registrations{{{8, 0, 0, 0, 0, 0, 0},
                                                                {8, 6, 0, 0, 0, 0, 0},
                                                                {8, 8, 8, 0, 0, 0, 0},
                                                                {8, 6, 8, 5, 0, 3, 0},
                                                                {8, 8, 8, 8, 6, 6, 4},
                                                                {8, 8, 8, 8, 8, 8, 8}}};
  const auto at = segment(t, registrations.size());
  std::ranges::fill(s, Complex{});
  for (std::size_t bar = 0; bar < harmonics.size(); ++bar) {
    const double setting =
        registrations[at.from][bar] * (1.0 - at.mix) + registrations[at.from + 1][bar] * at.mix;
    if (setting > 0.0) {
      // Below one step the bar fades to silence linearly.
      const double level =
          db_to_gain(3.0 * (std::max(setting, 1.0) - 8.0)) * std::min(setting, 1.0);
      s[harmonics[bar]] += sine_partial(level);
    }
  }
  // A whisper of tonewheel leakage keeps it from sounding sterile.
  for (std::size_t h = 9; h <= 16; ++h) {
    s[h] += sine_partial(0.004 / static_cast<double>(h - 7));
  }
  normalise_rms(s);
}

// A plucked string: pluck position comb and damping from bright to dull.
void pluck_damp(double t, Spectrum& s, Context&) {
  const double position = 0.13 + 0.1 * t;
  const double damping = 0.002 + 0.12 * t * t;
  std::ranges::fill(s, Complex{});
  for (std::size_t h = 1; h <= harmonic_count; ++h) {
    const double n = static_cast<double>(h);
    s[h] = sine_partial(std::fabs(std::sin(pi * n * position)) * std::exp(-damping * (n - 1.0)) /
                        std::pow(n, 0.85));
  }
  normalise_rms(s);
}

// Lush additive pads: five seeded spectral colours, each a rolled-off
// series under three soft bumps, with fixed random phases (wide, never
// peaky). Moving through the colours changes only magnitudes.
void spectral_pad(double t, Spectrum& s, Context&) {
  constexpr std::size_t colours = 5;
  const auto at = segment(t, colours);
  const auto magnitude = [](std::size_t colour, std::size_t h) {
    Random random(1'000U + colour);
    const double lh = std::log2(static_cast<double>(h));
    double envelope = 0.08;
    for (int bump = 0; bump < 3; ++bump) {
      const double centre = 1.0 + 6.5 * random.uniform();
      const double width = 0.5 + 1.2 * random.uniform();
      const double gain = 0.4 + 0.6 * random.uniform();
      const double d = (lh - centre) / width;
      envelope += gain * std::exp(-d * d);
    }
    // Air above 4 kHz-ish harmonics grows towards the last colour.
    envelope += 0.25 * static_cast<double>(colour) / static_cast<double>(colours - 1) *
                smooth((lh - 5.5) / 2.0);
    Random sparkle(77U * h + colour);
    return envelope * (0.55 + 0.45 * sparkle.uniform()) / std::pow(static_cast<double>(h), 0.75);
  };
  s[0] = Complex{};
  for (std::size_t h = 1; h <= harmonic_count; ++h) {
    const double m = magnitude(at.from, h) * (1.0 - at.mix) + magnitude(at.from + 1, h) * at.mix;
    Random phase(9'001U + h);
    s[h] = h == 1 ? sine_partial(m) : std::polar(m, two_pi * phase.uniform());
  }
  normalise_rms(s);
}

// Glass: two narrow resonances a bell's 2.76 apart sweep up a soft series,
// over a faint shimmer of upper partials.
void glass_formant(double t, Spectrum& s, Context&) {
  const double centre = 3.0 * std::exp2(5.0 * t);
  constexpr double q = 14.0;
  std::ranges::fill(s, Complex{});
  for (std::size_t h = 1; h <= harmonic_count; ++h) {
    const double n = static_cast<double>(h);
    double gain = 0.05 + 0.04 * smooth((n - 24.0) / 64.0);
    for (const auto& [ratio, level] :
         std::array<std::pair<double, double>, 3>{{{1.0, 1.0}, {2.76, 0.7}, {5.40, 0.35}}}) {
      const double peak = centre * ratio;
      const double detune = n / peak - peak / n;
      gain += level / std::sqrt(1.0 + q * q * detune * detune);
    }
    s[h] = sine_partial(gain / std::sqrt(n) * (h % 2 == 0 ? 1.0 : -1.0));
  }
  normalise_rms(s);
}

// A saw singing A, E, I, O, U.
void vowel_morph(double t, Spectrum& s, Context&) {
  saw_shape(s);
  apply_vowel(s, vowel_path(std::array{vowel_a, vowel_e, vowel_i, vowel_o, vowel_u}, t),
              voice_root_hz, 1.0, 0.03);
  normalise_rms(s);
}

// Formant Yoi: a saw through U, O, A, E, I.
void formant_yoi(double t, Spectrum& s, Context&) {
  saw_shape(s);
  apply_vowel(s, vowel_path(std::array{vowel_u, vowel_o, vowel_a, vowel_e, vowel_i}, t),
              growl_root_hz, 1.3, 0.05);
  normalise_rms(s);
}

// Formant Square: a hollow saw-square through O, A, E.
void formant_square(double t, Spectrum& s, Context&) {
  saw_shape(s);
  for (std::size_t h = 2; h <= harmonic_count; h += 2) {
    s[h] *= 0.25;
  }
  apply_vowel(s, vowel_path(std::array{vowel_o, vowel_a, vowel_e}, t), growl_root_hz, 1.0, 0.04);
  normalise_rms(s);
}

// Talk Box: a saw through narrow vowels I, E, A, O, U.
void talk_box(double t, Spectrum& s, Context&) {
  saw_shape(s);
  apply_vowel(s, vowel_path(std::array{vowel_i, vowel_e, vowel_a, vowel_o, vowel_u}, t),
              growl_root_hz, 0.7, 0.02);
  normalise_rms(s);
}

// FM bell: a carrier at 2 under a modulator at 7 (a 3.5 ratio, so the
// partials clang), with a second, softer pair; the index rises.
void fm_bell(double t, Spectrum& s, Context& context) {
  const double index = 0.2 + 5.8 * t;
  context.sample(s, [&](double x) {
    return std::sin(two_pi * 2.0 * x + index * std::sin(two_pi * 7.0 * x)) +
           0.4 * std::sin(two_pi * 5.0 * x + 0.6 * index * std::sin(two_pi * 2.0 * x));
  });
}

// FM Growl: a sine modulated an octave up with a little feedback.
void fm_growl(double t, Spectrum& s, Context& context) {
  const double index = 0.3 + 6.0 * t;
  context.sample(s, [&](double x) {
    const double modulator = std::sin(two_pi * 2.0 * x + 0.6 * t * std::sin(two_pi * 2.0 * x));
    return std::tanh(1.5 * std::sin(two_pi * x + index * modulator));
  });
}

// FM Bark: a saw phase-modulated at three times its pitch, clipped.
void fm_bark(double t, Spectrum& s, Context& context) {
  const double index = 3.0 * t;
  context.sample(s, [&](double x) {
    return std::clamp(saw(x + index / two_pi * std::sin(two_pi * 3.0 * x)) / 0.6, -1.0, 1.0);
  });
}

// Metallic: plate-mode ratios placed high in the series (so they relate
// inharmonically to each other) over a quiet fundamental; the modes spread
// and brighten. A fractional mode is shared between its two neighbours.
void metal_clang(double t, Spectrum& s, Context&) {
  constexpr std::array<double, 10> modes{1.0, 1.59, 2.14, 2.30, 2.65, 2.92, 3.16, 3.50, 4.15, 4.60};
  const double base = 12.0;
  const double stretch = 1.0 + 0.5 * t;
  std::ranges::fill(s, Complex{});
  s[1] = sine_partial(0.3);
  s[2] = sine_partial(0.1);
  for (std::size_t m = 0; m < modes.size(); ++m) {
    const double position = base * std::pow(modes[m], stretch);
    const auto low = static_cast<std::size_t>(position);
    if (low + 1 > harmonic_count) {
      continue;
    }
    const double mix = position - static_cast<double>(low);
    const double level =
        std::exp(-static_cast<double>(m) * (0.45 - 0.35 * t)) * (m % 2 == 0 ? 1.0 : 0.8);
    Random phase(500U + m);
    const Complex partial = std::polar(level, two_pi * phase.uniform());
    s[low] += partial * std::sqrt(1.0 - mix);
    s[low + 1] += partial * std::sqrt(mix);
  }
  normalise_rms(s);
}

// Casio-style phase distortion of a cosine: the knee moves, sine to saw.
void cz_saw(double t, Spectrum& s, Context& context) {
  const double knee = 0.5 - 0.47 * t;
  context.sample(s, [&](double x) {
    const double p = x < knee ? 0.5 * x / knee : 0.5 + 0.5 * (x - knee) / (1.0 - knee);
    return -std::cos(two_pi * p);
  });
}

// CZ Resonance: a decaying window over a resonance rising 1x to 16x.
void cz_resonance(double t, Spectrum& s, Context& context) {
  const double ratio = 1.0 + 15.0 * t;
  context.sample(s, [&](double x) { return (1.0 - x) * std::cos(two_pi * ratio * x); });
}

// Sine and saw folded harder, with an even-harmonic bias and soft grit.
void grit_fold(double t, Spectrum& s, Context& context) {
  const double gain = 1.0 + 11.0 * t * t;
  const double bias = 0.25 * t;
  context.sample(s, [&](double x) {
    const double folded =
        std::sin(0.5 * pi * ((0.8 * std::sin(two_pi * x) + 0.35 * saw(x + 0.5)) * gain + bias));
    return std::tanh(1.6 * folded) / std::tanh(1.6);
  });
}

// A west-coast folder: a triangle through a sine fold, harder and
// asymmetric. The triangle has no edges, so the folds stay clean.
void west_fold(double t, Spectrum& s, Context& context) {
  const double gain = 1.0 + 7.0 * t;
  const double symmetry = 0.15 * t;
  context.sample(s, [&](double x) {
    const double triangle = 1.0 - 4.0 * std::fabs(frac(x + 0.25) - 0.5);
    return std::sin(0.5 * pi * (gain * triangle + symmetry));
  });
}

// Metal Fold: sine and saw folded harder, with a bias for evens.
void metal_fold(double t, Spectrum& s, Context& context) {
  const double gain = 1.0 + 9.0 * t;
  const double bias = 0.3 * t;
  context.sample(s, [&](double x) {
    return std::sin(0.5 * pi * ((0.7 * std::sin(two_pi * x) + 0.3 * saw(x)) * gain + bias));
  });
}

// Hollow Comb: a saw with notches moving down the spectrum.
void comb_hollow(double t, Spectrum& s, Context&) {
  saw_shape(s);
  const double spacing = 0.03 + 0.22 * t;
  for (std::size_t h = 1; h <= harmonic_count; ++h) {
    s[h] *= 0.5 + 0.5 * std::cos(two_pi * spacing * static_cast<double>(h));
  }
  normalise_rms(s);
}

// Growl Saw: a saw phase-modulated by its own pitch, then driven.
void growl_saw(double t, Spectrum& s, Context& context) {
  const double index = 2.5 * t;
  const double drive = 1.0 + 3.0 * t;
  context.sample(s, [&](double x) {
    return std::tanh(drive * saw(x + index / two_pi * std::sin(two_pi * x))) / std::tanh(drive);
  });
}

// Resample Grind: an FM saw, folded, through a vowel and clipped,
// as a growl bounced through a chain and back.
void resample_grind(double t, Spectrum& s, Context& context) {
  const double fold = 1.0 + 5.0 * t;
  context.sample(s, [&](double x) {
    return std::sin(0.5 * pi * fold * saw(x + 0.25 * t * std::sin(two_pi * 2.0 * x)));
  });
  apply_vowel(s, vowel_path(std::array{vowel_a, vowel_o}, t), growl_root_hz, 1.2, 0.3);
  context.reshape(s, [](double y) { return std::tanh(2.5 * y); });
}

// Roar: dense, rough partials through vowels A, O, U.
void roar(double t, Spectrum& s, Context&) {
  const Vowel vowel = vowel_path(std::array{vowel_a, vowel_o, vowel_u}, t);
  std::ranges::fill(s, Complex{});
  for (std::size_t h = 1; h <= 400; ++h) {
    Random random(h * 2'654'435'761ULL + 97U);
    const double amplitude =
        (0.6 + 0.4 * random.bipolar()) / std::pow(static_cast<double>(h), 0.75);
    const double phase = pi * random.bipolar();
    s[h] = std::polar(
        amplitude * vowel_gain(vowel, growl_root_hz * static_cast<double>(h), 1.6, 0.15), phase);
  }
  normalise_rms(s);
}

// Diode Saw: a saw clipped harder on one side.
void diode_saw(double t, Spectrum& s, Context& context) {
  const double gain = 1.0 + 8.0 * t;
  context.sample(s, [&](double x) {
    const double y = gain * saw(x);
    return y > 0.0 ? std::tanh(y) : 0.45 * std::tanh(2.2 * y);
  });
}

// Wub Sine: a sine filling out towards a soft square.
void wub_sine(double t, Spectrum& s, Context& context) {
  const double gain = 1.0 + 5.0 * t;
  context.sample(s, [&](double x) {
    const double y = std::sin(two_pi * x) + 0.25 * t * std::sin(2.0 * two_pi * x);
    return std::tanh(gain * y) / std::tanh(gain);
  });
}

// Bit Grit: a saw losing bits and in-cycle sample rate. The steps
// are exact (piecewise constant), so the only harshness is the intended one.
void bit_grit(double t, Spectrum& s, Context&) {
  const double levels = std::round(std::exp2(7.0 - 5.0 * t));
  const auto steps = static_cast<std::size_t>(std::round(512.0 * std::exp2(-5.0 * t)));
  std::vector<Point> points;
  points.reserve(2 * steps);
  for (std::size_t k = 0; k < steps; ++k) {
    const double x0 = static_cast<double>(k) / static_cast<double>(steps);
    const double x1 = static_cast<double>(k + 1) / static_cast<double>(steps);
    const double y = std::round(saw(x0) * levels) / levels;
    points.push_back({x0, y});
    points.push_back({x1, y});
  }
  piecewise(points, s);
}

// Console chip voices: 12.5 %, 25 % and 50 % pulses, the 4-bit stepped
// triangle and a 4-bit saw.
void chiptune(double t, Spectrum& s, Context& context) {
  const auto stepped = [](auto&& wave, std::size_t steps, double levels, Spectrum& out) {
    std::vector<Point> points;
    points.reserve(2 * steps);
    for (std::size_t k = 0; k < steps; ++k) {
      const double x0 = static_cast<double>(k) / static_cast<double>(steps);
      const double x1 = static_cast<double>(k + 1) / static_cast<double>(steps);
      const double y = std::round(wave(x0) * levels) / levels;
      points.push_back({x0, y});
      points.push_back({x1, y});
    }
    piecewise(points, out);
  };
  const auto shape = [&](std::size_t which, Spectrum& out) {
    switch (which) {
    case 0:
      pulse_shape(0.125, out);
      return;
    case 1:
      pulse_shape(0.25, out);
      return;
    case 2:
      pulse_shape(0.5, out);
      return;
    case 3:
      stepped([](double x) { return 1.0 - 4.0 * std::fabs(frac(x + 0.25) - 0.5); }, 32, 7.5, out);
      return;
    default:
      stepped([](double x) { return saw(x + 0.5); }, 16, 7.5, out);
      return;
    }
  };
  const auto at = segment(t, 5);
  shape(at.from, s);
  auto& next = context.scratch();
  shape(at.from + 1, next);
  blend(s, next, at.mix, s);
}

// Noise bands: dense random-phase partials under a band that sweeps from
// the bass up the series; played low, it is pitched noise.
void noise_bands(double t, Spectrum& s, Context&) {
  const double centre = std::log2(3.0) + (std::log2(500.0) - std::log2(3.0)) * t;
  constexpr double width = 1.1; // octaves
  s[0] = Complex{};
  for (std::size_t h = 1; h <= harmonic_count; ++h) {
    Random random(31U * h + 7U);
    const double d = (std::log2(static_cast<double>(h)) - centre) / width;
    const double magnitude = (0.02 + std::exp(-d * d)) * (0.3 + 0.7 * random.uniform());
    s[h] = std::polar(magnitude, two_pi * random.uniform());
  }
  normalise_rms(s);
}

// ---- Registry --------------------------------------------------------------------------

using Generator = void (*)(double, Spectrum&, Context&);
struct Factory {
  std::string_view name;
  std::size_t frames;
  Generator generate;
};

// Sweeps whose shape changes fast across position get the full 256 frames;
// smooth spectral morphs need fewer (the spectral variant fills them in).
constexpr std::array<Factory, 36> factories{{
    {"basic_shapes", 128, basic_shapes},
    {"analog_saw", 64, analog_saw},
    {"analog_pulse", 256, analog_pulse},
    {"saw_stack", 64, saw_stack},
    {"reese_saw", 256, reese_saw},
    {"sync_saw", 256, sync_saw},
    {"sync_square", 256, sync_square},
    {"window_sync", 128, window_sync},
    {"harmonic_sweep", 128, harmonic_sweep},
    {"odd_sweep", 64, odd_sweep},
    {"organ_drawbars", 64, organ_drawbars},
    {"pluck_damp", 64, pluck_damp},
    {"spectral_pad", 64, spectral_pad},
    {"glass_formant", 128, glass_formant},
    {"vowel_morph", 128, vowel_morph},
    {"formant_yoi", 128, formant_yoi},
    {"formant_square", 64, formant_square},
    {"talk_box", 128, talk_box},
    {"fm_bell", 128, fm_bell},
    {"fm_growl", 128, fm_growl},
    {"fm_bark", 128, fm_bark},
    {"metal_clang", 64, metal_clang},
    {"cz_saw", 64, cz_saw},
    {"cz_resonance", 128, cz_resonance},
    {"grit_fold", 256, grit_fold},
    {"west_fold", 128, west_fold},
    {"metal_fold", 128, metal_fold},
    {"comb_hollow", 64, comb_hollow},
    {"growl_saw", 64, growl_saw},
    {"resample_grind", 128, resample_grind},
    {"roar", 64, roar},
    {"diode_saw", 64, diode_saw},
    {"wub_sine", 64, wub_sine},
    {"bit_grit", 128, bit_grit},
    {"chiptune", 128, chiptune},
    {"noise_bands", 64, noise_bands},
}};

constexpr auto names = [] {
  std::array<std::string_view, factories.size()> out{};
  for (std::size_t i = 0; i < factories.size(); ++i) {
    out[i] = factories[i].name;
  }
  return out;
}();

std::shared_ptr<const Wavetable> build(const Factory& factory) {
  Context context;
  Spectrum spectrum(harmonic_count + 1);
  std::vector<std::vector<float>> magnitudes(factory.frames, std::vector<float>(harmonic_count));
  std::vector<std::vector<float>> phases(factory.frames, std::vector<float>(harmonic_count));
  for (std::size_t frame = 0; frame < factory.frames; ++frame) {
    const double t = factory.frames > 1
                         ? static_cast<double>(frame) / static_cast<double>(factory.frames - 1)
                         : 0.0;
    factory.generate(t, spectrum, context);
    for (std::size_t h = 1; h <= harmonic_count; ++h) {
      // Back to the public sine-phase form: c = m e^(i (phase - pi/2)).
      magnitudes[frame][h - 1] = static_cast<float>(std::abs(spectrum[h]));
      phases[frame][h - 1] = static_cast<float>(std::arg(spectrum[h]) + 0.5 * pi);
    }
  }
  auto table = build_wavetable_from_spectra(magnitudes, phases, std::string(factory.name));
  return table ? std::move(table).value() : nullptr;
}

struct Cache {
  std::array<std::once_flag, factories.size()> once;
  std::array<std::shared_ptr<const Wavetable>, factories.size()> tables;
};

Cache& cache() {
  static Cache instance;
  return instance;
}

} // namespace

std::span<const std::string_view> builtin_table_names() noexcept { return names; }

std::shared_ptr<const Wavetable> builtin_table(std::string_view name) {
  const auto found = std::ranges::find(names, name);
  if (found == names.end()) {
    return nullptr;
  }
  const auto index = static_cast<std::size_t>(found - names.begin());
  auto& c = cache();
  std::call_once(c.once[index], [&] { c.tables[index] = build(factories[index]); });
  return c.tables[index];
}

} // namespace oscillate::native_dsp::synth::nova

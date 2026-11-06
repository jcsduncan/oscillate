#include "oscillate/native_dsp/synth/wavetable.hpp"

#include "oscillate/native_dsp/synth/shaping.hpp"

#include "dsp/fft.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numbers>
#include <utility>
#include <vector>

namespace oscillate::native_dsp::synth {
namespace {

using Complex = std::complex<double>;
constexpr double pi = std::numbers::pi;
constexpr std::size_t generation_length = wavetable_cycle_length;

constexpr std::array<std::string_view, wavetable_count> names{
    "Basic Shapes",  "PWM Sweep",     "Harmonic Series", "Odd Harmonics",
    "Vowel",         "Sync Sweep",    "FM Sweep",        "Phase Distortion",
    "Bit Crush",     "Noise Spectra", "Bell Partials",   "Organ Drawbars",
    "Digital Stack", "Hollow",        "Pluck Decay",     "Saw Fold"};

using Spectrum = WavetableSpectrum;

Complex sine_partial(double amplitude) { return Complex(0.0, -amplitude); }

void from_cycle(const std::vector<double>& cycle, Spectrum& spectrum) {
  cycle_spectrum(cycle, spectrum);
}

double saw(double x) { return 2.0 * (x - std::floor(x)) - 1.0; }

// The classic shapes, in Basic Shapes' morph order.
enum class Basic : std::uint8_t { sine, triangle, saw, square };

// Spectra of the classic shapes, exactly band-limited by construction.
void basic_spectrum(Basic shape, Spectrum& s) {
  for (std::size_t h = 1; h <= wavetable_max_harmonic; ++h) {
    const double n = static_cast<double>(h);
    switch (shape) {
    case Basic::sine:
      s[h] = h == 1 ? sine_partial(1.0) : 0.0;
      break;
    case Basic::triangle: // Odd harmonics, 1/n^2, alternating.
      s[h] = h % 2 == 1
                 ? sine_partial(8.0 / (pi * pi) / (n * n) * (((h - 1) / 2) % 2 == 0 ? 1 : -1))
                 : 0.0;
      break;
    case Basic::saw:
      s[h] = sine_partial(-2.0 / (pi * n) * (h % 2 == 1 ? -1.0 : 1.0));
      break;
    case Basic::square:
      s[h] = h % 2 == 1 ? sine_partial(4.0 / (pi * n)) : 0.0;
      break;
    }
  }
}

double bump(double x, double centre, double width) {
  const double d = (x - centre) / width;
  return std::exp(-d * d);
}

// A morph position t in [0, 1] across `segments` equal segments: the one it
// is in, and how far through (the top end stays in the last segment).
struct Segment {
  std::size_t from = 0;
  double mix = 0.0;
};
constexpr double segment_margin = 1e-6;
[[nodiscard]] Segment segment_of(double t, double segments) noexcept {
  const double segment = std::min(segments - segment_margin, t * segments);
  const auto from = static_cast<std::size_t>(segment);
  return {from, segment - static_cast<double>(from)};
}

// A frame built in the time domain: `wave` over one cycle, x in [0, 1).
template <typename Wave> void from_wave(Wave&& wave, Spectrum& s, std::vector<double>& cycle) {
  for (std::size_t n = 0; n < generation_length; ++n) {
    cycle[n] = wave(static_cast<double>(n) / generation_length);
  }
  from_cycle(cycle, s);
}

// ---- The tables, in names' order. Each writes one frame at morph t. -------------------------

// Sine -> triangle -> saw -> square.
void basic_shapes(double t, Spectrum& s, std::vector<double>&) {
  constexpr double segments = 3.0;
  const Segment at = segment_of(t, segments);
  Spectrum a{};
  Spectrum b{};
  basic_spectrum(static_cast<Basic>(at.from), a);
  basic_spectrum(static_cast<Basic>(at.from + 1), b);
  for (std::size_t h = 1; h <= wavetable_max_harmonic; ++h) {
    s[h] = a[h] * (1.0 - at.mix) + b[h] * at.mix;
  }
}

// Pulse width from square to a thin needle.
constexpr double widest_pulse = 0.5;
constexpr double pulse_narrowing = 0.47;
void pwm_sweep(double t, Spectrum& s, std::vector<double>& cycle) {
  const double width = widest_pulse - pulse_narrowing * t;
  from_wave([&](double x) { return (x < width ? 1.0 : -1.0) - (2.0 * width - 1.0); }, s, cycle);
}

// Additive build-up: 1 to 64 harmonics with a soft top edge.
constexpr std::size_t series_harmonics = 64;
void harmonic_series(double t, Spectrum& s, std::vector<double>&) {
  const double count = 1.0 + static_cast<double>(series_harmonics - 1) * t;
  for (std::size_t h = 1; h <= series_harmonics; ++h) {
    const double edge = std::clamp(count - static_cast<double>(h) + 1.0, 0.0, 1.0);
    s[h] = sine_partial(edge / static_cast<double>(h));
  }
}

// Odd harmonics, brightening: their roll-off falls from 1/n^2.2 to 1/n^0.8.
constexpr double odd_tilt_dark = 2.2;
constexpr double odd_tilt_span = 1.4;
void odd_harmonics(double t, Spectrum& s, std::vector<double>&) {
  const double tilt = odd_tilt_dark - odd_tilt_span * t;
  for (std::size_t h = 1; h <= wavetable_max_harmonic; h += 2) {
    s[h] = sine_partial(std::pow(static_cast<double>(h), -tilt));
  }
}

// Vowels A-E-I-O-U on a 110 Hz saw source: three formants each, the first
// strongest, over a small floor.
constexpr std::array<std::array<double, 3>, 5> vowel_formants{
    {{800, 1150, 2900}, {400, 1600, 2700}, {270, 2300, 3000}, {450, 800, 2830}, {325, 700, 2530}}};
constexpr double vowel_source_hz = 110.0;
constexpr std::size_t vowel_harmonics = 200;
constexpr double vowel_floor = 0.02;
constexpr double vowel_upper_formants = 0.6;
constexpr double vowel_width_hz = 90.0;
constexpr double vowel_width_share = 0.04;
void vowel(double t, Spectrum& s, std::vector<double>&) {
  const Segment at = segment_of(t, static_cast<double>(vowel_formants.size() - 1));
  for (std::size_t h = 1; h <= vowel_harmonics; ++h) {
    const double hz = vowel_source_hz * static_cast<double>(h);
    double envelope = vowel_floor;
    for (std::size_t f = 0; f < vowel_formants[0].size(); ++f) {
      const double centre =
          vowel_formants[at.from][f] * (1.0 - at.mix) + vowel_formants[at.from + 1][f] * at.mix;
      envelope += (f == 0 ? 1.0 : vowel_upper_formants) *
                  bump(hz, centre, vowel_width_hz + vowel_width_share * centre);
    }
    s[h] = sine_partial(envelope / std::sqrt(static_cast<double>(h)));
  }
}

// Hard-sync sweep of a saw, up to 8 times the master.
constexpr double sync_ratio_span = 7.0;
void sync_sweep(double t, Spectrum& s, std::vector<double>& cycle) {
  const double ratio = 1.0 + sync_ratio_span * t;
  from_wave([&](double x) { return saw(x * ratio); }, s, cycle);
}

// Two-operator FM, index rising, 1:2 ratio.
constexpr double fm_max_index = 5.0;
void fm_sweep(double t, Spectrum& s, std::vector<double>& cycle) {
  const double index = fm_max_index * t;
  from_wave([&](double x) { return std::sin(2.0 * pi * x + index * std::sin(4.0 * pi * x)); }, s,
            cycle);
}

// Phase distortion of a cosine, CZ style: the knee moves from the middle.
void phase_distortion(double t, Spectrum& s, std::vector<double>& cycle) {
  const double knee = widest_pulse - pulse_narrowing * t;
  from_wave(
      [&](double x) {
        const double p = x < knee ? 0.5 * x / knee : 0.5 + 0.5 * (x - knee) / (1.0 - knee);
        return -std::cos(2.0 * pi * p);
      },
      s, cycle);
}

// Sine through a bit crusher: 2^7 + 1 levels down to 2.
constexpr double crush_bits = 7.0;
void bit_crush(double t, Spectrum& s, std::vector<double>& cycle) {
  const double levels = std::round(std::pow(2.0, crush_bits * (1.0 - t))) + 1.0;
  from_wave([&](double x) { return std::round(std::sin(2.0 * pi * x) * levels) / levels; }, s,
            cycle);
}

// Seeded noise spectra morphing between four fixed sets, rolling off as
// 1/n^0.8.
constexpr std::size_t noise_sets = 4;
constexpr std::size_t noise_harmonics = 96;
constexpr std::uint32_t noise_harmonic_prime = 7'919U;
constexpr std::uint32_t noise_set_prime = 104'729U;
constexpr double noise_tilt = 0.8;
void noise_spectra(double t, Spectrum& s, std::vector<double>&) {
  const Segment at = segment_of(t, static_cast<double>(noise_sets - 1));
  const auto from = static_cast<std::uint32_t>(at.from);
  const double mix = at.mix;
  for (std::size_t h = 1; h <= noise_harmonics; ++h) {
    double amplitude[2];
    double phase[2];
    for (std::uint32_t k = 0; k < 2; ++k) {
      NoiseSource noise;
      noise.reset(
          static_cast<std::uint32_t>(h * noise_harmonic_prime + (from + k) * noise_set_prime + 1U));
      amplitude[k] = (0.5 + 0.5 * noise.white()) / std::pow(static_cast<double>(h), noise_tilt);
      phase[k] = pi * noise.white();
    }
    const double a = amplitude[0] * (1.0 - mix) + amplitude[1] * mix;
    s[h] = std::polar(a, phase[0] * (1.0 - mix) + phase[1] * mix);
  }
}

// Bell-like partial clusters on harmonic positions, the upper ones ringing
// longer as t rises.
constexpr std::array<std::size_t, 8> bell_partials{1, 2, 3, 5, 7, 9, 11, 14};
constexpr double bell_decay_dull = 1.2;
constexpr double bell_decay_span = 1.0;
constexpr double bell_upper_level = 0.7;
void bell_partials_table(double t, Spectrum& s, std::vector<double>&) {
  for (std::size_t i = 0; i < bell_partials.size(); ++i) {
    const double weight =
        std::exp(-static_cast<double>(i) * (bell_decay_dull - bell_decay_span * t));
    s[bell_partials[i]] = sine_partial(weight * (i == 0 ? 1.0 : bell_upper_level));
  }
}

// Organ drawbar registrations.
constexpr std::array<std::size_t, 7> drawbars{1, 2, 3, 4, 6, 8, 10};
constexpr std::array<std::array<double, 7>, 3> registrations{
    {{1, 0.8, 0.6, 0.3, 0, 0, 0}, {1, 0.8, 0.8, 0.8, 0.6, 0.5, 0}, {1, 1, 1, 1, 1, 1, 1}}};
constexpr double drawbar_level = 0.6;
void organ_drawbars(double t, Spectrum& s, std::vector<double>&) {
  const Segment at = segment_of(t, static_cast<double>(registrations.size() - 1));
  for (std::size_t i = 0; i < drawbars.size(); ++i) {
    const double level =
        registrations[at.from][i] * (1.0 - at.mix) + registrations[at.from + 1][i] * at.mix;
    s[drawbars[i]] = sine_partial(level * drawbar_level);
  }
}

// Saws stacked at 1x, 2x and 3x the fundamental.
constexpr double stack_second = 0.7;
constexpr double stack_third = 0.5;
void digital_stack(double t, Spectrum& s, std::vector<double>& cycle) {
  from_wave(
      [&](double x) {
        return saw(x) + stack_second * t * saw(2.0 * x) + stack_third * t * t * saw(3.0 * x);
      },
      s, cycle);
}

// Hollow: odd-weighted square, even harmonics fading in and out (every
// sixth one silent).
constexpr double hollow_even_period = 6.0;
void hollow(double t, Spectrum& s, std::vector<double>&) {
  for (std::size_t h = 1; h <= wavetable_max_harmonic; ++h) {
    const double even =
        h % 2 == 0 ? t * std::fabs(std::sin(pi * static_cast<double>(h) / hollow_even_period))
                   : 1.0;
    s[h] = sine_partial(even / static_cast<double>(h));
  }
}

// Pluck: bright to dull, high harmonics decaying away.
constexpr double pluck_decay_min = 0.002;
constexpr double pluck_decay_span = 0.25;
void pluck_decay(double t, Spectrum& s, std::vector<double>&) {
  const double decay = pluck_decay_min + pluck_decay_span * t;
  for (std::size_t h = 1; h <= wavetable_max_harmonic; ++h) {
    s[h] = sine_partial(std::exp(-decay * static_cast<double>(h - 1)) / static_cast<double>(h));
  }
}

// Saw through a wavefolder, folding harder (up to 6 times the gain).
constexpr double fold_gain_span = 5.0;
void saw_fold(double t, Spectrum& s, std::vector<double>& cycle) {
  const double gain = 1.0 + fold_gain_span * t;
  from_wave([&](double x) { return std::sin(0.5 * pi * gain * saw(x)); }, s, cycle);
}

using Generator = void (*)(double t, Spectrum& s, std::vector<double>& cycle);
// In names' order.
constexpr std::array<Generator, wavetable_count> generators{&basic_shapes,
                                                            &pwm_sweep,
                                                            &harmonic_series,
                                                            &odd_harmonics,
                                                            &vowel,
                                                            &sync_sweep,
                                                            &fm_sweep,
                                                            &phase_distortion,
                                                            &bit_crush,
                                                            &noise_spectra,
                                                            &bell_partials_table,
                                                            &organ_drawbars,
                                                            &digital_stack,
                                                            &hollow,
                                                            &pluck_decay,
                                                            &saw_fold};

// One frame of table `table` at morph position t in [0, 1].
void generate(std::size_t table, double t, Spectrum& s, std::vector<double>& cycle) {
  s.fill(0.0);
  generators[std::min(table, generators.size() - 1)](t, s, cycle);
}

std::array<Wavetable, wavetable_count> build() {
  std::array<Wavetable, wavetable_count> tables{};
  std::vector<double> cycle(generation_length);
  for (std::size_t table = 0; table < wavetable_count; ++table) {
    tables[table] = build_wavetable(
        names[table], [&](double t, Spectrum& spectrum) { generate(table, t, spectrum, cycle); });
  }
  return tables;
}

} // namespace

void cycle_spectrum(std::span<const double> cycle, WavetableSpectrum& spectrum) {
  std::vector<Complex> data(cycle.begin(), cycle.end());
  fft(data, false);
  spectrum[0] = 0.0;
  for (std::size_t h = 1; h <= wavetable_max_harmonic; ++h) {
    spectrum[h] = 2.0 * data[h] / static_cast<double>(cycle.size());
  }
}

Wavetable build_wavetable(std::string_view name,
                          const std::function<void(double, WavetableSpectrum&)>& generate) {
  Wavetable out;
  out.name = name;
  for (std::size_t level = 0; level < wavetable_levels; ++level) {
    auto& l = out.levels[level];
    l.harmonics = level + 1 == wavetable_levels ? 1U : wavetable_level_harmonics(level);
    l.length = std::clamp<std::uint32_t>(4U * l.harmonics, 256U, 2048U);
    l.data.assign(wavetable_frames * (l.length + 3), 0.0F);
  }
  Spectrum spectrum{};
  for (std::size_t frame = 0; frame < wavetable_frames; ++frame) {
    const double t = static_cast<double>(frame) / (wavetable_frames - 1);
    generate(t, spectrum);
    double scale = 1.0;
    for (std::size_t level = 0; level < wavetable_levels; ++level) {
      auto& l = out.levels[level];
      std::vector<Complex> bins(l.length, 0.0);
      for (std::size_t h = 1; h <= l.harmonics; ++h) {
        bins[h] = spectrum[h] * (static_cast<double>(l.length) / 2.0);
        bins[l.length - h] = std::conj(bins[h]);
      }
      fft(bins, true);
      if (level == 0) {
        // Peak-normalise each frame once, from its fullest band, so
        // morphing keeps a steady level and every level stays aligned.
        double peak = 0.0;
        for (const auto& value : bins) {
          peak = std::max(peak, std::fabs(value.real()));
        }
        scale = peak > 1e-12 ? 0.95 / peak : 1.0;
      }
      float* destination = l.data.data() + frame * (l.length + 3) + 1;
      for (std::size_t n = 0; n < l.length; ++n) {
        destination[n] = static_cast<float>(bins[n].real() * scale);
      }
      destination[-1] = destination[l.length - 1];
      destination[l.length] = destination[0];
      destination[l.length + 1] = destination[1];
    }
  }
  return out;
}

void fft(std::span<std::complex<double>> data, bool inverse) {
  const dsp::Fft transform(data.size());
  if (inverse) {
    transform.inverse(data);
  } else {
    transform.forward(data);
  }
}

std::size_t wavetable_level_for(double max_harmonic) noexcept {
  for (std::size_t level = 0; level + 1 < wavetable_levels; ++level) {
    if (static_cast<double>(wavetable_level_harmonics(level)) <= max_harmonic) {
      return level;
    }
  }
  return wavetable_levels - 1;
}

const std::array<Wavetable, wavetable_count>& wavetables() {
  static const std::array<Wavetable, wavetable_count> tables = build();
  return tables;
}

std::span<const std::string_view> wavetable_names() noexcept { return names; }

} // namespace oscillate::native_dsp::synth

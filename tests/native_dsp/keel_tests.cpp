#include "effect_test_support.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/keel.hpp"
#include "oscillate/native_dsp/loudness.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

// Keel (ADR-0119) against measured oracles: pink noise tilted 3 dB/oct
// either way converges to the target within a stated tolerance and time
// for each Speed; Amount 0 is bit for bit; a steady programme's correction
// holds still and a moving one glides without zipper sidebands; the curve,
// reference and learned targets are reached; regions, limits, absent
// bands, the gates, level match, delta and output; block splits, export
// against live, hostile input, the tail, restrained presets, a seek, an
// allocation-free callback and the panel's telemetry.
namespace {
using namespace effect_test;
namespace keel = oscillate::native_dsp::effects::keel;
using oscillate::native_dsp::dsp::db_to_gain;
using oscillate::native_dsp::dsp::gain_to_db;
using oscillate::native_dsp::dsp::power_to_db;

const effects::EffectEngine& engine() { return keel::engine(); }
const EngineDescriptor& table() { return *engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr std::size_t parameter_count = 45;
constexpr std::size_t preset_count = 6;
constexpr double target_curve = 1.0;
constexpr double target_reference = 2.0;
constexpr double on = 1.0;
constexpr double full = 1.0;

// Programme material: pink noise at -20 dBFS RMS, a fresh period every
// 2^20 frames (21.8 s).
constexpr double programme_rms = 0.1;
constexpr std::size_t pink_period = 1U << 20U;
constexpr double tilt_db = 3.0;
// The analysis's own frames at 48 kHz: the telemetry is read once a hop.
constexpr std::size_t hop = 2'048;
// The balance is judged from 40 Hz to 16 kHz, the third-octave bands
// with the content and the filters to steer them.
constexpr double judged_low_hz = 40.0;
constexpr double judged_high_hz = 16'000.0;
// Converged: every judged band within this of the target (the bank's fit
// of a 3 dB/oct tilt and the estimate's own spread).
constexpr double converged_db = 1.0;
// Speeds measured, in seconds.
constexpr std::array speeds{1.0, 4.0, 15.0};
// The full correction the tilt asks for stays inside the limits.
constexpr double wide_limit_db = 18.0;
// Welch analysis of rendered output.
constexpr std::size_t welch_window = 16'384;
constexpr std::size_t welch_hop = 8'192;

std::vector<double> defaults() { return preset(table(), "Default"); }

std::size_t frames(double seconds, double at_rate = rate) {
  return static_cast<std::size_t>(seconds * at_rate);
}

std::vector<double> with(std::vector<double> values, std::string_view id, double value) {
  values[at(id)] = value;
  return values;
}

double rounded(double x) { return static_cast<double>(static_cast<float>(x)); }

std::size_t band_of(double hz) {
  return static_cast<std::size_t>(
      std::lround(std::log2(hz / keel::reference_hz) * keel::bands_per_octave) +
      static_cast<long>(keel::reference_band));
}
// The judged bands, first and last.
std::size_t judged_first() { return band_of(judged_low_hz); }
std::size_t judged_last() { return band_of(judged_high_hz); }

// Pink noise tilted `tilt` dB/oct about 1 kHz (plus any `curve`, dB per
// third-octave band, read between band centres), at programme_rms,
// independent per channel: random phases on a 1/f power spectrum, a fresh
// period each pink_period frames.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named at every call.
Stereo pink(double tilt, std::size_t count, std::uint32_t seed = 1, double at_rate = rate,
            const keel::Curve* curve = nullptr) {
  constexpr double lowest_hz = 10.0;
  Stereo out{std::vector<double>(count), std::vector<double>(count)};
  std::uint32_t state = seed;
  const auto next = [&] {
    state = state * 1'664'525U + 1'013'904'223U;
    return static_cast<double>(state >> 8U) / static_cast<double>(1U << 24U);
  };
  for (auto* channel : {&out.left, &out.right}) {
    for (std::size_t from = 0; from < count; from += pink_period) {
      std::vector<std::complex<double>> spectrum(pink_period);
      for (std::size_t k = 1; k < pink_period / 2; ++k) {
        const double hz = static_cast<double>(k) * at_rate / static_cast<double>(pink_period);
        if (hz < lowest_hz) {
          continue;
        }
        double db = tilt * std::log2(hz / keel::reference_hz);
        if (curve != nullptr) {
          const double u = std::clamp(std::log2(hz / keel::reference_hz) * keel::bands_per_octave +
                                          static_cast<double>(keel::reference_band),
                                      0.0, static_cast<double>(keel::bands - 1));
          const auto b = std::min(static_cast<std::size_t>(u), keel::bands - 2);
          db += (*curve)[b] + ((*curve)[b + 1] - (*curve)[b]) * (u - static_cast<double>(b));
        }
        const double magnitude = db_to_gain(db) / std::sqrt(hz);
        spectrum[k] = std::polar(magnitude, 2.0 * std::numbers::pi * next());
        spectrum[pink_period - k] = std::conj(spectrum[k]);
      }
      fft(spectrum);
      double sum = 0.0;
      for (const auto& x : spectrum) {
        sum += x.real() * x.real();
      }
      const double scale = programme_rms / std::sqrt(sum / static_cast<double>(pink_period));
      for (std::size_t i = 0; i < pink_period && from + i < count; ++i) {
        (*channel)[from + i] = spectrum[i].real() * scale;
      }
    }
  }
  return out;
}

Stereo silence(std::size_t count) {
  return {std::vector<double>(count, 0.0), std::vector<double>(count, 0.0)};
}

Stereo render(const std::vector<double>& values, const Stereo& input,
              std::size_t block = oscillate::test::block_128, const Stereo* sidechain = nullptr) {
  auto runtime = make_effect(engine(), values, rate);
  return process(*runtime, input, {.block = block, .sidechain = sidechain});
}

// The third-octave balance (dB per band, both channels' mean power) of
// [from, to) by Welch's method on Keel's band edges.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named at every call.
std::array<double, keel::bands> balance_db(const Stereo& x, std::size_t from, std::size_t to,
                                           double at_rate = rate) {
  constexpr double half_band = 0.5 / keel::bands_per_octave;
  std::array<double, keel::bands> power{};
  std::vector<std::complex<double>> spectrum(welch_window);
  std::size_t segments = 0;
  for (std::size_t start = from; start + welch_window <= to; start += welch_hop) {
    for (const auto* channel : {&x.left, &x.right}) {
      for (std::size_t i = 0; i < welch_window; ++i) {
        spectrum[i] = (*channel)[start + i] *
                      oscillate::native_dsp::dsp::hann(static_cast<double>(i),
                                                       static_cast<double>(welch_window));
      }
      fft(spectrum);
      for (std::size_t b = 0; b < keel::bands; ++b) {
        const double lo = keel::band_hz(b) * std::exp2(-half_band);
        const double hi = keel::band_hz(b) * std::exp2(half_band);
        for (std::size_t k = 1; k < welch_window / 2; ++k) {
          const double hz = static_cast<double>(k) * at_rate / static_cast<double>(welch_window);
          if (hz >= lo && hz < hi) {
            power[b] += std::norm(spectrum[k]);
          }
        }
      }
    }
    ++segments;
  }
  std::array<double, keel::bands> out{};
  for (std::size_t b = 0; b < keel::bands; ++b) {
    out[b] = power_to_db(power[b] / static_cast<double>(std::max<std::size_t>(segments, 1)));
  }
  return out;
}

// The largest distance of `heard` from `wanted` over the judged bands,
// once both are centred there.
double deviation_db(const std::array<double, keel::bands>& heard, const keel::Curve& wanted) {
  double mean = 0.0;
  for (std::size_t b = judged_first(); b <= judged_last(); ++b) {
    mean += heard[b] - wanted[b];
  }
  mean /= static_cast<double>(judged_last() - judged_first() + 1);
  double most = 0.0;
  for (std::size_t b = judged_first(); b <= judged_last(); ++b) {
    most = std::max(most, std::fabs(heard[b] - wanted[b] - mean));
  }
  return most;
}

// The RMS distance of `heard` from `wanted` over the judged bands, once
// both are centred there.
double rms_deviation_db(const std::array<double, keel::bands>& heard, const keel::Curve& wanted) {
  double mean = 0.0;
  for (std::size_t b = judged_first(); b <= judged_last(); ++b) {
    mean += heard[b] - wanted[b];
  }
  const auto n = static_cast<double>(judged_last() - judged_first() + 1);
  mean /= n;
  double sum = 0.0;
  for (std::size_t b = judged_first(); b <= judged_last(); ++b) {
    const double d = heard[b] - wanted[b] - mean;
    sum += d * d;
  }
  return std::sqrt(sum / n);
}

keel::Curve tilted(double tilt) {
  keel::Curve out{};
  for (std::size_t b = 0; b < keel::bands; ++b) {
    out[b] = tilt * std::log2(keel::band_hz(b) / keel::reference_hz);
  }
  return out;
}

keel::Curve plus(keel::Curve a, const keel::Curve& b) {
  for (std::size_t i = 0; i < keel::bands; ++i) {
    a[i] += b[i];
  }
  return a;
}

// A flat target with room to correct the whole tilt: full Amount, 18 dB
// either way.
std::vector<double> wide(std::vector<double> values) {
  values[at("keel.tilt")] = 0.0;
  values[at("keel.amount")] = full;
  values[at("keel.max_boost")] = wide_limit_db;
  values[at("keel.max_cut")] = wide_limit_db;
  return values;
}

Stereo joined(Stereo a, const Stereo& b) {
  a.left.insert(a.left.end(), b.left.begin(), b.left.end());
  a.right.insert(a.right.end(), b.right.begin(), b.right.end());
  return a;
}

// A steady multitone from 50 Hz up: one tone per third-octave band, each on
// a bin of Keel's 16384-point window and at least six bins from the next,
// so every window reads the same power whatever its position; each tone at
// the level of its band in pink noise.
std::vector<double> multitone_hz() {
  constexpr double lowest_hz = 50.0;
  constexpr double bin_hz = rate / 16'384.0;
  constexpr double least_bins = 6.0;
  std::vector<double> out;
  double last_bin = 0.0;
  for (std::size_t b = band_of(lowest_hz); b < keel::bands; ++b) {
    const double bin = std::max(std::round(keel::band_hz(b) / bin_hz), last_bin + least_bins);
    if (bin * bin_hz < rate * oscillate::native_dsp::dsp::nyquist_guard) {
      out.push_back(bin * bin_hz);
    }
    last_bin = bin;
  }
  return out;
}

Stereo multitone(std::size_t count) {
  const auto hz = multitone_hz();
  const double amplitude = programme_rms * std::sqrt(2.0 / static_cast<double>(hz.size()));
  Stereo out = silence(count);
  for (std::size_t i = 0; i < count; ++i) {
    double x = 0.0;
    for (const double f : hz) {
      x += amplitude * std::sin(2.0 * std::numbers::pi * f * static_cast<double>(i) / rate);
    }
    out.left[i] = out.right[i] = x;
  }
  return out;
}

// A smooth drawn curve: lows lifted 3 dB below 100 Hz, a 2 dB dip about
// 3 kHz, the top eased 2 dB above 10 kHz.
keel::Curve drawn_curve() {
  constexpr double low_lift_db = 3.0;
  constexpr double low_hz = 100.0;
  constexpr double dip_db = -2.0;
  constexpr double dip_hz = 3'000.0;
  constexpr double dip_octaves = 1.0;
  constexpr double top_db = -2.0;
  constexpr double top_hz = 10'000.0;
  keel::Curve out{};
  for (std::size_t b = 0; b < keel::bands; ++b) {
    const double hz = keel::band_hz(b);
    const double below = std::clamp(-std::log2(hz / low_hz), 0.0, 1.0);
    const double near = std::log2(hz / dip_hz) / dip_octaves;
    const double above = std::clamp(std::log2(hz / top_hz), 0.0, 1.0);
    out[b] = low_lift_db * below + dip_db * std::exp(-near * near) + top_db * above;
  }
  return out;
}

// A mix's shape over its tilt: a 3 dB bump about 80 Hz and 2 dB more at
// 3 kHz than the target wants.
keel::Curve mix_curve() {
  constexpr double bump_db = 3.0;
  constexpr double bump_hz = 80.0;
  constexpr double presence_db = 2.0;
  constexpr double presence_hz = 3'000.0;
  keel::Curve out{};
  for (std::size_t b = 0; b < keel::bands; ++b) {
    const double hz = keel::band_hz(b);
    const double low = std::log2(hz / bump_hz);
    const double mid = std::log2(hz / presence_hz);
    out[b] = bump_db * std::exp(-low * low) + presence_db * std::exp(-mid * mid);
  }
  return out;
}

// Renders hop by hop and records the correction the telemetry reports after
// each hop: the balance the output has is the input's plus that.
struct Traced {
  Stereo out;
  std::vector<keel::Curve> corrections;
  std::vector<float> last;
};

Traced trace(const std::vector<double>& values, const Stereo& input,
             const Stereo* sidechain = nullptr, double at_rate = rate) {
  auto runtime = make_effect(engine(), values, at_rate);
  Traced result{silence(input.size()), {}, {}};
  std::vector<engine::StereoFrame> buffer;
  std::vector<engine::StereoFrame> side;
  for (std::size_t from = 0; from < input.size(); from += hop) {
    const std::size_t n = std::min(hop, input.size() - from);
    buffer.resize(n);
    side.resize(sidechain != nullptr ? n : 0);
    for (std::size_t i = 0; i < n; ++i) {
      buffer[i] = {static_cast<float>(input.left[from + i]),
                   static_cast<float>(input.right[from + i])};
      if (sidechain != nullptr) {
        side[i] = {static_cast<float>(sidechain->left[from + i]),
                   static_cast<float>(sidechain->right[from + i])};
      }
    }
    constexpr double beats_per_bar = 4.0;
    const Transport transport{0.0, 0.0, 0.0, beats_per_bar};
    REQUIRE_EQ(runtime->process(buffer, side, from, 1, transport, {}),
               effects::EffectProcessStatus::rendered, "the block renders");
    for (std::size_t i = 0; i < n; ++i) {
      result.out.left[from + i] = buffer[i].left;
      result.out.right[from + i] = buffer[i].right;
    }
    REQUIRE_TRUE(runtime->read_telemetry(result.last), "a telemetry frame");
    keel::Curve correction{};
    for (std::size_t b = 0; b < keel::bands; ++b) {
      correction[b] = result.last[keel::telemetry::correction + b];
    }
    result.corrections.push_back(correction);
  }
  return result;
}

// The first hop from `from` at which the input's balance plus the
// correction comes within `within` of the target; the hop count when it
// never does.
std::size_t reached_hop(const Traced& traced, std::size_t from, const keel::Curve& input,
                        const keel::Curve& wanted, double within) {
  for (std::size_t h = from; h < traced.corrections.size(); ++h) {
    if (deviation_db(plus(input, traced.corrections[h]), wanted) <= within) {
      return h;
    }
  }
  return traced.corrections.size();
}

// The first hop from which the input's balance plus the correction stays
// within `within` of the target; the hop count when it never does.
std::size_t settled_hop(const Traced& traced, const keel::Curve& input, const keel::Curve& wanted,
                        double within) {
  std::size_t settled = traced.corrections.size();
  for (std::size_t h = traced.corrections.size(); h-- > 0;) {
    if (deviation_db(plus(input, traced.corrections[h]), wanted) > within) {
      break;
    }
    settled = h;
  }
  return settled;
}

void descriptor_registry_and_presets() {
  REQUIRE_EQ(effects::find_engine("keel"), &engine(), "Keel is registered");
  require(engine().sidechain && !engine().generator, "Keel takes a reference on its sidechain");
  REQUIRE_EQ(table().parameters.size(), parameter_count, "the table's size");
  for (const auto* id : {"keel.target", "keel.match", "keel.delta", "keel.curve_01"}) {
    require(!table().parameters[at(id)].automatable, std::string(id) + " is not automatable");
  }
  REQUIRE_EQ(table().presets.size(), preset_count, "Default and five presets");
  for (std::size_t b = 0; b < keel::bands; ++b) {
    const double nominal = keel::band_hz(b);
    REQUIRE_EQ(band_of(nominal), b, "the band grid is a third of an octave");
  }
  REQUIRE_LT(std::fabs(keel::band_hz(keel::reference_band) - keel::reference_hz),
             tolerance::double_rounding, "1 kHz is a band centre");
}

// Amount 0 is a wire: the input bit for bit, with Level Match on, while
// the tracker follows a programme whose balance is far from the target.
void amount_zero_is_bit_exact() {
  constexpr double seconds = 6.0;
  const auto input = pink(tilt_db, frames(seconds));
  const auto values = with(defaults(), "keel.amount", 0.0);
  const auto out = render(values, input);
  for (std::size_t i = 0; i < input.size(); ++i) {
    require(out.left[i] == rounded(input.left[i]) && out.right[i] == rounded(input.right[i]),
            "Amount 0 passes the input untouched");
  }
  REQUIRE_EQ(engine().latency_frames(values, rate), 0U, "no latency");
}

// Pink noise tilted 3 dB/oct either way converges to a flat target: the
// balance the output has (the input's plus the correction) comes within
// 1 dB of the target over 40 Hz to 16 kHz in under two seconds at every
// Speed (a running mean fills the estimate at the start) and stays there,
// ending within half a decibel; a Welch analysis of the output agrees.
void tilted_pink_converges_to_the_target() {
  constexpr double seconds = 20.0;
  constexpr double first_seconds = 2.0;
  constexpr double measured_seconds = 8.0;
  constexpr double final_db = 0.5;
  // Eight seconds of noise read in the 40 Hz band spread a quarter of a
  // decibel of their own.
  constexpr double welch_tolerance_db = 1.25;
  const auto values = wide(defaults());
  const keel::Curve flat{};
  for (const double sign : {1.0, -1.0}) {
    const auto input = pink(sign * tilt_db, frames(seconds));
    for (const double speed : speeds) {
      const auto traced = trace(with(values, "keel.speed", speed), input);
      const auto settled = settled_hop(traced, tilted(sign * tilt_db), flat, converged_db);
      const double settled_seconds = static_cast<double>(settled * hop) / rate;
      const auto label = " at " + std::to_string(speed) + " s Speed, tilt " +
                         std::to_string(sign * tilt_db) + ": ";
      REQUIRE_LT(settled_seconds, first_seconds,
                 "converged" + label + std::to_string(settled_seconds) + " s");
      const double last =
          deviation_db(plus(tilted(sign * tilt_db), traced.corrections.back()), flat);
      REQUIRE_LT(last, final_db, "ends within half a decibel" + label + std::to_string(last));
      const auto heard =
          balance_db(traced.out, input.size() - frames(measured_seconds), input.size());
      const double off = deviation_db(heard, flat);
      REQUIRE_LT(off, welch_tolerance_db,
                 "the output's measured balance is the target's" + label + std::to_string(off));
    }
  }
}

// After the programme's balance changes, Speed sets how long the
// correction takes to follow: the bands average power over Speed, so a
// reversal from -3 to +3 dB/oct (36 dB at the ends) first comes within 1
// dB after six to seven Speeds at every Speed.
void speed_sets_the_time_to_follow() {
  constexpr double before_seconds = 10.0;
  constexpr double most_speeds = 7.5;
  constexpr double least_speeds = 5.0;
  constexpr double seconds_after = 120.0;
  const auto values = wide(defaults());
  const keel::Curve flat{};
  const auto input =
      joined(pink(-tilt_db, frames(before_seconds)), pink(tilt_db, frames(seconds_after), 2));
  for (const double speed : speeds) {
    const auto traced = trace(with(values, "keel.speed", speed), input);
    const auto settled =
        reached_hop(traced, frames(before_seconds) / hop, tilted(tilt_db), flat, converged_db);
    const double after = static_cast<double>(settled * hop) / rate - before_seconds;
    REQUIRE_LT(after, most_speeds * speed,
               "follows within seven and a half Speeds at " + std::to_string(speed) +
                   " s: " + std::to_string(after));
    REQUIRE_GT(after, least_speeds * speed, "and takes at least five: " + std::to_string(after));
  }
}

// The wander of a steady programme's correction once it has converged:
// its RMS about its mean (dB, the mean over the judged bands) and the RMS
// of what moves faster than a second (pumping), from the hop-by-hop trace.
struct Wander {
  double rms_db = 0.0;
  double fast_db = 0.0;
};

Wander wander(const Traced& traced, std::size_t from) {
  constexpr double trend_seconds = 1.0;
  const auto half = static_cast<std::size_t>(trend_seconds * rate / static_cast<double>(hop) / 2.0);
  Wander out;
  for (std::size_t b = judged_first(); b <= judged_last(); ++b) {
    double mean = 0.0;
    for (std::size_t h = from; h < traced.corrections.size(); ++h) {
      mean += traced.corrections[h][b];
    }
    mean /= static_cast<double>(traced.corrections.size() - from);
    double spread = 0.0;
    double fast = 0.0;
    std::size_t counted = 0;
    for (std::size_t h = from; h < traced.corrections.size(); ++h) {
      const double d = traced.corrections[h][b] - mean;
      spread += d * d;
      if (h >= from + half && h + half < traced.corrections.size()) {
        double trend = 0.0;
        for (std::size_t k = h - half; k <= h + half; ++k) {
          trend += traced.corrections[k][b];
        }
        trend /= static_cast<double>(2 * half + 1);
        fast += (traced.corrections[h][b] - trend) * (traced.corrections[h][b] - trend);
        ++counted;
      }
    }
    out.rms_db += std::sqrt(spread / static_cast<double>(traced.corrections.size() - from));
    out.fast_db += std::sqrt(fast / static_cast<double>(std::max<std::size_t>(counted, 1)));
  }
  const auto n = static_cast<double>(judged_last() - judged_first() + 1);
  out.rms_db /= n;
  out.fast_db /= n;
  return out;
}

// A steady programme is not modulated: once converged, steady pink noise's
// correction wanders by no more than its estimate's own spread (RMS 0.25
// dB at 1 s Speed, less when slower), and what moves faster than a second
// stays under 0.05 dB RMS: no pumping. A periodic multitone, whose
// spectrum each window reads the same but for the window's sidelobes,
// holds its correction within 0.05 dB.
void a_steady_programme_is_not_modulated() {
  constexpr double settle_seconds = 10.0;
  constexpr double seconds = 40.0;
  constexpr std::array most_rms_db{0.25, 0.15, 0.1};
  constexpr double most_fast_db = 0.05;
  const auto values = with(defaults(), "keel.amount", full);
  const auto input = pink(-tilt_db / 2.0, frames(seconds));
  for (std::size_t s = 0; s < speeds.size(); ++s) {
    const auto traced = trace(with(values, "keel.speed", speeds[s]), input);
    const auto measured = wander(traced, frames(settle_seconds) / hop);
    const auto label = " at " + std::to_string(speeds[s]) + " s Speed: ";
    REQUIRE_LT(measured.rms_db, most_rms_db[s],
               "the correction wanders little" + label + std::to_string(measured.rms_db));
    REQUIRE_LT(measured.fast_db, most_fast_db,
               "nothing pumps" + label + std::to_string(measured.fast_db));
  }
  constexpr double held_db = 0.05;
  const auto tones = multitone(frames(seconds));
  const auto traced = trace(values, tones);
  const auto from = frames(settle_seconds) / hop;
  for (std::size_t h = from; h < traced.corrections.size(); ++h) {
    for (std::size_t b = 0; b < keel::bands; ++b) {
      REQUIRE_LT(std::fabs(traced.corrections[h][b] - traced.corrections[from][b]), held_db,
                 "a periodic programme's correction holds still");
    }
  }
}

// While the correction moves (a reference whose balance turns over), a
// steady multitone through Keel gains no sidebands away from its tones:
// the gains glide across each hop and the filters follow every 16 frames,
// so nothing zips. Energy more than 40 Hz from every tone stays 100 dB
// under the tones.
void a_moving_correction_has_no_zipper() {
  constexpr double seconds = 12.0;
  constexpr double turn_seconds = 4.0;
  constexpr double reference_tilt = 1.5;
  constexpr double guard_hz = 40.0;
  constexpr double most_db = -100.0;
  constexpr std::size_t analysed = 1U << 18U;
  auto values = with(defaults(), "keel.amount", full);
  values[at("keel.target")] = target_reference;
  values[at("keel.speed")] = speeds.front();
  const auto tones = multitone(frames(seconds));
  const auto reference = joined(pink(-reference_tilt, frames(turn_seconds), 3),
                                pink(reference_tilt, frames(seconds - turn_seconds), 4));
  const auto out = render(values, tones, oscillate::test::block_128, &reference);
  // A four-term Blackman-Harris window over the turn.
  constexpr std::array<double, 4> bh{0.35875, 0.48829, 0.14128, 0.01168};
  const auto from = frames(turn_seconds) - analysed / 4;
  std::vector<std::complex<double>> spectrum(analysed);
  for (std::size_t i = 0; i < analysed; ++i) {
    const double u =
        2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(analysed);
    const double w =
        bh[0] - bh[1] * std::cos(u) + bh[2] * std::cos(2.0 * u) - bh[3] * std::cos(3.0 * u);
    spectrum[i] = out.left[from + i] * w;
  }
  fft(spectrum);
  double tone_power = 0.0;
  double other_power = 0.0;
  const auto hz_list = multitone_hz();
  for (std::size_t k = 1; k < analysed / 2; ++k) {
    const double hz = static_cast<double>(k) * rate / static_cast<double>(analysed);
    const bool near =
        std::ranges::any_of(hz_list, [&](double tone) { return std::fabs(hz - tone) <= guard_hz; });
    (near ? tone_power : other_power) += std::norm(spectrum[k]);
  }
  const double sidebands = power_to_db(other_power / tone_power);
  REQUIRE_LT(sidebands, most_db, "no zipper sidebands: " + std::to_string(sidebands) + " dB");
}

// Minimum phase earns its keep: converged on a 4 dB lift at 80 Hz, Keel's
// impulse response (impulses added to the programme, less the programme
// alone, averaged over the impulses) delays no third-octave band from
// 31.5 Hz up by more than 2 ms (1.5 ms at 63 Hz), and none from 200 Hz up
// by more than a quarter of a millisecond. A linear-phase filter resolving
// 20 Hz would add about 85 ms of latency and ring before every kick.
void minimum_phase_delays_little() {
  constexpr double settle_seconds = 8.0;
  constexpr std::size_t probes = 16;
  constexpr std::size_t length = 1U << 14U;
  constexpr double probe = 1e-3;
  constexpr double lift_db = 4.0;
  constexpr double lift_hz = 80.0;
  constexpr double lowest_hz = 31.5;
  constexpr double knee_hz = 200.0;
  constexpr double highest_hz = 16'000.0;
  constexpr double most_ms = 2.0;
  constexpr double most_above_knee_ms = 0.25;
  constexpr double least_lift_db = 2.0;
  constexpr double milliseconds = 1'000.0;
  auto values = wide(defaults());
  values[at("keel.target")] = target_curve;
  keel::Curve bump{};
  for (std::size_t b = 0; b < keel::bands; ++b) {
    const double octaves = std::log2(keel::band_hz(b) / lift_hz);
    bump[b] = lift_db * std::exp(-octaves * octaves);
  }
  keel::set_curve(values, bump);
  const auto first = frames(settle_seconds);
  const auto input = pink(0.0, first + probes * length);
  auto probed = input;
  for (std::size_t p = 0; p < probes; ++p) {
    probed.left[first + p * length] += probe;
    probed.right[first + p * length] += probe;
  }
  const auto plain = render(values, input);
  const auto heard = render(values, probed);
  std::vector<std::complex<double>> mean(length);
  for (std::size_t p = 0; p < probes; ++p) {
    std::vector<std::complex<double>> h(length);
    for (std::size_t n = 0; n < length; ++n) {
      const auto i = first + p * length + n;
      h[n] = (heard.left[i] - plain.left[i]) / probe;
    }
    fft(h);
    for (std::size_t k = 0; k < length; ++k) {
      mean[k] += h[k] / static_cast<double>(probes);
    }
  }
  const auto bin_hz = rate / static_cast<double>(length);
  const auto bin_of = [&](double hz) { return static_cast<std::size_t>(std::lround(hz / bin_hz)); };
  REQUIRE_GT(
      gain_to_db(std::abs(mean[bin_of(lift_hz)]) / std::abs(mean[bin_of(keel::reference_hz)])),
      least_lift_db, "the lift is in place");
  // Per band: the phase's turn across the band's bins over their span.
  const double half_band = 0.5 / keel::bands_per_octave;
  for (std::size_t b = band_of(lowest_hz); b <= band_of(highest_hz); ++b) {
    const auto lo = bin_of(keel::band_hz(b) * std::exp2(-half_band));
    const auto hi = bin_of(keel::band_hz(b) * std::exp2(half_band));
    std::complex<double> turn = 1.0;
    for (std::size_t k = lo; k < hi; ++k) {
      turn += mean[k + 1] * std::conj(mean[k]);
    }
    const double radians_per_bin = 2.0 * std::numbers::pi / static_cast<double>(length);
    const double delay_ms = -std::arg(turn) / radians_per_bin / rate * milliseconds;
    const double hz = keel::band_hz(b);
    REQUIRE_LT(std::fabs(delay_ms), hz < knee_hz ? most_ms : most_above_knee_ms,
               "group delay at " + std::to_string(hz) + " Hz: " + std::to_string(delay_ms) + " ms");
  }
}

// A drawn curve is reached: flat pink comes to the curve within 1 dB.
void a_curve_target_is_reached() {
  constexpr double seconds = 12.0;
  auto values = wide(defaults());
  values[at("keel.target")] = target_curve;
  const auto drawn = drawn_curve();
  keel::set_curve(values, drawn);
  const auto wanted = keel::target_curve(values);
  for (std::size_t b = 0; b < keel::bands; ++b) {
    REQUIRE_LT(std::fabs(wanted[b] - drawn[b]), tolerance::double_rounding,
               "the target is the drawn curve");
  }
  const auto traced = trace(values, pink(0.0, frames(seconds)));
  const keel::Curve flat{};
  const double off = deviation_db(plus(flat, traced.corrections.back()), wanted);
  REQUIRE_LT(off, converged_db, "the drawn curve is reached: " + std::to_string(off));
}

// Reference: the sidechain's own balance (plus the tilt) is the target.
// Until a reference has been heard the correction holds at 0 dB, so with
// no sidechain Keel passes the input bit for bit even at full Amount.
void a_reference_target_is_reached() {
  constexpr double seconds = 12.0;
  constexpr double reference_tilt = -1.5;
  auto values = wide(defaults());
  values[at("keel.target")] = target_reference;
  const auto input = pink(0.0, frames(seconds));
  const auto unheard = render(values, input);
  for (std::size_t i = 0; i < input.size(); ++i) {
    require(unheard.left[i] == rounded(input.left[i]), "no reference: no correction");
  }
  const auto bump = drawn_curve();
  const auto reference = pink(reference_tilt, frames(seconds), 5, rate, &bump);
  const auto traced = trace(values, input, &reference);
  const auto wanted = plus(tilted(reference_tilt), bump);
  const keel::Curve flat{};
  const double off = deviation_db(plus(flat, traced.corrections.back()), wanted);
  REQUIRE_LT(off, converged_db, "the reference's balance is reached: " + std::to_string(off));
  REQUIRE_EQ(traced.last[keel::telemetry::referenced], 1.0F, "the reference is reported");
}

// Learn captures the programme's balance as the target: on the same
// programme the correction then rests near 0 dB, with or without a tilt.
void learn_captures_the_balance() {
  constexpr double seconds = 12.0;
  constexpr double programme_tilt = -1.0;
  constexpr double learned_rest_db = 0.5;
  const auto bump = drawn_curve();
  const auto input = pink(programme_tilt, frames(seconds), 1, rate, &bump);
  for (const double tilt : {0.0, 1.0}) {
    auto values = wide(with(defaults(), "keel.tilt", tilt));
    const auto heard = trace(values, input);
    keel::set_curve(values, keel::learned_curve(heard.last, values));
    values[at("keel.target")] = target_curve;
    const auto traced = trace(values, input);
    for (std::size_t b = judged_first(); b <= judged_last(); ++b) {
      REQUIRE_LT(std::fabs(traced.corrections.back()[b]), learned_rest_db,
                 "the learned balance needs no correction at " + std::to_string(keel::band_hz(b)) +
                     " Hz: " + std::to_string(traced.corrections.back()[b]));
    }
  }
}

// Regions weight the correction: with only Low on, the lows move and the
// band two octaves above the low split and higher are left alone.
void regions_weight_the_correction() {
  constexpr double seconds = 10.0;
  constexpr double moved_db = 6.0;
  constexpr double left_db = 1.0;
  constexpr double low_band_hz = 63.0;
  constexpr double octaves_clear = 2.0;
  auto values = wide(defaults());
  values[at("keel.mid")] = 0.0;
  values[at("keel.high")] = 0.0;
  const auto traced = trace(values, pink(tilt_db, frames(seconds)));
  const auto& last = traced.corrections.back();
  REQUIRE_GT(last[band_of(low_band_hz)], moved_db, "the lows are lifted");
  const double clear_hz = values[at("keel.low_split")] * std::exp2(octaves_clear);
  for (std::size_t b = band_of(clear_hz); b < keel::bands; ++b) {
    REQUIRE_LT(std::fabs(last[b]), left_db,
               "the rest is left alone at " + std::to_string(keel::band_hz(b)) +
                   " Hz: " + std::to_string(last[b]));
  }
}

// Max Boost and Max Cut hold, within the bank's fit (its bells cannot
// turn the clamped correction's corners exactly).
void limits_hold() {
  constexpr double seconds = 10.0;
  constexpr double boost = 2.0;
  constexpr double cut = 3.0;
  constexpr double fit_db = 0.75;
  auto values = with(defaults(), "keel.amount", full);
  values[at("keel.max_boost")] = boost;
  values[at("keel.max_cut")] = cut;
  for (const double sign : {1.0, -1.0}) {
    const auto traced = trace(values, pink(sign * tilt_db, frames(seconds)));
    for (const auto& correction : traced.corrections) {
      for (const double db : correction) {
        REQUIRE_LT(db, boost + fit_db, "within Max Boost: " + std::to_string(db));
        REQUIRE_GT(db, -cut - fit_db, "within Max Cut: " + std::to_string(db));
      }
    }
  }
}

// Bands the programme leaves empty are never boosted: pink noise with
// nothing above 4 kHz is not lifted there, though the target asks for it.
void absent_bands_are_not_boosted() {
  constexpr double seconds = 10.0;
  constexpr double empty_from_hz = 5'000.0;
  constexpr double emptied_db = -80.0;
  constexpr double least_boost_db = 0.25;
  keel::Curve emptied{};
  for (std::size_t b = band_of(empty_from_hz); b < keel::bands; ++b) {
    emptied[b] = emptied_db;
  }
  const auto traced = trace(wide(defaults()), pink(0.0, frames(seconds), 1, rate, &emptied));
  const double absent_hz = empty_from_hz * 2.0;
  for (std::size_t b = band_of(absent_hz); b < keel::bands; ++b) {
    REQUIRE_LT(traced.corrections.back()[b], least_boost_db,
               "an empty band is not boosted at " + std::to_string(keel::band_hz(b)) +
                   " Hz: " + std::to_string(traced.corrections.back()[b]));
  }
}

// The gates: silences in the programme (fades, gaps, the stop at the end)
// leave the estimate where the music had it, and rare loud bursts move it
// a fraction of what they move a plain long-term average.
void silence_and_transients_do_not_drag_it() {
  constexpr double seconds = 24.0;
  constexpr double gap_period_seconds = 3.0;
  constexpr double gap_seconds = 1.0;
  constexpr double held_db = 0.25;
  constexpr double burst_period_seconds = 3.0;
  constexpr double burst_seconds = 0.02;
  constexpr double burst_gain = 30.0;
  constexpr double share_of_plain = 0.5;
  const auto values = wide(defaults());
  const auto input = pink(-tilt_db / 2.0, frames(seconds));
  const auto plain = trace(values, input).corrections.back();
  auto gapped = input;
  const auto period = frames(gap_period_seconds);
  for (std::size_t i = 0; i < gapped.size(); ++i) {
    if (i % period < frames(gap_seconds)) {
      gapped.left[i] = gapped.right[i] = 0.0;
    }
  }
  const auto after_gaps = trace(values, gapped).corrections.back();
  for (std::size_t b = judged_first(); b <= judged_last(); ++b) {
    REQUIRE_LT(std::fabs(after_gaps[b] - plain[b]), held_db,
               "silences do not drag it at " + std::to_string(keel::band_hz(b)) + " Hz");
  }
  auto burst = input;
  const auto clicks = noise(programme_rms * burst_gain, burst.size(), false, 7);
  for (std::size_t i = 0; i < burst.size(); ++i) {
    if (i % frames(burst_period_seconds) < frames(burst_seconds)) {
      burst.left[i] += clicks.left[i];
      burst.right[i] += clicks.right[i];
    }
  }
  const auto after_bursts = trace(values, burst).corrections.back();
  const auto heard_plain = balance_db(input, 0, input.size());
  const auto heard_burst = balance_db(burst, 0, burst.size());
  double moved = 0.0;
  double plain_moved = 0.0;
  for (std::size_t b = judged_first(); b <= judged_last(); ++b) {
    moved = std::max(moved, std::fabs(after_bursts[b] - plain[b]));
    plain_moved = std::max(plain_moved, std::fabs(heard_burst[b] - heard_plain[b]));
  }
  REQUIRE_LT(moved, share_of_plain * plain_moved,
             "bursts move it " + std::to_string(moved) + " dB where a plain average moves " +
                 std::to_string(plain_moved));
}

double loudness(const Stereo& x, std::size_t from) {
  oscillate::native_dsp::IntegratedLoudnessAnalyzer analyser(
      oscillate::test::sample_rate_48k, oscillate::native_dsp::LoudnessLayout::stereo);
  std::vector<float> interleaved;
  for (std::size_t i = from; i < x.size(); ++i) {
    interleaved.push_back(static_cast<float>(x.left[i]));
    interleaved.push_back(static_cast<float>(x.right[i]));
  }
  (void)analyser.append(interleaved);
  return analyser.finish().integrated_lkfs.value_or(-HUGE_VAL);
}

// Level Match holds the loudness through a large correction; without it
// the loudness moves.
void level_match_holds_the_loudness() {
  constexpr double seconds = 16.0;
  constexpr double settle_seconds = 4.0;
  constexpr double held_lu = 0.5;
  constexpr double moved_lu = 1.0;
  const auto values = wide(defaults());
  const auto input = pink(-tilt_db, frames(seconds));
  const double before = loudness(input, frames(settle_seconds));
  const double matched = loudness(render(values, input), frames(settle_seconds));
  const double unmatched =
      loudness(render(with(values, "keel.match", 0.0), input), frames(settle_seconds));
  REQUIRE_LT(std::fabs(matched - before), held_lu,
             "matched within half a LU: " + std::to_string(matched - before));
  REQUIRE_GT(std::fabs(unmatched - before), moved_lu,
             "unmatched it moves: " + std::to_string(unmatched - before));
}

// Delta plays what Keel changes: the output less the input.
void delta_plays_the_change() {
  constexpr double seconds = 6.0;
  auto values = wide(with(defaults(), "keel.match", 0.0));
  const auto input = pink(tilt_db, frames(seconds));
  const auto out = render(values, input);
  const auto change = render(with(values, "keel.delta", on), input);
  for (std::size_t i = 0; i < input.size(); ++i) {
    REQUIRE_LT(std::fabs(out.left[i] - rounded(input.left[i]) - change.left[i]),
               tolerance::float_round_trip, "delta is the output less the input");
  }
}

// Output trims after everything, gliding into place.
void output_trims() {
  constexpr double seconds = 2.0;
  constexpr double trim_db = 6.0;
  const auto values = with(with(defaults(), "keel.amount", 0.0), "keel.output", trim_db);
  const auto input = pink(0.0, frames(seconds));
  const auto out = render(values, input);
  for (std::size_t i = 0; i < input.size(); ++i) {
    REQUIRE_LT(std::fabs(out.left[i] - db_to_gain(trim_db) * rounded(input.left[i])),
               tolerance::float_round_trip, "the output is trimmed");
  }
}

// The same convergence at 44.1 and 96 kHz: the analysis frames scale with
// the rate, so the bands hold the same bins.
void other_rates_converge_alike() {
  constexpr double seconds = 12.0;
  const keel::Curve flat{};
  for (const double other :
       {oscillate::test::sample_rate_44k_hz, oscillate::test::sample_rate_96k_hz}) {
    const auto input = pink(-tilt_db, frames(seconds, other), 1, other);
    const auto traced = trace(wide(defaults()), input, nullptr, other);
    const double off = deviation_db(plus(tilted(-tilt_db), traced.corrections.back()), flat);
    REQUIRE_LT(off, converged_db,
               "converges at " + std::to_string(other) + " Hz: " + std::to_string(off));
  }
}

std::vector<std::vector<double>> varied() {
  std::vector<std::vector<double>> result;
  for (const auto& entry : table().presets) {
    result.push_back(preset(table(), entry.name));
  }
  auto extreme = wide(defaults());
  extreme[at("keel.tilt")] = tilt_db;
  extreme[at("keel.output")] = -tilt_db;
  result.push_back(extreme);
  auto drawn = wide(defaults());
  drawn[at("keel.target")] = target_curve;
  keel::set_curve(drawn, drawn_curve());
  drawn[at("keel.delta")] = on;
  result.push_back(drawn);
  return result;
}

void any_block_split_renders_the_same() {
  constexpr double seconds = 4.0;
  const auto input = pink(-tilt_db, frames(seconds));
  for (const auto& v : varied()) {
    const auto a = render(v, input, oscillate::test::block_128);
    const auto b = render(v, input, 0);
    require(a.left == b.left && a.right == b.right, "the split changes nothing");
  }
}

// Export renders in long slices on its own transport generation, stopped;
// live in callback blocks while playing. The audio is the same.
void export_equals_live() {
  constexpr std::size_t export_block = 4'096;
  constexpr double live_bpm = 124.0;
  constexpr double seconds = 4.0;
  const auto input = pink(-tilt_db, frames(seconds));
  for (const auto& v : varied()) {
    auto live = make_effect(engine(), v, rate);
    const auto played =
        process(*live, input, {.block = oscillate::test::block_128, .bpm = live_bpm});
    auto offline = make_effect(engine(), v, rate);
    const auto exported =
        process(*offline, input, {.block = export_block, .generation = 2, .bpm = 0.0});
    require(played.left == exported.left && played.right == exported.right, "export equals live");
  }
}

void hostile_input_stays_finite() {
  constexpr std::size_t nan_at = 100;
  constexpr std::size_t infinity_at = 200;
  constexpr std::size_t huge_at = 300;
  constexpr double huge = 1e30;
  constexpr double tiny = 1e-30;
  constexpr double seconds = 3.0;
  constexpr double tiny_from_seconds = 2.0;
  constexpr double recovered_seconds = 1.0;
  Stereo input = pink(-tilt_db, frames(seconds));
  input.left[nan_at] = std::numeric_limits<double>::quiet_NaN();
  input.right[infinity_at] = std::numeric_limits<double>::infinity();
  input.left[huge_at] = huge;
  for (std::size_t i = frames(tiny_from_seconds); i < input.size(); ++i) {
    input.left[i] = input.right[i] = tiny;
  }
  for (const auto& v : varied()) {
    const auto out = render(v, input, oscillate::test::block_128, &input);
    for (std::size_t i = 0; i < out.size(); ++i) {
      require(std::isfinite(out.left[i]) && std::isfinite(out.right[i]), "finite output");
    }
    REQUIRE_LT(peak(out.left, frames(recovered_seconds), frames(tiny_from_seconds)),
               db_to_gain(wide_limit_db) *
                   peak(input.right, frames(recovered_seconds), frames(tiny_from_seconds)),
               "the wild samples pass and are gone");
  }
}

// After the input stops, the output falls silent within the tail reported.
void the_tail_is_bounded() {
  constexpr double silent = 1e-6; // -120 dBFS
  constexpr double seconds = 4.0;
  const auto played = frames(seconds);
  for (const auto& v : varied()) {
    const auto tail = engine().tail_frames(v, rate);
    auto input = pink(-tilt_db, played + static_cast<std::size_t>(tail) + frames(seconds));
    for (std::size_t i = played; i < input.size(); ++i) {
      input.left[i] = input.right[i] = 0.0;
    }
    const auto out = render(v, input);
    REQUIRE_LT(peak(out.left, played + static_cast<std::size_t>(tail)), silent,
               "silent after the tail");
  }
}

// On a mix-like programme (pink falling 1.5 dB/oct, brighter than the
// default target, with a bump in the lows and a lift at 3 kHz), every
// preset moves the balance (its RMS distance) toward its
// target, never by more than 4 dB anywhere (and the bank's half-decibel
// fit), and holds the loudness within half a LU.
void presets_stay_restrained() {
  constexpr double seconds = 24.0;
  constexpr double settle_seconds = 8.0;
  constexpr double most_db = 4.5;
  constexpr double held_lu = 0.5;
  constexpr double programme_tilt = -1.5;
  const auto shape = mix_curve();
  const auto input = pink(programme_tilt, frames(seconds), 1, rate, &shape);
  const auto programme = plus(tilted(programme_tilt), shape);
  const double before_lufs = loudness(input, frames(settle_seconds));
  for (const auto& entry : table().presets) {
    const auto values = preset(table(), entry.name);
    const auto traced = trace(values, input);
    const auto& last = traced.corrections.back();
    const auto wanted = keel::target_curve(values);
    for (std::size_t h = 0; h < traced.corrections.size(); ++h) {
      for (const double db : traced.corrections[h]) {
        REQUIRE_LT(std::fabs(db), most_db,
                   std::string(entry.name) + " stays within 4 dB: " + std::to_string(db));
      }
    }
    const double before = rms_deviation_db(programme, wanted);
    const double after = rms_deviation_db(plus(programme, last), wanted);
    REQUIRE_LT(after, before,
               std::string(entry.name) + " moves toward its target: " + std::to_string(before) +
                   " to " + std::to_string(after));
    const double lufs = loudness(traced.out, frames(settle_seconds));
    REQUIRE_LT(std::fabs(lufs - before_lufs), held_lu,
               std::string(entry.name) +
                   " holds the loudness: " + std::to_string(lufs - before_lufs));
  }
}

// A seek (a new transport generation) clears every history: after it
// Keel plays as a fresh one.
void a_seek_starts_clean() {
  constexpr double seconds = 3.0;
  const auto before = pink(tilt_db, frames(seconds));
  const auto after = pink(-tilt_db, frames(seconds), 9);
  const auto v = preset(table(), "Streaming Balance");
  auto seeked = make_effect(engine(), v, rate);
  (void)process(*seeked, before);
  const auto resumed = process(*seeked, after, {.generation = 2});
  const auto fresh = render(v, after);
  require(resumed.left == fresh.left && resumed.right == fresh.right,
          "after a seek Keel plays as a fresh one");
}

void the_callback_never_allocates() {
  constexpr std::uint64_t blocks = 2'000;
  constexpr float level = 0.3F;
  constexpr double moved_speed = 1.0;
  const auto values = preset(table(), "Streaming Balance");
  auto runtime = make_effect(engine(), values, rate);
  const auto warm = pink(-tilt_db, frames(1.0));
  (void)process(*runtime, warm, {.sidechain = &warm});
  auto changed = values;
  changed[at("keel.target")] = target_reference;
  changed[at("keel.speed")] = moved_speed;
  changed[at("keel.delta")] = on;
  require(runtime->offer_values(changed, 1), "new values are offered");
  std::vector<engine::StereoFrame> buffer(oscillate::test::block_128, {level, -level});
  const std::vector<engine::StereoFrame> side(oscillate::test::block_128, {-level, level});
  const auto start = static_cast<std::uint64_t>(warm.size());
  counted_allocations = 0;
  counting_allocations = true;
  for (std::uint64_t block = 0; block < blocks; ++block) {
    constexpr double beats_per_bar = 4.0;
    const Transport transport{0.0, 0.0, 0.0, beats_per_bar};
    (void)runtime->process(buffer, side, start + block * buffer.size(),
                           block < blocks / 2 ? 1U : 2U, transport, {});
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "the callback allocates nothing");
}

// The panel's reading: the tracked balance and the target, centred, the
// correction now and its history, Level Match's gain and the meters.
void telemetry_feeds_the_panel() {
  namespace tm = keel::telemetry;
  constexpr double seconds = 12.0;
  constexpr float reading_db = 1.0F;
  constexpr float fit_db = 1.0F;
  const auto input = pink(-tilt_db, frames(seconds));
  const auto traced = trace(wide(defaults()), input);
  const auto& frame = traced.last;
  REQUIRE_EQ(frame.size(), tm::size, "the layout's size");
  REQUIRE_EQ(frame[tm::tracked], 1.0F, "the input is tracked");
  REQUIRE_EQ(frame[tm::referenced], 0.0F, "no reference");
  REQUIRE_EQ(frame[tm::latency_ms], 0.0F, "no latency");
  REQUIRE_EQ(frame[tm::target_source], 0.0F, "the tilt target");
  // The spectrum is the input's tilt, centred; the target flat, centred;
  // the correction their difference.
  const auto tilt = tilted(-tilt_db);
  const float offset =
      frame[tm::spectrum + keel::reference_band] - static_cast<float>(tilt[keel::reference_band]);
  for (std::size_t b = judged_first(); b <= judged_last(); ++b) {
    REQUIRE_LT(std::fabs(frame[tm::spectrum + b] - static_cast<float>(tilt[b]) - offset),
               reading_db, "the spectrum reads the input's balance");
    REQUIRE_LT(std::fabs(frame[tm::target + b] - frame[tm::target + keel::reference_band]),
               tolerance::float_processing_f, "the target is flat");
    REQUIRE_LT(
        std::fabs(frame[tm::correction + b] + frame[tm::spectrum + b] - frame[tm::target + b]),
        fit_db, "the correction closes the difference");
  }
  // The newest history slice is the correction a slice ago; the oldest
  // slices, before the render began, read 0 dB.
  const std::size_t newest = tm::history + (keel::history_slices - 1) * keel::bands;
  REQUIRE_LT(std::fabs(frame[newest + judged_first()] - frame[tm::correction + judged_first()]),
             reading_db, "the newest slice is the correction now");
  REQUIRE_EQ(frame[tm::history], 0.0F, "the oldest slice predates the render");
  REQUIRE_GT(frame[tm::input_peak], -tilt_db * keel::bands_per_octave * 2.0F, "the input meter");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "native keel contract passed",
      {
          {"descriptor_registry_and_presets", [] { descriptor_registry_and_presets(); }},
          {"amount_zero_is_bit_exact", [] { amount_zero_is_bit_exact(); }},
          {"tilted_pink_converges_to_the_target", [] { tilted_pink_converges_to_the_target(); }},
          {"speed_sets_the_time_to_follow", [] { speed_sets_the_time_to_follow(); }},
          {"a_steady_programme_is_not_modulated", [] { a_steady_programme_is_not_modulated(); }},
          {"a_moving_correction_has_no_zipper", [] { a_moving_correction_has_no_zipper(); }},
          {"minimum_phase_delays_little", [] { minimum_phase_delays_little(); }},
          {"a_curve_target_is_reached", [] { a_curve_target_is_reached(); }},
          {"a_reference_target_is_reached", [] { a_reference_target_is_reached(); }},
          {"learn_captures_the_balance", [] { learn_captures_the_balance(); }},
          {"regions_weight_the_correction", [] { regions_weight_the_correction(); }},
          {"limits_hold", [] { limits_hold(); }},
          {"absent_bands_are_not_boosted", [] { absent_bands_are_not_boosted(); }},
          {"silence_and_transients_do_not_drag_it",
           [] { silence_and_transients_do_not_drag_it(); }},
          {"level_match_holds_the_loudness", [] { level_match_holds_the_loudness(); }},
          {"delta_plays_the_change", [] { delta_plays_the_change(); }},
          {"output_trims", [] { output_trims(); }},
          {"other_rates_converge_alike", [] { other_rates_converge_alike(); }},
          {"any_block_split_renders_the_same", [] { any_block_split_renders_the_same(); }},
          {"export_equals_live", [] { export_equals_live(); }},
          {"hostile_input_stays_finite", [] { hostile_input_stays_finite(); }},
          {"the_tail_is_bounded", [] { the_tail_is_bounded(); }},
          {"presets_stay_restrained", [] { presets_stay_restrained(); }},
          {"a_seek_starts_clean", [] { a_seek_starts_clean(); }},
          {"the_callback_never_allocates", [] { the_callback_never_allocates(); }},
          {"telemetry_feeds_the_panel", [] { telemetry_feeds_the_panel(); }},
      });
}

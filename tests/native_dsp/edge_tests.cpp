#include "effect_test_support.hpp"
#include "effects/edge_dsp.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/edge.hpp"
#include "oscillate/native_dsp/true_peak.hpp"
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
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

// Edge (ADR-0108, ADR-0113) against measured oracles: a hard clip at the
// ceiling at shape 0, no aliasing above -100 dBFS at 16x, a true peak never
// over the ceiling, a delta that nulls with the output against the aligned
// input at any ceiling, the reported
// latency, stereo link, the level-matched compare, block splits, export
// against live, hostile input, restrained presets, an allocation-free
// callback and the panel's telemetry.
namespace {
using namespace effect_test;
namespace edge = oscillate::native_dsp::effects::edge;

const effects::EffectEngine& engine() { return edge::engine(); }
const EngineDescriptor& table() { return *engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr std::array rates{oscillate::test::sample_rate_44k_hz, oscillate::test::sample_rate_48k_hz,
                           oscillate::test::sample_rate_88k_hz,
                           oscillate::test::sample_rate_96k_hz};
constexpr std::size_t factor_count = edge::factors.size();
constexpr std::size_t filter_count = 2;
constexpr std::size_t x1 = 0;
constexpr std::size_t x8 = 3;
constexpr std::size_t x16 = 4;
constexpr std::size_t linear_phase = 0;
constexpr std::size_t light = 1;
constexpr double one_second = rate;
constexpr double half_second = rate / 2.0;
constexpr double seconds_per_minute = 60.0;
constexpr double ceiling = 1.0; // 0 dBTP
// Lowered ceilings across edge.ceiling's range (-12 to 0 dBFS).
constexpr double lowest_ceiling_db = -12.0;
constexpr double low_ceiling_db = -6.0;
constexpr double near_ceiling_db = -1.0;
constexpr std::array lowered_ceilings_db{lowest_ceiling_db, low_ceiling_db, near_ceiling_db};
const double twelve_db_over = oscillate::native_dsp::dsp::db_to_gain(12.0);
constexpr double alias_floor_db = -100.0;
// A tone well inside every filter's passband.
constexpr double tone_hz = 1'000.0;
// The guard at 48 kHz: 0.5 ms of attack (24 frames) and the true-peak
// interpolation's reach either side (2R - 1 = 31).
constexpr std::uint32_t guard_48k = 24 + 31;
constexpr std::uint32_t resampler_16x_linear = 93;
constexpr std::uint32_t resampler_16x_light = 40;
constexpr double knee_at_one = 0.6;
constexpr double mid_shape = 0.5;

std::vector<double> defaults() { return preset(table(), "Default"); }

double gain(double decibels) { return oscillate::native_dsp::dsp::db_to_gain(decibels); }

std::vector<double> under(std::vector<double> values, double ceiling_db) {
  values[at("edge.ceiling")] = ceiling_db;
  return values;
}

// Called with the named factor and filter indices below.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::vector<double> with(std::size_t oversampling, std::size_t filters, double shape) {
  auto v = defaults();
  v[at("edge.oversampling")] = static_cast<double>(oversampling);
  v[at("edge.filters")] = static_cast<double>(filters);
  v[at("edge.shape")] = shape;
  return v;
}

Stereo render(const std::vector<double>& values, const Stereo& input, std::size_t block = 128,
              double at_rate = rate) {
  auto runtime = make_effect(engine(), values, at_rate);
  return process(*runtime, input, {.block = block, .rate = at_rate});
}

// The source, then silence for the effect's latency and tail, so a meter
// reads the whole output rather than one cut off mid-signal.
Stereo then_silence(Stereo s, const std::vector<double>& values) {
  const auto extra = engine().latency_frames(values, rate) + engine().tail_frames(values, rate) +
                     2 * oscillate::native_dsp::StereoTruePeakMeter::kernel_radius;
  s.left.resize(s.size() + extra, 0.0);
  s.right.resize(s.left.size(), 0.0);
  return s;
}

Stereo scaled(Stereo s, double gain) {
  for (std::size_t i = 0; i < s.size(); ++i) {
    s.left[i] *= gain;
    s.right[i] *= gain;
  }
  return s;
}

// A kick every half second: a pitch sweep from 250 Hz down to 50 Hz with a
// short click, peaking near `level`; the right channel a little quieter.
Stereo kicks(std::size_t frames, double level) {
  constexpr double body_hz = 50.0;
  constexpr double sweep_hz = 200.0;
  constexpr double sweep_rate = 40.0;
  constexpr double decay_rate = 8.0;
  constexpr std::size_t click_frames = 20;
  constexpr double right_gain = 0.9;
  const auto click = noise(1.0, click_frames, true);
  Stereo out{std::vector<double>(frames), std::vector<double>(frames)};
  const auto period = static_cast<std::size_t>(half_second);
  for (std::size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i % period) / rate;
    const double phase = body_hz * t + sweep_hz * (1.0 - std::exp(-sweep_rate * t)) / sweep_rate;
    double x = std::sin(2.0 * std::numbers::pi * phase) * std::exp(-decay_rate * t);
    if (i % period < click_frames) {
      x += click.left[i % period];
    }
    out.left[i] = x;
    out.right[i] = x * right_gain;
  }
  return scaled(out, level / peak(out.left));
}

// A kit: the kicks with noise hats on every eighth.
Stereo kit(std::size_t frames, double level) {
  constexpr double hat_decay_frames = 300.0;
  constexpr double hat_level = 0.6;
  auto out = kicks(frames, 1.0);
  const auto hiss = noise(1.0, frames);
  const auto period = static_cast<std::size_t>(one_second / 4.0);
  for (std::size_t i = 0; i < frames; ++i) {
    const double envelope =
        hat_level * std::exp(-static_cast<double>(i % period) / hat_decay_frames);
    out.left[i] += hiss.left[i] * envelope;
    out.right[i] += hiss.right[i] * envelope;
  }
  return scaled(out, level / std::max(peak(out.left), peak(out.right)));
}

double true_peak(const Stereo& s) {
  oscillate::native_dsp::StereoTruePeakMeter meter;
  REQUIRE_TRUE(meter.prepare(static_cast<std::uint32_t>(rate)), "the meter prepares");
  std::vector<oscillate::engine::StereoFrame> frames;
  for (std::size_t i = 0; i < s.size(); ++i) {
    frames.push_back({static_cast<float>(s.left[i]), static_cast<float>(s.right[i])});
    if (frames.size() == oscillate::native_dsp::StereoTruePeakMeter::max_block_frames) {
      REQUIRE_TRUE(meter.process(frames), "the meter reads");
      frames.clear();
    }
  }
  REQUIRE_TRUE(meter.process(frames) && meter.finish(), "the meter reads");
  const auto reading = meter.reading();
  return std::max(reading.left_true_peak, reading.right_true_peak);
}

void descriptor_registry_and_presets() {
  REQUIRE_EQ(effects::find_engine("edge"), &engine(), "Edge is registered");
  require(!engine().sidechain && !engine().generator, "Edge is a plain insert");
  REQUIRE_EQ(table().parameters.size(), 10, "the table's size");
  REQUIRE_EQ(table().parameters[at("edge.ceiling")].default_value, 0.0, "the ceiling is 0 dBFS");
  REQUIRE_EQ(table().parameters[at("edge.ceiling")].minimum, lowest_ceiling_db, "down to -12");
  for (const auto& p : table().presets) {
    for (const auto& [id, value] : p.values) {
      require(id != "edge.ceiling", "presets stay at 0 dBFS: " + std::string(p.name));
    }
  }
  REQUIRE_EQ(table().presets.size(), 5, "Default and four presets");
  for (const auto* name : {"Default", "Kick Punch", "Drum Bus", "808", "Master"}) {
    const auto out = render(preset(table(), name), noise(0.5, 9'600));
    for (std::size_t i = 0; i < out.size(); ++i) {
      require(std::isfinite(out.left[i]) && std::isfinite(out.right[i]),
              "preset renders finite: " + std::string(name));
    }
  }
  for (const auto* id : {"edge.oversampling", "edge.filters"}) {
    require(!table().parameters[at(id)].automatable, std::string(id) + " sets the latency");
  }
}

// The static curve is clamp at shape 0, linear to the knee, odd, monotonic
// and under the ceiling at every shape. Through the kernel at 1x a 1 kHz
// sine 6 dB over comes out as the clamped sine under the guard's steady
// gain.
void shape_zero_is_a_hard_clip() {
  constexpr std::size_t points = 4'001;
  constexpr double span = 8.0;
  constexpr std::size_t shapes = 8;
  constexpr double shape_step = 1.0 / shapes;
  for (std::size_t i = 0; i < points; ++i) {
    const double x = -span + 2.0 * span * static_cast<double>(i) / (points - 1);
    REQUIRE_EQ(edge::clip(x, 0.0, 0.0), std::clamp(x, -ceiling, ceiling), "shape 0 is a clamp");
    for (const double ceiling_db : lowered_ceilings_db) {
      const double c = gain(ceiling_db);
      REQUIRE_LT(std::fabs(edge::clip(x, 0.0, ceiling_db) - std::clamp(x, -c, c)),
                 tolerance::double_rounding, "shape 0 is a clamp at the ceiling");
    }
  }
  for (std::size_t s = 0; s <= shapes; ++s) {
    const double shape = static_cast<double>(s) * shape_step;
    double previous = -ceiling;
    for (std::size_t i = 0; i < points; ++i) {
      const double x = -span + 2.0 * span * static_cast<double>(i) / (points - 1);
      const double y = edge::clip(x, shape, 0.0);
      REQUIRE_LE(std::fabs(y), ceiling, "under the ceiling");
      REQUIRE_GE(y, previous, "monotonic");
      REQUIRE_EQ(edge::clip(-x, shape, 0.0), -y, "odd");
      REQUIRE_LE(std::fabs(edge::clip(x, shape, low_ceiling_db)),
                 gain(low_ceiling_db) + tolerance::double_rounding, "under a lowered ceiling");
      previous = y;
    }
  }
  constexpr double under_knee = 1.0 - knee_at_one;
  REQUIRE_EQ(edge::clip(under_knee, 1.0, 0.0), under_knee, "linear up to the knee");
  REQUIRE_LT(edge::clip(under_knee + shape_step, 1.0, 0.0), under_knee + shape_step,
             "then rounded");

  // Through the kernel, a sine 6 dB over each ceiling.
  constexpr double six_db_over = 2.0;
  constexpr double least_guard_gain = 0.9;
  for (const double ceiling_db : {0.0, low_ceiling_db, lowest_ceiling_db}) {
    const double c = gain(ceiling_db);
    const auto input = sine(tone_hz, six_db_over * c, static_cast<std::size_t>(one_second));
    const auto v = under(with(x1, linear_phase, 0.0), ceiling_db);
    const auto out = render(v, input);
    const auto latency = engine().latency_frames(v, rate);
    const auto settled = static_cast<std::size_t>(half_second);
    const double guard = peak(out.left, settled) / c;
    REQUIRE_GT(guard, least_guard_gain, "the guard's steady gain");
    REQUIRE_LE(guard, 1.0, "at or under the ceiling");
    double worst = 0.0;
    for (std::size_t i = settled; i < out.size(); ++i) {
      const double expected = guard * std::clamp(input.left[i - latency], -c, c);
      worst = std::max(worst, std::fabs(out.left[i] - expected));
    }
    REQUIRE_LT(worst, tolerance::float_processing,
               "a hard clip at " + std::to_string(ceiling_db) + " dB: " + std::to_string(worst));
  }
}

// The largest spectral line from 20 Hz to 20 kHz away from `hz`, in dBFS,
// through a Kaiser window whose sidelobes sit far under the floor measured.
double worst_spur_db(const std::vector<double>& x, std::size_t from, double hz) {
  constexpr std::size_t size = 65'536;
  constexpr double window_beta = 20.0;
  constexpr double lowest_hz = 20.0;
  constexpr double highest_hz = 20'000.0;
  constexpr double main_lobe_hz = 50.0;
  std::vector<std::complex<double>> spectrum(size);
  double window_sum = 0.0;
  const double half = static_cast<double>(size - 1) / 2.0;
  for (std::size_t i = 0; i < size; ++i) {
    const double w =
        oscillate::native_dsp::dsp::kaiser(static_cast<double>(i) - half, half, window_beta);
    spectrum[i] = x[from + i] * w;
    window_sum += w;
  }
  fft(spectrum);
  double worst = 0.0;
  for (std::size_t bin = 1; bin < size / 2; ++bin) {
    const double f = static_cast<double>(bin) * rate / static_cast<double>(size);
    if (f >= lowest_hz && f <= highest_hz && std::fabs(f - hz) > main_lobe_hz) {
      worst = std::max(worst, 2.0 * std::abs(spectrum[bin]) / window_sum);
    }
  }
  return db(worst);
}

void no_aliasing_at_16x() {
  constexpr double hz = 10'000.0;
  constexpr std::size_t frames = 82'000;
  constexpr std::size_t settled = 12'000;
  const auto input = sine(hz, twelve_db_over, frames);
  const auto out = render(with(x16, linear_phase, 0.0), input);
  const double worst = worst_spur_db(out.left, settled, hz);
  REQUIRE_LT(worst, alias_floor_db, "16x aliases under -100 dBFS: " + std::to_string(worst));
  // 1x folds the clip's harmonics straight back: the measurement sees it.
  const auto folded = render(with(x1, linear_phase, 0.0), input);
  REQUIRE_GT(worst_spur_db(folded.left, settled, hz), alias_floor_db, "1x aliases");
}

void true_peak_never_exceeds_0_dbtp() {
  constexpr std::size_t frames = 24'000;
  const std::array<Stereo, 3> sources{scaled(noise(1.0, frames), twelve_db_over),
                                      kicks(frames, twelve_db_over),
                                      sine(10'000.0, twelve_db_over, frames)};
  for (std::size_t o = 0; o < factor_count; ++o) {
    for (std::size_t f = 0; f < filter_count; ++f) {
      for (const double shape : {0.0, 1.0}) {
        for (const double link : {0.0, 1.0}) {
          auto v = with(o, f, shape);
          v[at("edge.link")] = link;
          for (const auto& source : sources) {
            const double tp = true_peak(render(v, then_silence(source, v)));
            REQUIRE_LE(tp, ceiling,
                       "true peak at or under 0 dBTP: factor " + std::to_string(edge::factors[o]) +
                           ", filters " + std::to_string(f) + ": " + std::to_string(db(tp)));
          }
        }
      }
    }
  }
}

// Under a lowered ceiling the true peak stays at or under it, and the
// output meter reads it there.
void true_peak_never_exceeds_a_lowered_ceiling() {
  constexpr std::size_t frames = 24'000;
  // Sources 12 dB over reach the ceiling: the guard leaves them this close.
  constexpr double reaches_db = 1.0;
  const std::array<Stereo, 3> sources{scaled(noise(1.0, frames), twelve_db_over),
                                      kicks(frames, twelve_db_over),
                                      sine(10'000.0, twelve_db_over, frames)};
  for (const double ceiling_db : lowered_ceilings_db) {
    for (const std::size_t o : {x1, x16}) {
      for (std::size_t f = 0; f < filter_count; ++f) {
        for (const double shape : {0.0, 1.0}) {
          const auto v = under(with(o, f, shape), ceiling_db);
          for (const auto& source : sources) {
            auto runtime = make_effect(engine(), v, rate);
            const auto out = process(*runtime, then_silence(source, v));
            const double tp = true_peak(out);
            REQUIRE_LE(tp, gain(ceiling_db),
                       "true peak at or under " + std::to_string(ceiling_db) + " dBTP: factor " +
                           std::to_string(edge::factors[o]) + ", filters " + std::to_string(f) +
                           ": " + std::to_string(db(tp)));
            REQUIRE_GT(tp, gain(ceiling_db - reaches_db), "and reaches it");
            std::vector<float> frame;
            REQUIRE_TRUE(runtime->read_telemetry(frame), "a telemetry frame");
            REQUIRE_LE(frame[edge::telemetry::output_peak], static_cast<float>(ceiling_db),
                       "the output meter reads under the ceiling");
          }
        }
      }
    }
  }
}

// While the ceiling glides, the output is the guarded signal times the
// ceiling it went in under: its true peak never passes the highest ceiling
// the glide moved through.
void true_peak_holds_while_the_ceiling_glides() {
  constexpr std::size_t frames = 48'000;
  // Each ceiling holds 100 ms: long enough to settle after the glide and
  // the latency, and be measured under the lower one.
  constexpr std::size_t glide_every = 4'800;
  // The ceiling glides over its parameter's glide time, adopted at the
  // next control tick.
  const auto settle =
      static_cast<std::size_t>(table().parameters[at("edge.ceiling")].glide_seconds * rate +
                               oscillate::native_dsp::control_interval_frames);
  const std::array<Stereo, 2> sources{scaled(noise(1.0, frames), twelve_db_over),
                                      sine(10'000.0, twelve_db_over, frames)};
  for (const std::size_t o : {x1, x16}) {
    for (std::size_t f = 0; f < filter_count; ++f) {
      for (const double shape : {0.0, 1.0}) {
        const auto high = under(with(o, f, shape), near_ceiling_db);
        const auto low = under(with(o, f, shape), low_ceiling_db);
        for (const auto& source : sources) {
          auto runtime = make_effect(engine(), high, rate);
          RenderOptions options;
          for (std::uint64_t at_frame = glide_every; at_frame < frames; at_frame += glide_every)
            options.at.push_back(at_frame);
          std::uint64_t offers = 0;
          options.before = [&](std::uint64_t at_frame) {
            require(
                runtime->offer_values((at_frame / glide_every) % 2 == 1 ? low : high, ++offers + 1),
                "the ceiling moves");
          };
          const auto out = process(*runtime, then_silence(source, high), options);
          const double tp = true_peak(out);
          // Settled under the lower ceiling, the output stays under it.
          const auto latency = engine().latency_frames(high, rate);
          for (std::size_t from = glide_every; from < frames; from += 2 * glide_every) {
            double settled = 0.0;
            for (std::size_t i = from + latency + settle; i < from + glide_every + latency; ++i)
              settled = std::max({settled, std::fabs(out.left[i]), std::fabs(out.right[i])});
            REQUIRE_LE(settled, gain(low_ceiling_db),
                       "under the lower ceiling once settled: factor " +
                           std::to_string(edge::factors[o]) + ", filters " + std::to_string(f) +
                           ": " + std::to_string(db(settled)));
          }
          REQUIRE_LE(tp, gain(near_ceiling_db),
                     "true peak under the highest ceiling while it glides: factor " +
                         std::to_string(edge::factors[o]) + ", filters " + std::to_string(f) +
                         ": " + std::to_string(db(tp)));
        }
      }
    }
  }
}

// Delta is the driven input, delayed by the latency, less the output, both
// after Output gain: output + delta is the input times both gains.
void delta_nulls_with_the_output() {
  constexpr double input_db = 6.0;
  constexpr double output_db = -3.0;
  const auto input = kit(static_cast<std::size_t>(one_second), 1.0);
  for (const double ceiling_db : {0.0, low_ceiling_db}) {
    for (std::size_t o = 0; o < factor_count; ++o) {
      auto v = under(with(o, linear_phase, mid_shape), ceiling_db);
      v[at("edge.input")] = input_db;
      v[at("edge.output")] = output_db;
      const auto out = render(v, input);
      v[at("edge.delta")] = 1.0;
      const auto removed = render(v, input);
      const auto latency = engine().latency_frames(v, rate);
      const double both = gain(input_db + output_db);
      double worst = 0.0;
      for (std::size_t i = latency; i < input.size(); ++i) {
        for (const auto channel : {&Stereo::left, &Stereo::right}) {
          const double sum = (out.*channel)[i] + (removed.*channel)[i];
          worst = std::max(worst, std::fabs(sum - both * (input.*channel)[i - latency]));
        }
      }
      REQUIRE_LT(worst, tolerance::float_processing,
                 "output + delta = the aligned input: " + std::to_string(worst));
      REQUIRE_GT(peak(removed.left), tolerance::coarse, "and the clipper removed something");
    }
  }
}

// Unclipped audio comes out exactly delayed by the reported latency (to the
// filters' ripple and, from 8x, the antialiasing's half-sample mean), and
// every factor's latency differs, so any change of it rebuilds the route.
void latency_is_reported_and_exact() {
  constexpr double quiet = 0.25;
  constexpr double ripple = 1e-4;
  const auto input = sine(tone_hz, quiet, static_cast<std::size_t>(one_second / 4.0));
  for (std::size_t o = 0; o < factor_count; ++o) {
    for (std::size_t f = 0; f < filter_count; ++f) {
      const auto v = with(o, f, 1.0);
      const auto latency = engine().latency_frames(v, rate);
      REQUIRE_EQ(latency, edge::latency_frames(o, f, rate), "the engine reports it");
      const auto out = render(v, input);
      double worst = 0.0;
      for (std::size_t i = 2 * latency + 1; i < input.size(); ++i) {
        worst = std::max(worst, std::fabs(out.left[i] - input.left[i - latency]));
      }
      REQUIRE_LT(worst, ripple,
                 "delayed exactly by the latency: factor " + std::to_string(edge::factors[o]) +
                     ": " + std::to_string(worst));
    }
  }
  REQUIRE_EQ(edge::latency_frames(x1, linear_phase, rate), guard_48k, "1x: the guard alone");
  REQUIRE_EQ(edge::latency_frames(x16, linear_phase, rate), guard_48k + resampler_16x_linear,
             "16x linear phase");
  REQUIRE_EQ(edge::latency_frames(x16, light, rate), guard_48k + resampler_16x_light, "16x light");
  for (const double r : rates) {
    std::set<std::uint32_t> seen;
    for (std::size_t o = 1; o < factor_count; ++o) {
      for (std::size_t f = 0; f < filter_count; ++f) {
        require(seen.insert(edge::latency_frames(o, f, r)).second,
                "each oversampled setting has its own latency");
      }
    }
    require(!seen.contains(edge::latency_frames(x1, linear_phase, r)), "and 1x another");
  }
}

// Linked, the quiet channel takes the loud one's gain: the image holds.
// Unlinked, a channel under the knee passes untouched.
void stereo_link_shares_the_gain() {
  constexpr double loud = 2.0;
  constexpr double quiet = 0.5;
  // Unlinked the quiet side passes nearly whole; linked it takes the loud
  // side's half.
  constexpr double kept = 0.9;
  constexpr double taken = 0.6;
  Stereo input = sine(tone_hz, loud, static_cast<std::size_t>(half_second));
  for (std::size_t i = 0; i < input.size(); ++i) {
    input.right[i] = input.left[i] * quiet / loud;
  }
  const auto linked = render(with(x1, linear_phase, 0.0), input);
  const std::size_t settled = input.size() / 2;
  for (std::size_t i = settled; i < input.size(); ++i) {
    REQUIRE_LT(std::fabs(linked.right[i] - linked.left[i] * quiet / loud),
               tolerance::float_round_trip, "linked: one gain for both");
  }
  auto v = with(x1, linear_phase, 0.0);
  v[at("edge.link")] = 0.0;
  const auto separate = render(v, input);
  REQUIRE_GT(peak(separate.right, settled), kept * quiet,
             "unlinked: the quiet side keeps its peaks");
  REQUIRE_LT(peak(linked.right, settled), taken * quiet, "linked: it does not");
}

// Compare plays the aligned input, at the output's level when matched.
void compare_is_level_matched() {
  constexpr double drive_db = 9.0;
  constexpr double matched_db = 0.5;
  const auto input = kit(static_cast<std::size_t>(4.0 * one_second), 1.0);
  auto v = defaults();
  v[at("edge.input")] = drive_db;
  const auto out = render(v, input);
  v[at("edge.compare")] = 1.0;
  const auto matched = render(v, input);
  const std::size_t from = static_cast<std::size_t>(2.0 * one_second);
  const double out_rms = rms(std::vector<double>(out.left.begin() + from, out.left.end()));
  const double compare_rms =
      rms(std::vector<double>(matched.left.begin() + from, matched.left.end()));
  REQUIRE_LT(std::fabs(db(compare_rms / out_rms)), matched_db,
             "matched compare is as loud as the output: " +
                 std::to_string(db(compare_rms / out_rms)));
  v[at("edge.match")] = 0.0;
  const auto plain = render(v, input);
  const auto latency = engine().latency_frames(v, rate);
  for (std::size_t i = latency; i < input.size(); ++i) {
    REQUIRE_LT(std::fabs(plain.left[i] - input.left[i - latency]), tolerance::float_round_trip,
               "unmatched compare is the aligned input");
  }
}

// ADR-0125: the oversampler a block at a time is each stage's direct
// convolution, sample by sample in tap order, bit for bit, at every factor
// and filter and for any block split.
namespace direct {
namespace edge_dsp = oscillate::native_dsp::effects::edge_dsp;

// sum over i of taps[i] * x[k - i] with zeros before the start, in tap order.
double dot(std::span<const double> taps, const std::vector<double>& x, std::size_t k) {
  double sum = 0.0;
  for (std::size_t i = 0; i < taps.size(); ++i) {
    sum += taps[i] * (i <= k ? x[k - i] : 0.0);
  }
  return sum;
}

double back(const std::vector<double>& x, std::size_t k, std::size_t delay) {
  return delay <= k ? x[k - delay] : 0.0;
}

std::vector<double> up(const edge_dsp::Design& design, std::vector<double> x) {
  for (std::size_t s = 0; s < design.stages; ++s) {
    const auto& halfband = design.halfbands[s];
    std::vector<double> y(2 * x.size());
    for (std::size_t k = 0; k < x.size(); ++k) {
      y[2 * k] = 2.0 * dot(halfband.even, x, k);
      y[2 * k + 1] = back(x, k, halfband.centre);
    }
    x = std::move(y);
  }
  return x;
}

std::vector<double> down(const edge_dsp::Design& design, std::vector<double> x) {
  std::vector<double> padded(x.size());
  for (std::size_t k = 0; k < x.size(); ++k) {
    padded[k] = back(x, k, design.pad);
  }
  x = std::move(padded);
  for (std::size_t s = design.stages; s-- > 0;) {
    const bool top = s + 1 == design.stages && design.antialiased;
    const auto& halfband = design.halfbands[s];
    std::vector<double> firsts(x.size() / 2);
    std::vector<double> seconds(x.size() / 2);
    for (std::size_t k = 0; k < firsts.size(); ++k) {
      firsts[k] = x[2 * k];
      seconds[k] = x[2 * k + 1];
    }
    std::vector<double> y(firsts.size());
    for (std::size_t k = 0; k < y.size(); ++k) {
      if (top) {
        // The even-length low-pass over the plain stream, at each first.
        y[k] = dot(design.top, x, 2 * k);
      } else {
        y[k] = dot(halfband.even, firsts, k) + 0.5 * back(seconds, k, halfband.centre + 1);
      }
    }
    x = std::move(y);
  }
  return x;
}
} // namespace direct

void the_block_resampler_is_the_direct_filters() {
  namespace edge_dsp = oscillate::native_dsp::effects::edge_dsp;
  constexpr std::size_t frames = 1'000;
  constexpr double level = 0.9;
  // Block sizes in turn, up to the resampler's largest.
  constexpr std::array<std::size_t, 5> splits{1, 7, 32, 5, 19};
  const auto input = noise(level, frames).left;
  for (std::size_t o = 0; o < factor_count; ++o) {
    for (std::size_t f = 0; f < filter_count; ++f) {
      const auto& design = edge_dsp::design(o, f);
      const std::string name = std::to_string(design.factor) + "x, filter " + std::to_string(f);
      const auto expected_up = direct::up(design, input);
      // Down from a top-rate stream of its own: the curve's output.
      const auto top_input = noise(level, frames * design.factor, false, 99).left;
      const auto expected_down = direct::down(design, top_input);
      for (std::size_t first = 0; first < splits.size(); ++first) {
        edge_dsp::Resampler resampler;
        resampler.prepare(design);
        std::vector<double> got_up(frames * design.factor);
        std::vector<double> got_down(frames);
        std::array<double, edge_dsp::block_frames * edge_dsp::max_factor> top{};
        std::size_t done = 0;
        for (std::size_t block = first; done < frames; ++block) {
          const std::size_t n = std::min(splits[block % splits.size()], frames - done);
          resampler.up(std::span(input).subspan(done, n), top.data());
          std::copy_n(top.begin(), n * design.factor,
                      got_up.begin() + static_cast<std::ptrdiff_t>(done * design.factor));
          std::copy_n(top_input.begin() + static_cast<std::ptrdiff_t>(done * design.factor),
                      n * design.factor, top.begin());
          resampler.down(top.data(), std::span(got_down).subspan(done, n));
          done += n;
        }
        require(got_up == expected_up, name + ": up is the direct filters, bit for bit");
        require(got_down == expected_down, name + ": down is the direct filters, bit for bit");
      }
    }
  }
}

void any_block_split_renders_the_same() {
  constexpr double hot = 2.0;
  const auto input = kit(20'000, hot);
  for (const auto* name : {"Default", "Kick Punch", "808", "Master"}) {
    for (const double ceiling_db : {0.0, low_ceiling_db}) {
      auto v = under(preset(table(), name), ceiling_db);
      v[at("edge.link")] = 0.0;
      const auto a = render(v, input, 128);
      const auto b = render(v, input, 0);
      for (std::size_t i = 0; i < input.size(); ++i) {
        require(a.left[i] == b.left[i] && a.right[i] == b.right[i],
                std::string(name) + ": the split changed frame " + std::to_string(i));
      }
    }
  }
}

// Export renders in long slices on its own transport generation, stopped or
// at any tempo; live in callback blocks while playing. The audio is the same.
void export_equals_live() {
  constexpr std::size_t export_block = 4'096;
  constexpr double live_bpm = 124.0;
  constexpr double hot = 2.0;
  const auto input = kit(static_cast<std::size_t>(one_second), hot);
  for (const auto* name : {"Kick Punch", "Drum Bus", "808", "Master"}) {
    for (const double ceiling_db : {0.0, near_ceiling_db}) {
      const auto v = under(preset(table(), name), ceiling_db);
      auto live = make_effect(engine(), v, rate);
      const auto played = process(*live, input, {.block = 128, .bpm = live_bpm});
      auto offline = make_effect(engine(), v, rate);
      const auto exported =
          process(*offline, input, {.block = export_block, .generation = 2, .bpm = 0.0});
      require(played.left == exported.left && played.right == exported.right,
              std::string(name) + ": export equals live");
    }
  }
}

void hostile_input_stays_finite() {
  Stereo input = noise(0.5, 9'600);
  input.left[100] = std::numeric_limits<double>::quiet_NaN();
  input.right[200] = std::numeric_limits<double>::infinity();
  input.left[300] = 1e30;
  for (std::size_t i = 4'000; i < input.size(); ++i) {
    input.left[i] = input.right[i] = 1e-30;
  }
  for (std::size_t o = 0; o < factor_count; ++o) {
    const auto out = render(with(o, linear_phase, mid_shape), input);
    for (std::size_t i = 0; i < out.size(); ++i) {
      require(std::isfinite(out.left[i]) && std::isfinite(out.right[i]), "finite output");
      REQUIRE_LE(std::fabs(out.left[i]), ceiling, "under the ceiling");
    }
  }
}

// An 808 every half second: a 45 Hz sine dropping a little in pitch and
// decaying over the half second, peaking at `level`.
Stereo booms(std::size_t frames, double level) {
  constexpr double hz = 45.0;
  constexpr double drop_hz = 10.0;
  constexpr double drop_rate = 10.0;
  constexpr double decay_rate = 3.0;
  Stereo out{std::vector<double>(frames), std::vector<double>(frames)};
  const auto period = static_cast<std::size_t>(half_second);
  for (std::size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i % period) / rate;
    const double phase = hz * t + drop_hz * (1.0 - std::exp(-drop_rate * t)) / drop_rate;
    out.left[i] = out.right[i] =
        std::sin(2.0 * std::numbers::pi * phase) * std::exp(-decay_rate * t);
  }
  return scaled(out, level / peak(out.left));
}

// Each preset on the source it is for, peaking at -1 dBFS (a mix: the kit
// over the 808), clips a few dB at most and stays under 0 dBTP.
void presets_stay_restrained() {
  constexpr double source_peak = 0.891; // -1 dBFS
  constexpr double most_clipped_db = 4.0;
  constexpr double least_clipped_db = 0.5;
  const auto frames = static_cast<std::size_t>(2.0 * one_second);
  Stereo mix = kit(frames, 1.0);
  const auto low = booms(frames, 1.0);
  for (std::size_t i = 0; i < frames; ++i) {
    mix.left[i] += low.left[i];
    mix.right[i] += low.right[i];
  }
  mix = scaled(mix, source_peak / std::max(peak(mix.left), peak(mix.right)));
  const std::array<std::pair<const char*, Stereo>, 4> cases{{
      {"Kick Punch", kicks(frames, source_peak)},
      {"Drum Bus", kit(frames, source_peak)},
      {"808", booms(frames, source_peak)},
      {"Master", mix},
  }};
  for (const auto& [name, input] : cases) {
    const auto values = preset(table(), name);
    auto runtime = make_effect(engine(), values, rate);
    const auto out = process(*runtime, then_silence(input, values));
    std::vector<float> frame;
    REQUIRE_TRUE(runtime->read_telemetry(frame), "a telemetry frame");
    double most = 0.0;
    for (std::size_t b = 0; b < edge::history_beats; ++b) {
      most = std::max(most, static_cast<double>(frame[edge::telemetry::history + b]));
    }
    REQUIRE_LT(most, most_clipped_db,
               std::string(name) + " clips a few dB at most: " + std::to_string(most));
    REQUIRE_GT(most, least_clipped_db, std::string(name) + " clips: " + std::to_string(most));
    REQUIRE_LE(true_peak(out), ceiling, std::string(name) + " stays under 0 dBTP");
  }
}

// A seek (a new transport generation) clears every history: linked at 16x,
// the audio after it is what a fresh Edge plays from the same point.
void a_seek_starts_clean() {
  constexpr double hot = 2.0;
  const auto before = kit(static_cast<std::size_t>(half_second), hot);
  const auto after = scaled(noise(1.0, static_cast<std::size_t>(half_second), true, 99), hot);
  const auto v = with(x16, linear_phase, mid_shape);
  auto seeked = make_effect(engine(), v, rate);
  (void)process(*seeked, before);
  const auto resumed = process(*seeked, after, {.generation = 2});
  const auto fresh = render(v, after);
  require(resumed.left == fresh.left && resumed.right == fresh.right,
          "after a seek Edge plays as a fresh one");
}

void the_callback_never_allocates() {
  const auto values = preset(table(), "Master");
  auto runtime = make_effect(engine(), values, rate);
  (void)process(*runtime, noise(0.3, 4'096));
  auto changed = values;
  changed[at("edge.shape")] = 0.0;
  changed[at("edge.link")] = 0.0;
  changed[at("edge.delta")] = 1.0;
  changed[at("edge.ceiling")] = low_ceiling_db;
  require(runtime->offer_values(changed, 1), "new values are offered");
  std::vector<engine::StereoFrame> buffer(128, {0.9F, -0.9F});
  counted_allocations = 0;
  counting_allocations = true;
  for (std::uint64_t block = 0; block < 2'000; ++block) {
    const Transport transport{0.0, 0.0, 0.0, 4.0};
    (void)runtime->process(buffer, {}, 4'096 + block * buffer.size(), block < 1'000 ? 1U : 2U,
                           transport, {});
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "the callback allocates nothing");
}

void telemetry_feeds_the_panel() {
  constexpr double drive_db = 6.0;
  constexpr double beats = 8.0;
  constexpr double bpm = 120.0;
  auto v = defaults();
  v[at("edge.input")] = drive_db;
  auto runtime = make_effect(engine(), v, rate);
  // Read just after a hit, while the held peaks are fresh.
  constexpr double after_hit_seconds = 0.01;
  const auto frames =
      static_cast<std::size_t>((beats * seconds_per_minute / bpm + after_hit_seconds) * one_second);
  (void)process(*runtime, kit(frames, 1.0), {.bpm = bpm});
  std::vector<float> frame;
  REQUIRE_TRUE(runtime->read_telemetry(frame), "a telemetry frame");
  REQUIRE_EQ(frame.size(), edge::telemetry::size, "the layout's size");
  namespace t = edge::telemetry;
  constexpr float fresh_input_db = 4.0F;
  constexpr float near_ceiling_db = -1.0F;
  constexpr float clipped_db = 3.0F;
  constexpr double milliseconds = 1'000.0;
  REQUIRE_GT(frame[t::input_peak], fresh_input_db, "the driven input peak");
  REQUIRE_LE(frame[t::output_peak], 0.0F, "the output true peak");
  REQUIRE_GT(frame[t::output_peak], near_ceiling_db, "the output true peak");
  REQUIRE_GT(frame[t::clipped], clipped_db, "clipping");
  REQUIRE_GE(frame[t::dot_input], frame[t::dot_output], "the dot sits on the curve");
  REQUIRE_LE(frame[t::dot_output], 0.0F, "under the ceiling");
  const double latency_ms =
      static_cast<double>(engine().latency_frames(v, rate)) / rate * milliseconds;
  REQUIRE_LT(std::fabs(frame[t::latency_ms] - latency_ms), tolerance::coarse, "the latency");
  REQUIRE_EQ(frame[t::factor], static_cast<float>(edge::factors[x8]), "8x by default");
  std::size_t filed = 0;
  for (std::size_t b = 0; b < edge::history_beats; ++b) {
    filed += frame[t::history + b] > 0.0F ? std::size_t{1} : std::size_t{0};
  }
  REQUIRE_GE(filed, 6, "a clipping reading for each beat played");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "native edge contract passed",
      {
          {"descriptor_registry_and_presets", [] { descriptor_registry_and_presets(); }},
          {"shape_zero_is_a_hard_clip", [] { shape_zero_is_a_hard_clip(); }},
          {"no_aliasing_at_16x", [] { no_aliasing_at_16x(); }},
          {"true_peak_never_exceeds_0_dbtp", [] { true_peak_never_exceeds_0_dbtp(); }},
          {"true_peak_never_exceeds_a_lowered_ceiling",
           [] { true_peak_never_exceeds_a_lowered_ceiling(); }},
          {"true_peak_holds_while_the_ceiling_glides",
           [] { true_peak_holds_while_the_ceiling_glides(); }},
          {"delta_nulls_with_the_output", [] { delta_nulls_with_the_output(); }},
          {"latency_is_reported_and_exact", [] { latency_is_reported_and_exact(); }},
          {"stereo_link_shares_the_gain", [] { stereo_link_shares_the_gain(); }},
          {"compare_is_level_matched", [] { compare_is_level_matched(); }},
          {"any_block_split_renders_the_same", [] { any_block_split_renders_the_same(); }},
          {"the_block_resampler_is_the_direct_filters",
           [] { the_block_resampler_is_the_direct_filters(); }},
          {"export_equals_live", [] { export_equals_live(); }},
          {"hostile_input_stays_finite", [] { hostile_input_stays_finite(); }},
          {"presets_stay_restrained", [] { presets_stay_restrained(); }},
          {"a_seek_starts_clean", [] { a_seek_starts_clean(); }},
          {"the_callback_never_allocates", [] { the_callback_never_allocates(); }},
          {"telemetry_feeds_the_panel", [] { telemetry_feeds_the_panel(); }},
      });
}

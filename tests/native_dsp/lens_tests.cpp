#include "effect_test_support.hpp"

#include "oscillate/native_dsp/effects/lens.hpp"
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
#include <exception>
#include <iostream>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Lens (ADR-0042) against exact oracles: audio untouched; sines read their
// level within 0.1 dB from 25 Hz to 18 kHz at 44.1, 48 and 96 kHz; pink
// noise flat untilted, tilt exact; close low tones resolved; highs faster
// than lows; peak hold holds and decays at its rate; views, the sidechain
// overlay, resonances with frequency and level, infinite averaging and
// freeze; telemetry independent of block size; the tonal meter and match
// settings (ADR-0120).
namespace {
using namespace effect_test;
namespace lens = oscillate::native_dsp::effects::lens;

const EngineDescriptor& table() { return *lens::engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }

std::vector<double> untilted() {
  auto values = preset(table(), "Mix");
  values[at("lens.tilt")] = 0;
  return values;
}

std::vector<float> analyse(const std::vector<double>& values, const Stereo& input,
                           double rate = oscillate::test::sample_rate_48k_hz,
                           const Stereo* side = nullptr, std::size_t block = 128) {
  auto runtime = make_effect(lens::engine(), values, rate);
  (void)process(*runtime, input, {.block = block, .sidechain = side, .rate = rate});
  std::vector<float> t;
  REQUIRE_TRUE(runtime->read_telemetry(t), "telemetry");
  REQUIRE_EQ(t.size(), lens::telemetry::size, "telemetry");
  return t;
}

// The highest reading within a 24th of an octave of `hz`.
double level_at(const std::vector<float>& t, std::size_t offset, double hz) {
  double best = -300.0;
  for (std::size_t j = 0; j < lens::points; ++j) {
    if (std::fabs(std::log2(lens::point_hz(j) / hz)) < 1.0 / 24.0) {
      best = std::max(best, static_cast<double>(t[offset + j]));
    }
  }
  return best;
}

std::size_t point_near(double hz);

// A display point's own frequency near `hz`: where the narrow reading
// shows a sine's peak (between points it shows the lobe's flank; the
// resonance finder interpolates).
double on_point(double hz);

std::size_t point_near(double hz) {
  std::size_t best = 0;
  for (std::size_t j = 0; j < lens::points; ++j) {
    if (std::fabs(std::log(lens::point_hz(j) / hz)) <
        std::fabs(std::log(lens::point_hz(best) / hz))) {
      best = j;
    }
  }
  return best;
}

double on_point(double hz) { return lens::point_hz(point_near(hz)); }

Stereo add(Stereo a, const Stereo& b) {
  for (std::size_t i = 0; i < a.size(); ++i) {
    a.left[i] += b.left[i];
    a.right[i] += b.right[i];
  }
  return a;
}

Stereo slice(const Stereo& s, std::size_t from, std::size_t to) {
  return {std::vector<double>(s.left.begin() + static_cast<std::ptrdiff_t>(from),
                              s.left.begin() + static_cast<std::ptrdiff_t>(to)),
          std::vector<double>(s.right.begin() + static_cast<std::ptrdiff_t>(from),
                              s.right.begin() + static_cast<std::ptrdiff_t>(to))};
}

// Pink noise: white noise's spectrum shaped by 1 / sqrt(f), mono.
Stereo pink(std::size_t frames, std::uint32_t seed = 7) {
  std::size_t n = 1;
  while (n < frames) {
    n *= 2;
  }
  const auto white = noise(1.0, n, true, seed);
  std::vector<std::complex<double>> w(n);
  for (std::size_t i = 0; i < n; ++i) {
    w[i] = white.left[i];
  }
  fft(w);
  for (std::size_t k = 1; k < n; ++k) {
    w[k] /= std::sqrt(static_cast<double>(std::min(k, n - k)));
  }
  w[0] = 0.0;
  for (auto& v : w) {
    v = std::conj(v);
  }
  fft(w);
  Stereo out{std::vector<double>(frames), std::vector<double>(frames)};
  double top = 0.0;
  for (std::size_t i = 0; i < frames; ++i) {
    out.left[i] = w[i].real();
    top = std::max(top, std::fabs(out.left[i]));
  }
  for (std::size_t i = 0; i < frames; ++i) {
    out.left[i] *= 0.3 / top;
    out.right[i] = out.left[i];
  }
  return out;
}

void audio_passes_untouched() {
  const auto input = noise(0.5, 48'000);
  const auto side = sine(60.0, 0.5, 48'000);
  auto runtime = make_effect(lens::engine(), preset(table(), "Reference Match"));
  const auto out = process(*runtime, input, {.sidechain = &side});
  for (std::size_t i = 0; i < input.size(); ++i) {
    REQUIRE_EQ(out.left[i], static_cast<double>(static_cast<float>(input.left[i])),
               "Lens never touches the audio");
    REQUIRE_EQ(out.right[i], static_cast<double>(static_cast<float>(input.right[i])),
               "Lens never touches the audio");
  }
}

void sines_read_their_level() {
  for (const double rate : {44'100.0, 48'000.0, 96'000.0}) {
    for (const double nominal : {25.0, 50.0, 100.0, 440.0, 1'000.0, 5'000.0, 12'000.0, 18'000.0}) {
      const double hz = on_point(nominal);
      for (const double amplitude : {0.5, 0.01}) {
        const auto frames = static_cast<std::size_t>(rate * 1.5);
        const auto t = analyse(untilted(), sine(hz, amplitude, frames, rate), rate);
        const double reading = level_at(t, lens::telemetry::curve_a, hz);
        REQUIRE_LT(std::fabs(reading - db(amplitude)), 0.1,
                   "a " + std::to_string(hz) + " Hz sine at " + std::to_string(rate) +
                       " Hz reads " + std::to_string(reading) + " dB, true " +
                       std::to_string(db(amplitude)));
      }
    }
  }
}

// Below where a window resolves the smoothing band, the band widens to the
// window's resolution: sines stay exact, noise reads more. So each
// resolution is flat from where it resolves a third of an octave.
void pink_noise_is_flat_and_tilt_is_exact() {
  const auto input = pink(std::size_t{12} * 48'000);
  std::vector<float> balanced;
  // Tolerances allow for the estimate itself: twelve seconds of noise in a
  // 9 Hz band at 40 Hz is only about a hundred independent measurements.
  struct Case {
    double resolution;
    double from;
    double tolerance;
  };
  for (const auto& [resolution, from, tolerance] :
       std::vector<Case>{{1.0, 80.0, 0.75}, {2.0, 40.0, 1.25}}) {
    auto values = untilted();
    values[at("lens.average")] = 3.0;   // Infinite
    values[at("lens.smoothing")] = 3.0; // A third of an octave
    values[at("lens.resolution")] = resolution;
    const auto t = analyse(values, input);
    double low = 1e9;
    double high = -1e9;
    for (std::size_t j = 0; j < lens::points; ++j) {
      const double hz = lens::point_hz(j);
      if (hz >= from && hz <= 19'000.0) {
        low = std::min(low, static_cast<double>(t[lens::telemetry::curve_a + j]));
        high = std::max(high, static_cast<double>(t[lens::telemetry::curve_a + j]));
      }
    }
    std::cout << "  pink noise spread " << from << " Hz-19 kHz at resolution " << resolution << ": "
              << high - low << " dB\n";
    REQUIRE_LT(high - low, tolerance, "pink noise is flat untilted");
    if (resolution == 1.0) {
      balanced = t;
    }
  }
  auto values = untilted();
  values[at("lens.average")] = 3.0;
  values[at("lens.smoothing")] = 3.0;
  values[at("lens.tilt")] = 2.0; // 3 dB per octave
  const auto tilted = analyse(values, input);
  for (const double hz : {100.0, 1'000.0, 8'000.0}) {
    const auto j = point_near(hz);
    const double change =
        tilted[lens::telemetry::curve_a + j] - balanced[lens::telemetry::curve_a + j];
    const double expected = 3.0 * std::log2(lens::point_hz(j) / 1'000.0);
    REQUIRE_LT(std::fabs(change - expected), oscillate::test::tolerance::coarse,
               "tilt pivots on 1 kHz at 3 dB per octave");
  }
}

// The dip between two equal sines, below the lower peak.
double dip_db(const std::vector<float>& t, double a, double b) {
  const double top =
      std::min(level_at(t, lens::telemetry::curve_a, a), level_at(t, lens::telemetry::curve_a, b));
  double low = 1e9;
  for (std::size_t j = point_near(a); j <= point_near(b); ++j) {
    low = std::min(low, static_cast<double>(t[lens::telemetry::curve_a + j]));
  }
  return top - low;
}

void close_low_tones_resolve() {
  auto values = untilted();
  values[at("lens.smoothing")] = 0.0;
  const auto pair =
      add(sine(40.0, 0.3, std::size_t{2} * 48'000), sine(52.0, 0.3, std::size_t{2} * 48'000));
  const double balanced = dip_db(analyse(values, pair), 40.0, 52.0);
  values[at("lens.resolution")] = 2.0; // Precise
  const auto close =
      add(sine(30.0, 0.3, std::size_t{3} * 48'000), sine(36.0, 0.3, std::size_t{3} * 48'000));
  const double precise = dip_db(analyse(values, close), 30.0, 36.0);
  std::cout << "  resolution: 40/52 Hz dip " << balanced << " dB (Balanced), 30/36 Hz dip "
            << precise << " dB (Precise)\n";
  REQUIRE_GT(balanced, 6.0, "Balanced resolves 40 and 52 Hz");
  REQUIRE_GT(precise, 6.0, "Precise resolves 30 and 36 Hz");
}

// Milliseconds from a tone's start until a point reads within 3 dB of its
// final level.
double rise_ms(double hz) {
  auto values = untilted();
  values[at("lens.average")] = 0.0; // Fast
  values[at("lens.hold")] = 0.0;
  auto runtime = make_effect(lens::engine(), values);
  Stereo input{std::vector<double>(48'000), std::vector<double>(48'000)};
  const auto tone = sine(hz, 0.5, 48'000);
  for (std::size_t i = 24'000; i < 48'000; ++i) {
    input.left[i] = tone.left[i - 24'000];
    input.right[i] = tone.right[i - 24'000];
  }
  std::vector<engine::StereoFrame> buffer(64);
  for (std::size_t position = 0; position < input.size(); position += 64) {
    for (std::size_t i = 0; i < 64; ++i) {
      buffer[i] = {static_cast<float>(input.left[position + i]),
                   static_cast<float>(input.right[position + i])};
    }
    (void)runtime->process(buffer, {}, position, 1, Transport{0, 0, 0, 4}, {});
    std::vector<float> t;
    if (position >= 24'000 && runtime->read_telemetry(t) &&
        level_at(t, lens::telemetry::curve_a, hz) > db(0.5) - 3.0) {
      return static_cast<double>(position + 64 - 24'000) / 48.0;
    }
  }
  return 1e9;
}

void highs_follow_faster_than_lows() {
  const double high = rise_ms(5'000.0);
  const double low = rise_ms(50.0);
  std::cout << "  rise to -3 dB: 5 kHz " << high << " ms, 50 Hz " << low << " ms\n";
  REQUIRE_LT(high, 50.0, "highs follow a transient within a few hops");
  REQUIRE_GT(low, high, "while lows trade time for resolution");
}

void peak_hold_holds_and_decays() {
  auto values = untilted();
  values[at("lens.average")] = 0.0;
  values[at("lens.hold_decay")] = 12.0;
  auto runtime = make_effect(lens::engine(), values);
  auto input = sine(on_point(1'000.0), 0.5, std::size_t{2} * 48'000);
  std::fill(input.left.begin() + 48'000, input.left.end(), 0.0);
  std::fill(input.right.begin() + 48'000, input.right.end(), 0.0);
  (void)process(*runtime, slice(input, 0, 48'000));
  std::vector<float> t;
  require(runtime->read_telemetry(t), "telemetry");
  const double held = level_at(t, lens::telemetry::hold, 1'000.0);
  REQUIRE_LT(std::fabs(held - db(0.5)), 0.15, "the hold catches the peak");
  (void)process(*runtime, slice(input, 48'000, std::size_t{2} * 48'000), {}, 48'000);
  require(runtime->read_telemetry(t), "telemetry");
  const double later = level_at(t, lens::telemetry::hold, 1'000.0);
  std::cout << "  peak hold fell " << held - later << " dB in 1 s at 12 dB/s\n";
  REQUIRE_LT(std::fabs((held - later) - 12.0), 1.0, "and falls 12 dB a second");
  REQUIRE_LT(level_at(t, lens::telemetry::curve_a, 1'000.0), -60.0,
             "while the live curve falls away");
}

void views_and_overlay_read_true() {
  // A left-only sine: left reads it, mid and side half of it.
  // Two seconds: the Medium average (300 ms) settles well within it.
  const double hz = on_point(300.0);
  auto input = sine(hz, 0.5, std::size_t{2} * 48'000);
  std::ranges::fill(input.right, 0.0);
  for (const auto& [view, expected] :
       std::vector<std::pair<double, double>>{{2.0, db(0.5)}, {0.0, db(0.25)}, {1.0, db(0.25)}}) {
    auto values = untilted();
    values[at("lens.view")] = view;
    const auto t = analyse(values, input);
    REQUIRE_LT(std::fabs(level_at(t, lens::telemetry::curve_a, hz) - expected), 0.1,
               "view " + std::to_string(view) + " reads its signal: " +
                   std::to_string(level_at(t, lens::telemetry::curve_a, hz)) + " dB, expected " +
                   std::to_string(expected));
    REQUIRE_LE(t[lens::telemetry::curve_b], -199.0, "a single view leaves the second curve silent");
  }
  auto pair = untilted();
  pair[at("lens.view")] = 5.0; // Left / Right
  const auto t = analyse(pair, input);
  REQUIRE_LT(std::fabs(level_at(t, lens::telemetry::curve_a, hz) - db(0.5)), 0.1,
             "Left / Right puts the right channel second");
  REQUIRE_LT(level_at(t, lens::telemetry::curve_b, hz), -100.0,
             "Left / Right puts the right channel second");
  const double side_hz = on_point(80.0);
  const auto side = sine(side_hz, 0.2, std::size_t{2} * 48'000);
  auto overlay = untilted();
  overlay[at("lens.overlay")] = 1.0;
  const auto o = analyse(overlay, input, 48'000.0, &side);
  REQUIRE_LT(std::fabs(level_at(o, lens::telemetry::overlay, side_hz) - db(0.2)), 0.1,
             "the overlay reads the sidechain");
  const auto off = analyse(untilted(), input, 48'000.0, &side);
  REQUIRE_LE(off[lens::telemetry::overlay], -199.0, "and is silent when off");
}

void resonances_are_found() {
  const auto input =
      add(add(sine(80.0, 0.4, std::size_t{2} * 48'000), sine(440.0, 0.2, std::size_t{2} * 48'000)),
          sine(3'000.0, 0.1, std::size_t{2} * 48'000));
  auto values = untilted();
  values[at("lens.smoothing")] = 0.0;
  const auto t = analyse(values, input);
  const std::array<std::pair<double, double>, 3> expected{
      {{80.0, 0.4}, {440.0, 0.2}, {3'000.0, 0.1}}};
  for (std::size_t k = 0; k < 3; ++k) {
    const double hz = t[lens::telemetry::peaks + 2 * k];
    const double level = t[lens::telemetry::peaks + 2 * k + 1];
    REQUIRE_LT(std::fabs(std::log2(hz / expected[k].first)), 1.0 / 48.0,
               "resonance " + std::to_string(k) + " at " + std::to_string(hz) + " Hz, expected " +
                   std::to_string(expected[k].first));
    REQUIRE_LT(std::fabs(level - db(expected[k].second)), 0.3,
               "resonance " + std::to_string(k) + " reads " + std::to_string(level) + " dB");
  }
}

void infinite_average_and_freeze() {
  auto values = untilted();
  values[at("lens.average")] = 3.0;
  values[at("lens.smoothing")] = 3.0;
  // Infinite: two seconds of noise then two of silence average to half
  // the power of the noise alone.
  auto input = pink(std::size_t{4} * 48'000, 3);
  std::fill(input.left.begin() + std::ptrdiff_t{2} * 48'000, input.left.end(), 0.0);
  std::fill(input.right.begin() + std::ptrdiff_t{2} * 48'000, input.right.end(), 0.0);
  const auto burst = slice(input, 0, std::size_t{2} * 48'000);
  const auto half = analyse(values, input);
  const auto full = analyse(values, burst);
  const auto j = point_near(1'000.0);
  const double change = half[lens::telemetry::curve_a + j] - full[lens::telemetry::curve_a + j];
  REQUIRE_LT(std::fabs(change + 3.01), 0.5,
             "infinite averaging halves the power over twice the time: " + std::to_string(change));
  auto runtime = make_effect(lens::engine(), values);
  (void)process(*runtime, burst);
  std::vector<float> before;
  (void)runtime->read_telemetry(before);
  auto frozen = values;
  frozen[at("lens.freeze")] = 1.0;
  require(runtime->offer_values(frozen, 1), "freeze offered");
  (void)process(*runtime, sine(5'000.0, 0.9, 48'000), {}, std::uint64_t{2} * 48'000);
  std::vector<float> after;
  (void)runtime->read_telemetry(after);
  require(std::equal(before.begin(),
                     before.begin() + static_cast<std::ptrdiff_t>(lens::telemetry::peaks),
                     after.begin()),
          "a frozen picture does not move");
}

void telemetry_ignores_block_size() {
  const auto input = pink(48'000, 9);
  const auto values = preset(table(), "Mix");
  const auto a = analyse(values, input, 48'000.0, nullptr, 128);
  const auto b = analyse(values, input, 48'000.0, nullptr, 0);
  REQUIRE_EQ(a, b, "the analysis is identical for any block split");
}

// ADR-0120: the tonal meter is the band meter over the input's mid and the
// sidechain's, whatever the view, and the match settings read the table.
void tonal_meter_reads_input_and_sidechain() {
  namespace tonal = oscillate::native_dsp::tonal;
  constexpr std::size_t seconds = 2;
  constexpr std::uint32_t input_seed = 4;
  constexpr double side_hz = 700.0;
  constexpr double side_level = 0.3;
  const auto frames = seconds * oscillate::test::sample_rate_48k;
  const auto input = pink(frames, input_seed);
  const auto side = sine(side_hz, side_level, frames);
  auto values = untilted();
  values[at("lens.view")] = 2.0; // Left: the meter still reads the mid.
  const auto t = analyse(values, input, oscillate::test::sample_rate_48k_hz, &side);
  tonal::BandMeter meter(oscillate::test::sample_rate_48k_hz);
  for (std::size_t i = 0; i < frames; ++i) {
    const auto left = static_cast<double>(static_cast<float>(input.left[i]));
    const auto right = static_cast<double>(static_cast<float>(input.right[i]));
    const auto side_mid = 0.5 * (static_cast<double>(static_cast<float>(side.left[i])) +
                                 static_cast<double>(static_cast<float>(side.right[i])));
    meter.push(0.5 * (left + right), side_mid);
  }
  REQUIRE_EQ(static_cast<std::uint64_t>(t[lens::telemetry::tonal_windows]), meter.windows(),
             "the window count");
  for (std::size_t i = 0; i < tonal::fine_bands; ++i) {
    REQUIRE_EQ(t[lens::telemetry::tonal_input + i], static_cast<float>(meter.mean_a()[i]),
               "the input's mid in fine band " + std::to_string(i));
    REQUIRE_EQ(t[lens::telemetry::tonal_sidechain + i], static_cast<float>(meter.mean_b()[i]),
               "the sidechain's in fine band " + std::to_string(i));
  }
  values[at("lens.match_amount")] = 0.5;
  values[at("lens.match_smoothing")] = 3.0;
  values[at("lens.match_low")] = 40.0;
  values[at("lens.match_high")] = 12'000.0;
  values[at("lens.match_boost")] = 3.0;
  values[at("lens.match_cut")] = 9.0;
  const auto settings = lens::match_settings(values);
  require(settings.amount == 0.5 && settings.smoothing_octaves == 1.0 && settings.low_hz == 40.0 &&
              settings.high_hz == 12'000.0 && settings.max_boost_db == 3.0 &&
              settings.max_cut_db == 9.0,
          "the match settings are the table's");
  REQUIRE_FALSE(lens::matches_target(values), "matching to the sidechain by default");
  values[at("lens.match_source")] = 1.0;
  REQUIRE_TRUE(lens::matches_target(values), "or to a target");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "native lens contract passed",
      {
          {"audio_passes_untouched", [] { audio_passes_untouched(); }},
          {"sines_read_their_level", [] { sines_read_their_level(); }},
          {"pink_noise_is_flat_and_tilt_is_exact", [] { pink_noise_is_flat_and_tilt_is_exact(); }},
          {"close_low_tones_resolve", [] { close_low_tones_resolve(); }},
          {"highs_follow_faster_than_lows", [] { highs_follow_faster_than_lows(); }},
          {"peak_hold_holds_and_decays", [] { peak_hold_holds_and_decays(); }},
          {"views_and_overlay_read_true", [] { views_and_overlay_read_true(); }},
          {"resonances_are_found", [] { resonances_are_found(); }},
          {"infinite_average_and_freeze", [] { infinite_average_and_freeze(); }},
          {"telemetry_ignores_block_size", [] { telemetry_ignores_block_size(); }},
          {"tonal_meter_reads_input_and_sidechain",
           [] { tonal_meter_reads_input_and_sidechain(); }},
      });
}

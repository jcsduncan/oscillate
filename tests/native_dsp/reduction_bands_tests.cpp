#include "effect_test_support.hpp"

#include "oscillate/native_dsp/effects/ceiling.hpp"
#include "oscillate/native_dsp/effects/press.hpp"
#include "oscillate/native_dsp/effects/reduction_bands.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// Where gain reduction comes from (reduction_bands.hpp): Ceiling and Press
// put a low sine's reduction in its low band and a high sine's in the top
// band, follow Press's key and its detector high-pass, show nothing while
// nothing is reduced, and never allocate on the callback.
namespace {
using namespace effect_test;
namespace bands = oscillate::native_dsp::effects::reduction_bands;
namespace ceiling = oscillate::native_dsp::effects::ceiling;
namespace press = oscillate::native_dsp::effects::press;
using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr std::size_t block = oscillate::test::block_128;
// Long enough for the shares to settle: over three smoothing times.
constexpr std::size_t settle_frames = oscillate::test::sample_rate_48k;
// A kick's fundamental and a cymbal's sizzle: the centres of bands 1 and 7.
constexpr std::size_t low_band = 1;
constexpr std::size_t high_band = bands::count - 1;
// A loud sine (-6 dBFS) and one that stays clear of any threshold here.
constexpr double loud = 0.5;
constexpr double quiet = 0.01;
// A sine at a band's centre sits about 1.2 octaves from each edge, where the
// LR4 neighbours pass under a tenth of its power; after three smoothing
// times its share is within 5% of its settled value.
constexpr double least_dominant_share = 0.75;
// With nothing reduced the shares never leave zero.
constexpr double no_share = 0.0;
// Ceiling: driven 12 dB into a -6 dB ceiling.
constexpr double ceiling_drive_db = 12.0;
constexpr double ceiling_level_db = -6.0;
// Press: a -30 dB threshold at 4:1, and a detector high-pass at 300 Hz.
constexpr double press_threshold_db = -30.0;
constexpr double press_ratio = 4.0;
constexpr double press_highpass_hz = 300.0;
constexpr double highpass_on = 1.0;
// Two sines sharing the loud level.
constexpr double each_of_two = loud / 2.0;
// The transport the callback test plays: 120 BPM in 4/4.
constexpr double beats_per_second = 2.0;
constexpr double beats_per_bar = 4.0;
constexpr std::size_t allocation_blocks = 40;
constexpr std::uint64_t generation = 1;

std::vector<double> ceiling_values() {
  const auto& table = *ceiling::engine().descriptor;
  auto values = default_values(table);
  values[index(table, "ceiling.gain")] = ceiling_drive_db;
  values[index(table, "ceiling.ceiling")] = ceiling_level_db;
  return values;
}

std::vector<double> press_values() {
  const auto& table = *press::engine().descriptor;
  auto values = default_values(table);
  values[index(table, "press.threshold")] = press_threshold_db;
  values[index(table, "press.ratio")] = press_ratio;
  return values;
}

// Commutative, so a swapped call is harmless.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Stereo sum(const Stereo& a, const Stereo& b) {
  Stereo out = a;
  for (std::size_t i = 0; i < out.size(); ++i) {
    out.left[i] += b.left[i];
    out.right[i] += b.right[i];
  }
  return out;
}

// The shares the engine publishes after rendering `input` (keyed by
// `sidechain` when given).
std::array<float, bands::count> shares(const effects::EffectEngine& engine,
                                       const std::vector<double>& values, const Stereo& input,
                                       std::size_t first, const Stereo* sidechain = nullptr) {
  auto runtime = make_effect(engine, values, rate);
  RenderOptions options;
  options.block = block;
  options.sidechain = sidechain;
  (void)process(*runtime, input, options);
  std::vector<float> frame;
  REQUIRE_TRUE(runtime->read_telemetry(frame), "telemetry is published");
  REQUIRE_GE(frame.size(), first + bands::count, "the frame carries the reduction bands");
  std::array<float, bands::count> out{};
  std::copy_n(frame.begin() + static_cast<std::ptrdiff_t>(first), bands::count, out.begin());
  return out;
}

void require_dominant(const std::array<float, bands::count>& shares, std::size_t expected,
                      const std::string& what) {
  const auto strongest =
      static_cast<std::size_t>(std::distance(shares.begin(), std::ranges::max_element(shares)));
  REQUIRE_EQ(strongest, expected, what + ": the reduction is attributed to its band");
  REQUIRE_GT(static_cast<double>(shares[expected]), least_dominant_share,
             what + ": its band holds most of the detector");
}

void ceiling_puts_a_low_sine_in_the_low_band() {
  const auto input = sine(bands::centre_hz(low_band), loud, settle_frames, rate);
  require_dominant(
      shares(ceiling::engine(), ceiling_values(), input, ceiling::telemetry::reduction_bands),
      low_band, "Ceiling, low sine");
}

void ceiling_puts_a_high_sine_in_the_high_band() {
  const auto input = sine(bands::centre_hz(high_band), loud, settle_frames, rate);
  require_dominant(
      shares(ceiling::engine(), ceiling_values(), input, ceiling::telemetry::reduction_bands),
      high_band, "Ceiling, high sine");
}

void press_puts_a_low_sine_in_the_low_band() {
  const auto input = sine(bands::centre_hz(low_band), loud, settle_frames, rate);
  require_dominant(
      shares(press::engine(), press_values(), input, press::telemetry::reduction_bands), low_band,
      "Press, low sine");
}

void press_puts_a_high_sine_in_the_high_band() {
  const auto input = sine(bands::centre_hz(high_band), loud, settle_frames, rate);
  require_dominant(
      shares(press::engine(), press_values(), input, press::telemetry::reduction_bands), high_band,
      "Press, high sine");
}

// Keyed, the reduction comes from the key: a quiet high input ducked by a
// loud low key reads low.
void press_follows_its_key() {
  const auto input = sine(bands::centre_hz(high_band), quiet, settle_frames, rate);
  const auto key = sine(bands::centre_hz(low_band), loud, settle_frames, rate);
  require_dominant(
      shares(press::engine(), press_values(), input, press::telemetry::reduction_bands, &key),
      low_band, "Press, keyed by a low sine");
}

// The detector high-pass hides the lows from the detector: equal low and
// high sines read high.
void press_weighs_its_detector_high_pass() {
  const auto& table = *press::engine().descriptor;
  auto values = press_values();
  values[index(table, "press.detector_hp")] = highpass_on;
  values[index(table, "press.detector_freq")] = press_highpass_hz;
  const auto input = sum(sine(bands::centre_hz(low_band), each_of_two, settle_frames, rate),
                         sine(bands::centre_hz(high_band), each_of_two, settle_frames, rate));
  require_dominant(shares(press::engine(), values, input, press::telemetry::reduction_bands),
                   high_band, "Press, detector high-pass");
}

void nothing_reduced_shows_nothing() {
  const auto input = sine(bands::centre_hz(low_band), quiet, settle_frames, rate);
  for (const auto& [effect, values, first] :
       {std::tuple{&ceiling::engine(), ceiling_values(), ceiling::telemetry::reduction_bands},
        std::tuple{&press::engine(), press_values(), press::telemetry::reduction_bands}}) {
    for (const float share : shares(*effect, values, input, first)) {
      REQUIRE_EQ(static_cast<double>(share), no_share,
                 std::string(effect->descriptor->id) + ": no reduction, no attribution");
    }
  }
}

void the_callback_never_allocates() {
  const auto input = sine(bands::centre_hz(low_band), loud, block, rate);
  for (const auto& [effect, values] : {std::pair{&ceiling::engine(), ceiling_values()},
                                       std::pair{&press::engine(), press_values()}}) {
    auto runtime = make_effect(*effect, values, rate);
    std::vector<engine::StereoFrame> buffer(block);
    std::vector<engine::StereoFrame> side(block);
    for (std::size_t i = 0; i < block; ++i) {
      buffer[i] = {static_cast<float>(input.left[i]), static_cast<float>(input.right[i])};
      side[i] = buffer[i];
    }
    const SynthTransport transport{0.0, beats_per_second / rate, 0.0, beats_per_bar};
    counted_allocations = 0;
    counting_allocations = true;
    for (std::size_t n = 0; n < allocation_blocks; ++n) {
      (void)runtime->process(buffer, effect->sidechain ? std::span(side) : std::span(side).first(0),
                             n * block, generation, transport, {});
    }
    counting_allocations = false;
    REQUIRE_EQ(counted_allocations, 0,
               std::string(effect->descriptor->id) + ": process never allocates");
  }
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "native reduction bands contract passed",
      {
          {"ceiling_puts_a_low_sine_in_the_low_band",
           [] { ceiling_puts_a_low_sine_in_the_low_band(); }},
          {"ceiling_puts_a_high_sine_in_the_high_band",
           [] { ceiling_puts_a_high_sine_in_the_high_band(); }},
          {"press_puts_a_low_sine_in_the_low_band",
           [] { press_puts_a_low_sine_in_the_low_band(); }},
          {"press_puts_a_high_sine_in_the_high_band",
           [] { press_puts_a_high_sine_in_the_high_band(); }},
          {"press_follows_its_key", [] { press_follows_its_key(); }},
          {"press_weighs_its_detector_high_pass", [] { press_weighs_its_detector_high_pass(); }},
          {"nothing_reduced_shows_nothing", [] { nothing_reduced_shows_nothing(); }},
          {"the_callback_never_allocates", [] { the_callback_never_allocates(); }},
      });
}

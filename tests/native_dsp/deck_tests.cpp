#include "effect_test_support.hpp"

#include "oscillate/native_dsp/effects/deck.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Deck (ADR-0055): flat at every band's 0 dB, since the Linkwitz-Riley bands
// sum to an allpass; a killed band (or one at its bottom stop) gone to the
// depth fourth-order splits give; each crossover 6 dB down on its corner;
// boosts exact inside their band; kills that glide in without a click; and
// crossovers that stay finite while automated.
namespace {
using namespace effect_test;
namespace deck = oscillate::native_dsp::effects::deck;

const EngineDescriptor& table() { return *deck::engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }
std::vector<double> flat() { return preset(table(), "Flat"); }

constexpr std::size_t frames = 48'000;

// A tone's gain through Deck, measured once its filters have settled.
double tone_db(const std::vector<double>& values, double hz) {
  const auto input = sine(hz, 0.25, frames);
  auto runtime = make_effect(deck::engine(), values);
  const auto out = process(*runtime, input);
  return db(rms(out.left, frames / 2, frames) / rms(input.left, frames / 2, frames));
}

std::vector<double> with(const std::vector<std::pair<std::string_view, double>>& changes) {
  auto values = flat();
  for (const auto& [id, value] : changes)
    values[at(id)] = value;
  return values;
}

void flat_at_zero_db() {
  for (const double hz : {40.0, 100.0, 250.0, 600.0, 1'000.0, 2'500.0, 5'000.0, 12'000.0})
    REQUIRE_LT(std::fabs(tone_db(flat(), hz)), 0.05,
               "every band at 0 dB is flat at " + std::to_string(hz) + " Hz");
  // Noise keeps its energy: the sum is an allpass.
  const auto input = noise(0.3, frames);
  auto runtime = make_effect(deck::engine(), flat());
  const auto out = process(*runtime, input);
  REQUIRE_LT(std::fabs(db(rms(out.left, 4'800, frames) / rms(input.left, 4'800, frames))), 0.05,
             "noise keeps its level through a flat Deck");
}

void kills_isolate_their_band() {
  REQUIRE_LT(tone_db(with({{"deck.low_kill", 1}}), 40.0), -50.0, "a low kill removes the bass");
  REQUIRE_LT(tone_db(with({{"deck.high_kill", 1}}), 12'000.0), -45.0,
             "a high kill removes the top");
  REQUIRE_LT(tone_db(with({{"deck.mid_kill", 1}}), 790.0), -30.0, "a mid kill removes the mids");
  // The other bands are untouched by a kill.
  require(std::fabs(tone_db(with({{"deck.low_kill", 1}}), 8'000.0)) < 0.1 &&
              std::fabs(tone_db(with({{"deck.high_kill", 1}}), 60.0)) < 0.1,
          "a kill leaves the other bands alone");
  // The bottom stop is the same full kill.
  const auto input = noise(0.3, 12'000);
  auto killed = make_effect(deck::engine(), with({{"deck.low_kill", 1}}));
  auto bottom = make_effect(deck::engine(), with({{"deck.low", -40}}));
  const auto a = process(*killed, input);
  const auto b = process(*bottom, input);
  REQUIRE_EQ(a.left, b.left, "the bottom stop is a full kill");
  REQUIRE_EQ(a.right, b.right, "the bottom stop is a full kill");
}

void crossovers_are_six_db_down_on_their_corner() {
  REQUIRE_LT(std::fabs(tone_db(with({{"deck.low_kill", 1}}), 250.0) + 6.02), 0.3,
             "the low crossover is -6 dB at its corner");
  REQUIRE_LT(std::fabs(tone_db(with({{"deck.high_kill", 1}}), 2'500.0) + 6.02), 0.3,
             "the high crossover is -6 dB at its corner");
  // Moving a crossover moves its corner.
  REQUIRE_LT(std::fabs(tone_db(with({{"deck.low_kill", 1}, {"deck.low_x", 120}}), 120.0) + 6.02),
             0.3, "the low crossover follows its knob");
}

void boosts_are_exact_in_band() {
  REQUIRE_LT(std::fabs(tone_db(with({{"deck.low", 6}}), 40.0) - 6.0), 0.2, "low boost is +6 dB");
  REQUIRE_LT(std::fabs(tone_db(with({{"deck.mid", -12}}), 790.0) + 12.0), 1.0, "mid cut is -12 dB");
  REQUIRE_LT(std::fabs(tone_db(with({{"deck.high", 6}}), 12'000.0) - 6.0), 0.2,
             "high boost is +6 dB");
  REQUIRE_LT(std::fabs(tone_db(with({{"deck.gain", -6}}), 1'000.0) + 6.0), 0.05,
             "output gain is exact");
}

void kills_glide_in() {
  const auto input = sine(100.0, 0.4, frames);
  auto runtime = make_effect(deck::engine(), flat());
  const auto next = with({{"deck.low_kill", 1}});
  const auto out = process(*runtime, input, {.at = {24'000}, .before = [&](std::uint64_t) {
                                               require(runtime->offer_values(next, 1), "offered");
                                             }});
  double steady = 0.0;
  for (std::size_t i = 12'001; i < 24'000; ++i)
    steady = std::max(steady, std::fabs(out.left[i] - out.left[i - 1]));
  double across = 0.0;
  for (std::size_t i = 24'000; i < 25'200; ++i)
    across = std::max(across, std::fabs(out.left[i] - out.left[i - 1]));
  REQUIRE_LT(across, steady * 1.3 + 1e-3, "a kill glides in without a click");
  REQUIRE_LT(rms(out.left, 36'000, frames), 0.01, "the killed bass is gone once it has glided");
}

void automated_crossovers_stay_finite() {
  const auto input = noise(0.5, frames);
  auto runtime = make_effect(deck::engine(), flat());
  std::vector<std::uint64_t> moments;
  for (std::uint64_t frame = 960; frame < frames; frame += 960)
    moments.push_back(frame);
  std::uint64_t revision = 0;
  const auto out =
      process(*runtime, input, {.at = moments, .before = [&](std::uint64_t frame) {
                                  auto values = flat();
                                  const double phase =
                                      static_cast<double>(frame) / static_cast<double>(frames);
                                  values[at("deck.low_x")] = 60.0 + 540.0 * phase;
                                  values[at("deck.high_x")] = 8'000.0 - 7'000.0 * phase;
                                  require(runtime->offer_values(values, ++revision), "offered");
                                }});
  for (std::size_t i = 0; i < out.size(); ++i)
    require(std::isfinite(out.left[i]) && std::fabs(out.left[i]) < 2.0,
            "a swept crossover stays finite and bounded");
}

} // namespace

int main() {
  try {
    flat_at_zero_db();
    kills_isolate_their_band();
    crossovers_are_six_db_down_on_their_corner();
    boosts_are_exact_in_band();
    kills_glide_in();
    automated_crossovers_stay_finite();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "native deck contract passed\n";
  return 0;
}

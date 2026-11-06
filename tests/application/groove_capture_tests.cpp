// Groove from a record (ADR-0079): synthetic records whose feel is known
// (kicks, hats, swing, pushes, accents) give that feel back within a
// tolerance, measured against the kick; one band's push stays out of
// another; hats laid back behind the kick read late; random hits are not
// trusted; two-bar cycles keep their second bar; the A/B audition is whole
// bars and deterministic.

#include "oscillate/application/groove_capture.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace application = oscillate::application;
namespace groove = oscillate::application::groove_capture;
using oscillate::timeline::ticks_per_quarter;

constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
constexpr double bpm = 125.0;
constexpr double quarter = 60.0 / bpm * rate;
constexpr double sixteenth = quarter / 4.0;
// The record starts after a little silence, on a downbeat.
constexpr double lead = 0.25 * rate;
constexpr std::size_t record_bars = 17;
// How close a measured offset must be (a share of a step).
constexpr double timing_tolerance = 0.03;

using oscillate::test::require;

class Record {
public:
  Record()
      : samples_(
            static_cast<std::size_t>(lead + quarter * 4.0 * static_cast<double>(record_bars + 1)),
            0.0F) {}

  // A kick: a sine falling from 130 Hz to 50 Hz.
  void kick(double at, double gain) {
    double phase = 0.0;
    const auto length = static_cast<std::size_t>(0.3 * rate);
    for (std::size_t i = 0; i < length; ++i) {
      const double t = static_cast<double>(i) / rate;
      phase += 2.0 * std::numbers::pi * (50.0 + 80.0 * std::exp(-t / 0.02)) / rate;
      add(at, i, gain * std::sin(phase) * std::exp(-t / 0.12));
    }
  }
  // A hat: bright noise (differenced twice), a 12 ms decay.
  void hat(double at, double gain) {
    const auto length = static_cast<std::size_t>(0.05 * rate);
    double previous = 0.0;
    double earlier = 0.0;
    for (std::size_t i = 0; i < length; ++i) {
      const double x = noise();
      const double y = (x - 2.0 * previous + earlier) * 0.25;
      earlier = previous;
      previous = x;
      add(at, i, gain * y * std::exp(-static_cast<double>(i) / rate / 0.012));
    }
  }
  double noise() {
    state_ ^= state_ << 13U;
    state_ ^= state_ >> 7U;
    state_ ^= state_ << 17U;
    return static_cast<double>(state_ >> 11U) / static_cast<double>(1ULL << 53U) * 2.0 - 1.0;
  }
  [[nodiscard]] std::span<const float> samples() const { return samples_; }

private:
  void add(double at, std::size_t i, double value) {
    const auto n = static_cast<std::size_t>(std::llround(at)) + i;
    if (n < samples_.size())
      samples_[n] += static_cast<float>(value);
  }
  std::vector<float> samples_;
  std::uint64_t state_ = 0x2545f4914f6cdd1dULL;
};

// Kicks on every beat, the bar's first louder (it marks the one).
void four_to_the_floor(Record& record) {
  for (std::size_t beat = 0; beat < record_bars * 4; ++beat)
    record.kick(lead + quarter * static_cast<double>(beat), beat % 4 == 0 ? 0.9 : 0.7);
}

groove::Request request(groove::Band band, std::size_t steps = 16) {
  groove::Request r;
  r.name = "Test";
  r.band = band;
  r.bars = 8;
  r.cycle_steps = steps;
  return r;
}

bool near(double value, double expected, double tolerance) {
  return std::abs(value - expected) <= tolerance;
}

void swing_and_accents_are_taken_from_the_hats() {
  Record record;
  four_to_the_floor(record);
  // Swung 16ths: every off-16th 0.3 of a step late; the offbeat 8ths loud.
  for (std::size_t step = 0; step < record_bars * 16; ++step) {
    const double late = step % 2 == 1 ? 0.3 : 0.0;
    record.hat(lead + sixteenth * (static_cast<double>(step) + late), step % 4 == 2 ? 1.0 : 0.45);
  }
  auto captured = groove::capture_audio(record.samples(), rate, request(groove::Band::hats));
  require(static_cast<bool>(captured), "a groove is taken");
  const auto& c = captured.value();
  require(near(c.bpm, bpm, 0.05), "the tempo is found from the kick: " + std::to_string(c.bpm));
  const auto& m = c.measurement;
  for (std::size_t step = 0; step < 16; ++step) {
    const double expected = step % 2 == 1 ? 0.3 : 0.0;
    require(near(m.steps[step].timing, expected, timing_tolerance),
            "step " + std::to_string(step) + " timing " + std::to_string(m.steps[step].timing));
    REQUIRE_EQ(m.groove.steps[step].timing, m.steps[step].timing, "the template is the feel");
    REQUIRE_EQ(m.steps[step].hits, 8, "a hit a bar on every step");
  }
  require(near(m.swing_percent, 65.0, 1.5), "65 % swing: " + std::to_string(m.swing_percent));
  require(near(m.steps[1].timing_ms, 0.3 * 120.0, 4.0), "0.3 of a 120 ms step is 36 ms");
  REQUIRE_LT(std::abs(m.offbeat_ms), 2.0,
             "the offbeat 8ths are on the grid: " + std::to_string(m.offbeat_ms) +
                 " ms; steps 2, 6: " + std::to_string(m.steps[2].timing_ms) + ", " +
                 std::to_string(m.steps[6].timing_ms) +
                 " ms; step 0: " + std::to_string(m.steps[0].timing_ms) + " ms");
  REQUIRE_GT(m.steps[2].velocity, 1.15, "the offbeats are accented");
  REQUIRE_LT(m.steps[1].velocity, 1.0, "the offbeats are accented");
  REQUIRE_EQ(m.accents, 4, "four accents a bar: " + std::to_string(m.accents));
  REQUIRE_GT(m.confidence, 0.7, "a tight record is trusted: " + std::to_string(m.confidence));
  REQUIRE_EQ(c.bar_peaks.size(), groove::bar_peak_points,
             "the fingerprint has a bar and its 16 hits");
  REQUIRE_EQ(c.bar_onsets.size(), 16, "the fingerprint has a bar and its 16 hits");
  const auto words = groove::describe(m);
  require(words.find("Swung 16ths") == 0 && words.find("4 accents") != std::string::npos,
          "plain words: " + words);
}

void a_kick_push_stays_out_of_the_hats() {
  Record record;
  // Beats 2 and 4 pushed 0.15 of a 16th early; straight 8th hats.
  for (std::size_t beat = 0; beat < record_bars * 4; ++beat) {
    const double push = beat % 2 == 1 ? -0.15 : 0.0;
    record.kick(lead + quarter * static_cast<double>(beat) + sixteenth * push,
                beat % 4 == 0 ? 0.9 : 0.7);
  }
  for (std::size_t eighth = 0; eighth < record_bars * 8; ++eighth)
    record.hat(lead + sixteenth * 2.0 * static_cast<double>(eighth), 0.5);
  auto kick_request = request(groove::Band::kick);
  kick_request.bpm = bpm;
  auto kicks = groove::capture_audio(record.samples(), rate, kick_request);
  require(static_cast<bool>(kicks), "the kick's groove");
  const auto& k = kicks.value().measurement;
  require(near(k.steps[0].timing, 0.0, timing_tolerance) &&
              near(k.steps[8].timing, 0.0, timing_tolerance),
          "the one and three on the grid: " + std::to_string(k.steps[8].timing));
  require(near(k.steps[4].timing, -0.15, timing_tolerance) &&
              near(k.steps[12].timing, -0.15, timing_tolerance),
          "two and four pushed: " + std::to_string(k.steps[4].timing));
  auto hat_request = request(groove::Band::hats);
  hat_request.bpm = bpm;
  auto hats = groove::capture_audio(record.samples(), rate, hat_request);
  require(static_cast<bool>(hats), "the hats' groove");
  const auto& h = hats.value().measurement;
  for (std::size_t step = 0; step < 16; step += 2)
    require(near(h.steps[step].timing, 0.0, timing_tolerance),
            "the hats stay straight: step " + std::to_string(step) + " " +
                std::to_string(h.steps[step].timing));
  REQUIRE_EQ(h.steps[1].hits, 0, "no hats between the 8ths");
}

void hats_keep_their_lean_against_the_kick() {
  Record record;
  four_to_the_floor(record);
  // Every 8th hat 10 ms behind the kick: the feel is the lean, not the grid.
  constexpr double lean_ms = 10.0;
  for (std::size_t eighth = 0; eighth < record_bars * 8; ++eighth)
    record.hat(lead + sixteenth * 2.0 * static_cast<double>(eighth) + lean_ms / 1000.0 * rate, 0.5);
  auto wanted = request(groove::Band::hats);
  wanted.bpm = bpm;
  auto captured = groove::capture_audio(record.samples(), rate, wanted);
  require(static_cast<bool>(captured), "the hats' groove");
  const auto& m = captured.value().measurement;
  for (std::size_t step = 0; step < 16; step += 2)
    require(near(m.steps[step].timing_ms, lean_ms, 1.5),
            "laid back 10 ms on step " + std::to_string(step) + ": " +
                std::to_string(m.steps[step].timing_ms));
  require(near(m.offbeat_ms, lean_ms, 1.5), "the offbeats too");
}

void random_hits_are_not_trusted() {
  Record record;
  four_to_the_floor(record);
  const double span = quarter * 4.0 * static_cast<double>(record_bars);
  for (std::size_t i = 0; i < record_bars * 16; ++i) {
    const double at = lead + (record.noise() * 0.5 + 0.5) * span;
    record.hat(at, 0.3 + 0.3 * (record.noise() * 0.5 + 0.5));
  }
  auto captured = groove::capture_audio(record.samples(), rate, request(groove::Band::hats));
  require(static_cast<bool>(captured), "random hits still measure");
  const auto& m = captured.value().measurement;
  REQUIRE_LT(m.confidence, 0.35, "but are not trusted: " + std::to_string(m.confidence));
  REQUIRE_EQ(groove::describe(m).find("Loose"), 0, "and say so");
}

void a_two_bar_cycle_keeps_its_second_bar() {
  Record record;
  four_to_the_floor(record);
  // 8th hats; the second bar of each pair drags its 4th 8th (step 6).
  for (std::size_t bar = 0; bar < record_bars; ++bar)
    for (std::size_t eighth = 0; eighth < 8; ++eighth) {
      const double late = bar % 2 == 1 && eighth == 3 ? 0.25 : 0.0;
      record.hat(lead + quarter * 4.0 * static_cast<double>(bar) +
                     sixteenth * (2.0 * static_cast<double>(eighth) + late),
                 0.5);
    }
  auto wanted = request(groove::Band::hats, 32);
  wanted.bpm = bpm;
  auto captured = groove::capture_audio(record.samples(), rate, wanted);
  require(static_cast<bool>(captured), "a two-bar groove");
  const auto& m = captured.value().measurement;
  REQUIRE_EQ(m.groove.steps.size(), 32, "32 16ths");
  REQUIRE_EQ(m.groove.steps_per_quarter, 4, "32 16ths");
  require(near(m.steps[6].timing, 0.0, timing_tolerance), "the first bar straight");
  require(near(m.steps[22].timing, 0.25, timing_tolerance),
          "the second bar drags: " + std::to_string(m.steps[22].timing));
}

void hits_measure_to_their_feel() {
  // Pure: hits in ticks. A bar of 16ths with every hit 10 % of a step late
  // and the odd ones another 20 %: shifted by the downbeats' offset, the
  // odd ones read 0.2.
  constexpr auto step = ticks_per_quarter / 4;
  std::vector<application::GrooveHit> hits;
  for (std::int64_t bar = 0; bar < 4; ++bar)
    for (std::int64_t i = 0; i < 16; ++i)
      hits.push_back({(bar * 16 + i) * step + step / 10 + (i % 2 == 1 ? step / 5 : 0), 1.0});
  const auto shift = groove::downbeat_shift(hits, 4);
  REQUIRE_TRUE(shift, "the downbeats' offset");
  REQUIRE_EQ(*shift, step / 10, "the downbeats' offset");
  auto measured = groove::measure(hits, {"Pure", 4, 16, 4, 100.0, *shift});
  require(static_cast<bool>(measured), "measured");
  const auto& m = measured.value();
  REQUIRE_TRUE(near(m.steps[0].timing, 0.0, 1e-9), "measured against the downbeats");
  REQUIRE_TRUE(near(m.steps[1].timing, 0.2, 1e-9), "measured against the downbeats");
  require(near(m.steps[1].timing_ms, 20.0, 1e-6), "in milliseconds");
  require(near(m.swing_percent, 60.0, 1e-6), "60 % swing");
  REQUIRE_GT(m.confidence, 0.99, "tight and complete");
  REQUIRE_LT(m.steps[3].spread, 1e-9, "tight and complete");
  auto raw = groove::measure(hits, {"Raw", 4, 16, 4, 100.0, 0});
  REQUIRE_TRUE(raw, "unshifted keeps the drag");
  REQUIRE_TRUE(near(raw.value().steps[0].timing, 0.1, 1e-9), "unshifted keeps the drag");
  require(!groove::measure(std::span(hits).first(3), {"Few", 4, 16, 4, 100.0, 0}),
          "four hits needed");
  // Off the downbeats, the beats are the reference; with neither, none.
  const std::vector<application::GrooveHit> beats{{ticks_per_quarter + step / 4, 1.0},
                                                  {3 * ticks_per_quarter + step / 4, 1.0}};
  REQUIRE_EQ(groove::downbeat_shift(beats, 4), step / 4, "the beats when no downbeat");
  const std::vector<application::GrooveHit> offbeats{{step, 1.0}, {3 * step, 1.0}};
  require(!groove::downbeat_shift(offbeats, 4), "no reference without beats");
}

void the_grid_is_fitted_without_a_tempo() {
  Record record;
  four_to_the_floor(record);
  const auto kicks = groove::detect_onsets(record.samples(), rate, groove::Band::kick);
  REQUIRE_EQ(kicks.size(), record_bars * 4, "every kick found: " + std::to_string(kicks.size()));
  auto grid = groove::fit_grid(kicks, rate, std::nullopt);
  REQUIRE_TRUE(grid, "125 BPM, not 62.5 or 250");
  REQUIRE_TRUE(near(grid.value().bpm, bpm, 0.02), "125 BPM, not 62.5 or 250");
  REQUIRE_GT(grid.value().fit, 0.95, "the kicks sit on the quarters");
  // The one is the louder kick: the first bar's start (within a few ms of
  // detector delay).
  require(near(grid.value().downbeat_frame, lead, 0.005 * rate), "the downbeat");
}

void the_audition_is_whole_bars_and_repeatable() {
  const oscillate::persistence::GrooveTemplate swung{"Swung", 4, {{0.0, 1.0}, {0.3, 0.6}}};
  const auto straight = groove::audition(swung, bpm, rate, 4, false);
  const auto grooved = groove::audition(swung, bpm, rate, 4, true);
  REQUIRE_EQ(straight.size(), static_cast<std::size_t>(std::llround(quarter * 16)) * 2,
             "four bars of stereo");
  REQUIRE_EQ(straight.size(), grooved.size(), "B is not A");
  REQUIRE_NE(straight, grooved, "B is not A");
  REQUIRE_EQ(grooved, groove::audition(swung, bpm, rate, 4, true), "the same every time");
  require(std::ranges::all_of(grooved, [](float v) { return std::abs(v) <= 1.0F; }), "no clipping");
}

void requests_are_checked() {
  Record record;
  four_to_the_floor(record);
  auto few = request(groove::Band::hats);
  few.bars = 2;
  require(!groove::capture_audio(record.samples(), rate, few), "at least four bars");
  auto odd = request(groove::Band::hats, 24);
  require(!groove::capture_audio(record.samples(), rate, odd), "16 or 32 steps");
  require(!groove::capture_audio({}, rate, request(groove::Band::hats)), "an empty record");
}

} // namespace

int main() {
  try {
    hits_measure_to_their_feel();
    the_grid_is_fitted_without_a_tempo();
    swing_and_accents_are_taken_from_the_hats();
    a_kick_push_stays_out_of_the_hats();
    hats_keep_their_lean_against_the_kick();
    random_hits_are_not_trusted();
    a_two_bar_cycle_keeps_its_second_bar();
    the_audition_is_whole_bars_and_repeatable();
    requests_are_checked();
  } catch (const std::exception& error) {
    std::cerr << "groove capture: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

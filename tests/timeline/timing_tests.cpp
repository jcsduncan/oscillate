#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/timeline/timing.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {

using oscillate::foundation::ErrorCode;
using oscillate::foundation::Result;
using namespace oscillate::timeline;

static_assert(!std::is_convertible_v<TickPosition, SamplePosition>);
static_assert(!std::is_convertible_v<SamplePosition, TickPosition>);

using oscillate::test::require;

template <typename T> T take(Result<T> result, std::string_view reason) {
  REQUIRE_OK(result, reason);
  return std::move(result).value();
}

void rates_and_exact_scaling() {
  for (const auto rate : {44'100U, 48'000U, 88'200U, 96'000U}) {
    require(supported_sample_rate(rate), "standard rate rejected");
    auto same = scale_sample_position({1'000'001}, rate, rate);
    REQUIRE_TRUE(same, "same-rate position changed");
    REQUIRE_EQ(same.value().value, 1'000'001, "same-rate position changed");
  }
  REQUIRE_TRUE(!supported_sample_rate(0), "unsupported rate accepted");
  REQUIRE_TRUE(!supported_sample_rate(47'999), "unsupported rate accepted");
  REQUIRE_EQ(take(scale_sample_position({80}, 48'000, 44'100), "first half tie failed").value, 74,
             "half tie should round odd floor upward");
  REQUIRE_EQ(take(scale_sample_position({240}, 48'000, 44'100), "second half tie failed").value,
             220, "half tie should keep even floor");
  REQUIRE_EQ(take(scale_sample_position({44'100}, 44'100, 96'000), "one second failed").value,
             96'000, "one second did not remain one second");
  require(!scale_sample_position({-1}, 48'000, 44'100), "negative elapsed anchor accepted");
  require(!scale_sample_position({0}, 0, 44'100), "zero old rate accepted");
  require(!scale_sample_position({0}, 48'000, 192'000), "unsupported new rate accepted");
  auto overflow = scale_sample_position({std::numeric_limits<std::int64_t>::max()}, 44'100, 96'000);
  REQUIRE_TRUE(!overflow, "sample scaling overflow accepted");
  REQUIRE_EQ(overflow.error().code, ErrorCode::resource_exhausted,
             "sample scaling overflow accepted");
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  const auto near_limit = (maximum / 96'000) * 44'100 + 44'099;
  auto fractional_overflow = scale_sample_position({near_limit}, 44'100, 96'000);
  REQUIRE_TRUE(!fractional_overflow, "fractional sample scaling overflow accepted");
  REQUIRE_EQ(fractional_overflow.error().code, ErrorCode::resource_exhausted,
             "fractional sample scaling overflow accepted");
}

void tempo_validation() {
  require(!TempoMap::create({}, 1), "empty tempo map accepted");
  require(!TempoMap::create({{{1}, 120, TempoShape::step}}, 1), "missing zero event accepted");
  for (const double bad : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
    require(!TempoMap::create({{{0}, bad, TempoShape::step}}, 1), "invalid BPM accepted");
  }
  require(!TempoMap::create({{{0}, 120, TempoShape::step}, {{0}, 140, TempoShape::step}}, 1),
          "duplicate tempo tick accepted");
  require(!TempoMap::create({{{0}, 120, TempoShape::step}, {{-1}, 140, TempoShape::step}}, 1),
          "out-of-order tempo tick accepted");
  require(!TempoMap::create({{{0}, 120, TempoShape::linear_bpm}}, 1),
          "ramp with no destination accepted");
  const auto collapsed = TempoMap::create(
      {{{0}, 1.0e-9, TempoShape::step},
       {{ticks_per_quarter}, std::numeric_limits<double>::max(), TempoShape::step},
       {{ticks_per_quarter + 1}, std::numeric_limits<double>::max(), TempoShape::step}},
      1);
  REQUIRE_TRUE(!collapsed, "tempo segment with collapsed elapsed duration accepted");
  REQUIRE_EQ(collapsed.error().code, ErrorCode::resource_exhausted,
             "tempo segment with collapsed elapsed duration accepted");
}

void steady_tempo_and_rounding() {
  const auto map =
      take(TempoMap::create({{{0}, 120, TempoShape::step}}, 7), "constant map rejected");
  REQUIRE_EQ(map.revision(), 7, "immutable tempo map metadata changed");
  REQUIRE_EQ(map.events().size(), 1, "immutable tempo map metadata changed");
  for (const auto rate : {44'100U, 48'000U, 88'200U, 96'000U}) {
    REQUIRE_EQ(take(map.tick_to_sample({ticks_per_quarter}, rate), "beat failed").value,
               static_cast<std::int64_t>(rate / 2), "120 BPM beat position wrong");
    REQUIRE_EQ(
        take(map.sample_to_tick({static_cast<std::int64_t>(rate / 2)}, rate), "inverse beat failed")
            .value,
        ticks_per_quarter, "inverse 120 BPM beat wrong");
  }
  REQUIRE_EQ(take(map.tick_to_sample({20'000}, 48'000), "half sample failed").value, 0,
             "half sample at zero did not tie to even");
  REQUIRE_EQ(take(map.tick_to_sample({60'000}, 48'000), "one and a half failed").value, 2,
             "one and a half samples did not tie to even");
  constexpr std::int64_t day_ticks = 86'400LL * 2 * ticks_per_quarter;
  REQUIRE_EQ(take(map.tick_to_sample({day_ticks}, 96'000), "day-length forward failed").value,
             8'294'400'000, "day-length sample position wrong");
  REQUIRE_EQ(take(map.sample_to_tick({8'294'400'000}, 96'000), "day-length inverse failed").value,
             day_ticks, "day-length inverse coordinate wrong");
  require(!map.tick_to_sample({-1}, 48'000), "negative tick accepted");
  require(!map.tick_to_sample({0}, 0), "invalid rate accepted by forward conversion");
  require(!map.sample_to_tick({-1}, 48'000), "negative sample accepted");
  require(!map.sample_to_tick({0}, 0), "invalid rate accepted by inverse conversion");
  const auto slow =
      take(TempoMap::create({{{0}, 1.0e-9, TempoShape::step}}, 8), "very slow finite BPM rejected");
  auto overflow = slow.tick_to_sample({std::numeric_limits<std::int64_t>::max()}, 96'000);
  REQUIRE_TRUE(!overflow, "forward conversion overflow accepted");
  REQUIRE_EQ(overflow.error().code, ErrorCode::resource_exhausted,
             "forward conversion overflow accepted");
  auto inverse_overflow = map.sample_to_tick({std::numeric_limits<std::int64_t>::max()}, 48'000);
  REQUIRE_TRUE(!inverse_overflow, "inverse conversion overflow accepted");
  REQUIRE_EQ(inverse_overflow.error().code, ErrorCode::resource_exhausted,
             "inverse conversion overflow accepted");
  TickPosition callback_tick{42};
  REQUIRE_TRUE(!map.sample_to_tick_rt({-1}, 48'000, callback_tick),
               "callback conversion changed output on an invalid sample");
  REQUIRE_EQ(callback_tick.value, 42, "callback conversion changed output on an invalid sample");
  REQUIRE_TRUE(!map.sample_to_tick_rt({0}, 0, callback_tick),
               "callback conversion changed output on an invalid rate");
  REQUIRE_EQ(callback_tick.value, 42, "callback conversion changed output on an invalid rate");
  counting_allocations = true;
  const bool overflow_rt =
      map.sample_to_tick_rt({std::numeric_limits<std::int64_t>::max()}, 48'000, callback_tick);
  counting_allocations = false;
  REQUIRE_TRUE(!overflow_rt, "callback overflow conversion allocated or changed its output");
  REQUIRE_EQ(counted_allocations, 0,
             "callback overflow conversion allocated or changed its output");
  REQUIRE_EQ(callback_tick.value, 42,
             "callback overflow conversion allocated or changed its output");
}

void callback_tempo_conversion_matches_control() {
  constexpr auto four_beats = 4 * ticks_per_quarter;
  const auto map =
      take(TempoMap::create(
               {{{0}, 120, TempoShape::linear_bpm}, {{four_beats}, 240, TempoShape::step}}, 1),
           "callback tempo map rejected");
  for (const auto rate : {44'100U, 48'000U, 88'200U, 96'000U}) {
    for (const auto sample : {0LL, 1LL, 12'000LL, 38'925LL, 66'542LL, 100'000LL}) {
      const auto diagnostic = map.sample_to_tick({sample}, rate);
      TickPosition callback_tick{-1};
      REQUIRE_TRUE(diagnostic, "callback tempo conversion diverged from control conversion");
      REQUIRE_TRUE(map.sample_to_tick_rt({sample}, rate, callback_tick),
                   "callback tempo conversion diverged from control conversion");
      REQUIRE_EQ(callback_tick, diagnostic.value(),
                 "callback tempo conversion diverged from control conversion");
    }
  }
}

void ramps_and_boundaries() {
  constexpr auto four_beats = 4 * ticks_per_quarter;
  const auto linear =
      take(TempoMap::create(
               {{{0}, 120, TempoShape::linear_bpm}, {{four_beats}, 240, TempoShape::step}}, 1),
           "linear ramp rejected");
  // Independent analytic fixtures: 120 -> 240 BPM over four beats takes 2 ln 2 s;
  // at its midpoint it takes 2 ln(3/2) s. Rounded at 48 kHz.
  REQUIRE_EQ(
      take(linear.tick_to_sample({2 * ticks_per_quarter}, 48'000), "ramp midpoint failed").value,
      38'925, "linear BPM ramp midpoint wrong");
  REQUIRE_EQ(take(linear.tick_to_sample({four_beats}, 48'000), "ramp end failed").value, 66'542,
             "linear BPM ramp boundary wrong");
  for (const auto [rate, expected] : std::array<std::pair<std::uint32_t, std::int64_t>, 4>{
           {{44'100, 61'136}, {48'000, 66'542}, {88'200, 122'271}, {96'000, 133'084}}}) {
    REQUIRE_EQ(take(linear.tick_to_sample({four_beats}, rate), "ramp rate fixture failed").value,
               expected, "ramp end differs from independent rate fixture");
  }
  REQUIRE_EQ(
      take(linear.tick_to_sample({four_beats + ticks_per_quarter}, 48'000), "post-ramp beat failed")
          .value,
      78'542, "new segment did not start at the boundary");
  REQUIRE_EQ(take(linear.sample_to_tick({0}, 48'000), "inverse zero failed").value, 0,
             "inverse origin wrong");
  double bpm = -1.0;
  counted_allocations = 0;
  counting_allocations = true;
  const bool at_zero = linear.bpm_at_sample_rt({0}, 48'000, bpm);
  const bool at_mid_time = linear.bpm_at_sample_rt({48'000}, 48'000, bpm);
  const auto mid_time_bpm = bpm;
  const bool at_end = linear.bpm_at_sample_rt({66'542}, 48'000, bpm);
  counting_allocations = false;
  REQUIRE_TRUE(at_zero, "callback BPM query diverged from analytic linear ramp or allocated");
  REQUIRE_TRUE(at_mid_time, "callback BPM query diverged from analytic linear ramp or allocated");
  REQUIRE_TRUE(at_end, "callback BPM query diverged from analytic linear ramp or allocated");
  REQUIRE_EQ(counted_allocations, 0,
             "callback BPM query diverged from analytic linear ramp or allocated");
  REQUIRE_LT(std::fabs(mid_time_bpm - 120.0 * std::exp(0.5)), 0.00001,
             "callback BPM query diverged from analytic linear ramp or allocated");
  REQUIRE_LT(std::fabs(bpm - 240.0), 0.001,
             "callback BPM query diverged from analytic linear ramp or allocated");
  const auto before_invalid = bpm;
  REQUIRE_TRUE(!linear.bpm_at_sample_rt({-1}, 48'000, bpm),
               "invalid callback BPM query changed output");
  REQUIRE_EQ(bpm, before_invalid, "invalid callback BPM query changed output");
  REQUIRE_TRUE(!linear.bpm_at_sample_rt({0}, 0, bpm), "invalid callback BPM query changed output");
  REQUIRE_EQ(bpm, before_invalid, "invalid callback BPM query changed output");

  const struct {
    TempoShape shape;
    std::int64_t knot_tick;
    std::int64_t knot_sample;
  } ease_fixtures[] = {{TempoShape::ease_in, 2 * ticks_per_quarter, 42'844},
                       {TempoShape::ease_out, 2 * ticks_per_quarter, 35'815},
                       {TempoShape::ease_in_out, ticks_per_quarter, 22'614}};
  for (const auto fixture : ease_fixtures) {
    const auto map = take(
        TempoMap::create({{{0}, 120, fixture.shape}, {{four_beats}, 240, TempoShape::step}}, 2),
        "ease ramp rejected");
    REQUIRE_EQ(take(map.tick_to_sample({fixture.knot_tick}, 48'000), "ease knot failed").value,
               fixture.knot_sample, "ease knot time wrong");
    std::int64_t previous = -1;
    for (std::int64_t tick = 0; tick <= four_beats + ticks_per_quarter;
         tick += ticks_per_quarter / 16) {
      const auto sample = take(map.tick_to_sample({tick}, 48'000), "ease forward failed").value;
      REQUIRE_GE(sample, previous, "tempo conversion is not monotone");
      previous = sample;
      const auto inverse = take(map.sample_to_tick({sample}, 48'000), "ease inverse failed");
      REQUIRE_EQ(take(map.tick_to_sample(inverse, 48'000), "ease inverse forward failed").value,
                 sample, "inverse tick does not reproduce sample");
    }
  }
  const auto flat_ramp =
      take(TempoMap::create(
               {{{0}, 120, TempoShape::linear_bpm}, {{four_beats}, 120, TempoShape::step}}, 1),
           "flat ramp rejected");
  REQUIRE_EQ(take(flat_ramp.tick_to_sample({four_beats}, 48'000), "flat ramp failed").value, 96'000,
             "constant-BPM branch of ramp wrong");
  REQUIRE_EQ(take(flat_ramp.sample_to_tick({48'000}, 48'000), "flat inverse failed").value,
             2 * ticks_per_quarter, "constant-BPM inverse branch wrong");
  const auto stepped = take(
      TempoMap::create({{{0}, 120, TempoShape::step}, {{four_beats}, 240, TempoShape::step}}, 3),
      "step map rejected");
  REQUIRE_EQ(take(stepped.tick_to_sample({four_beats}, 48'000), "step boundary failed").value,
             96'000, "step segment should keep original BPM to boundary");
  REQUIRE_EQ(take(stepped.tick_to_sample({four_beats + ticks_per_quarter}, 48'000),
                  "step after boundary failed")
                 .value,
             108'000, "step event did not change BPM at boundary");
}

void meter_validation_and_location() {
  require(!MeterMap::create({}, 1), "empty meter map accepted");
  require(!MeterMap::create({{{1}, 4, 4}}, 1), "missing meter origin accepted");
  for (const MeterEvent bad : std::array<MeterEvent, 5>{
           {{{0}, 0, 4}, {{0}, 33, 4}, {{0}, 4, 0}, {{0}, 4, 3}, {{0}, 4, 128}}}) {
    require(!MeterMap::create({bad}, 1), "invalid time signature accepted");
  }
  require(!MeterMap::create({{{0}, 4, 4}, {{0}, 3, 4}}, 1), "duplicate meter tick accepted");
  require(!MeterMap::create({{{0}, 4, 4}, {{ticks_per_quarter}, 3, 4}}, 1),
          "mid-bar meter change accepted");
  const auto map =
      take(MeterMap::create(
               {{{0}, 4, 4}, {{8 * ticks_per_quarter}, 3, 4}, {{14 * ticks_per_quarter}, 7, 8}}, 9),
           "aligned meter map rejected");
  REQUIRE_EQ(map.revision(), 9, "immutable meter map metadata changed");
  REQUIRE_EQ(map.events().size(), 3, "immutable meter map metadata changed");
  const std::array fixtures = {
      std::pair{TickPosition{0}, BarBeat{1, 1, 0}},
      std::pair{TickPosition{4 * ticks_per_quarter}, BarBeat{2, 1, 0}},
      std::pair{TickPosition{8 * ticks_per_quarter - 1}, BarBeat{2, 4, ticks_per_quarter - 1}},
      std::pair{TickPosition{8 * ticks_per_quarter}, BarBeat{3, 1, 0}},
      std::pair{TickPosition{11 * ticks_per_quarter}, BarBeat{4, 1, 0}},
      std::pair{TickPosition{14 * ticks_per_quarter}, BarBeat{5, 1, 0}},
      std::pair{TickPosition{14 * ticks_per_quarter + 3 * ticks_per_quarter / 2}, BarBeat{5, 4, 0}},
  };
  for (const auto& [tick, location] : fixtures) {
    REQUIRE_EQ(take(map.locate(tick), "meter location failed"), location,
               "bar/beat numbering wrong at fixture");
    REQUIRE_EQ(take(map.tick_at(location), "meter reverse location failed"), tick,
               "bar/beat did not return fixture tick");
  }
  require(!map.locate({-1}), "negative meter coordinate accepted");
  require(!map.tick_at({0, 1, 0}), "bar zero accepted");
  require(!map.tick_at({5, 8, 0}), "beat outside 7/8 accepted");
  require(!map.tick_at({5, 1, ticks_per_quarter / 2}), "tick outside eighth-note beat accepted");
  require(!map.tick_at({4, 4, 0}), "beat beyond preceding meter accepted");
  require(!map.tick_at({std::numeric_limits<std::int64_t>::max(), 1, 0}),
          "overflowing bar index accepted");
  const auto smallest_bar =
      take(MeterMap::create({{{0}, 1, 64}}, 10), "minimum-length bar rejected");
  constexpr std::int64_t shortest_bar_ticks = ticks_per_quarter / 16;
  const auto last_bar = std::numeric_limits<std::int64_t>::max() / shortest_bar_ticks + 1;
  require(!smallest_bar.tick_at({last_bar, 1, shortest_bar_ticks - 1}),
          "overflow within final representable bar accepted");
}

} // namespace

int main() {
  try {
    rates_and_exact_scaling();
    tempo_validation();
    steady_tempo_and_rounding();
    callback_tempo_conversion_matches_control();
    ramps_and_boundaries();
    meter_validation_and_location();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

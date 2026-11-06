#include "oscillate/automation/curve.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <vector>

using oscillate::test::counting_process_allocations;
using oscillate::test::process_allocations;

namespace {
using namespace oscillate;
using automation::BezierHandles;
using automation::blend;
using automation::ClipEnvelope;
using automation::Curve;
using automation::ParameterDescriptor;
using automation::Point;
using automation::PreparedAutomation;
using automation::Shape;
using automation::Timebase;
using automation::Warp;
using timeline::SamplePosition;
using timeline::TempoEvent;
using timeline::TempoMap;
using timeline::TempoShape;
using timeline::TickPosition;

using oscillate::test::require;

void near(double actual, double expected, double tolerance, const char* description) {
  REQUIRE_NEAR(actual, expected, tolerance, description);
}

std::shared_ptr<const TempoMap> make_tempo(bool ramp = false) {
  std::vector<TempoEvent> events{
      {TickPosition{0}, 120.0, ramp ? TempoShape::linear_bpm : TempoShape::step}};
  if (ramp) {
    events.push_back({TickPosition{timeline::ticks_per_quarter}, 60.0, TempoShape::step});
  }
  auto result = TempoMap::create(std::move(events), 91);
  REQUIRE_OK(result, "tempo fixture");
  return std::make_shared<const TempoMap>(std::move(result.value()));
}

PreparedAutomation prepare(Curve curve, ParameterDescriptor descriptor = {0.0, 1.0, 0.25, false},
                           std::shared_ptr<const TempoMap> tempo = make_tempo()) {
  auto result = PreparedAutomation::create(descriptor, std::move(curve), std::nullopt,
                                           std::move(tempo), 48'000);
  REQUIRE_OK(result, "valid curve preparation");
  return std::move(result.value());
}

double value_at(const PreparedAutomation& prepared, std::int64_t sample) {
  double value = -1.0;
  require(prepared.evaluate_block(SamplePosition{sample}, std::span{&value, 1}),
          "single sample evaluation");
  return value;
}

void interpolation_oracles() {
  constexpr auto beginning = std::int64_t{100};
  constexpr auto ending = std::int64_t{300};
  const auto segment = [](Shape shape, BezierHandles handles = {}) {
    return prepare(Curve{Timebase::elapsed, {{beginning, 0.0, shape, handles}, {ending, 1.0}}});
  };
  near(value_at(segment(Shape::hold), 200), 0.0, 0.0, "hold midpoint");
  near(value_at(segment(Shape::hold), ending), 1.0, 0.0, "right endpoint owns boundary");
  near(value_at(segment(Shape::linear), 200), 0.5, 1e-12, "linear midpoint");
  near(value_at(segment(Shape::quadratic), 200), 0.25, 1e-12, "quadratic midpoint");
  near(value_at(segment(Shape::exponential), 200), (std::exp(2.0) - 1.0) / (std::exp(4.0) - 1.0),
       1e-12, "exponential midpoint");
  near(value_at(segment(Shape::ease_in_out), 150), 0.15625, 1e-12, "smoothstep quarter point");
  near(value_at(segment(Shape::bezier, {0.25, 0.0, 0.75, 1.0}), 200), 0.5, 1e-7,
       "symmetric cubic Bezier midpoint");
  near(value_at(segment(Shape::bezier, {0.5, 0.0, 0.5, 0.0}), 200), 0.125, 1e-7,
       "cubic Bezier x inversion and y ordinate");
  near(value_at(segment(Shape::bezier, {0.5, 0.0, 0.5, 0.0}), beginning), 0.0, 0.0,
       "Bézier preserves exact control point value");
  near(value_at(segment(Shape::linear), 0), 0.0, 0.0, "left extrapolation clamped");
  near(value_at(segment(Shape::linear), 400), 1.0, 0.0, "right extrapolation clamped");
  const auto downward = prepare(Curve{Timebase::elapsed, {{100, 0.8, Shape::linear}, {300, 0.2}}});
  near(value_at(downward, 200), 0.5, 1e-12, "descending physical domain");
}

// A segment runs straight in its lane's space (schema 1.39): a gain in
// decibels with silence at -70 dB, a logarithmic parameter in its log.
void warped_lanes_blend_in_their_space() {
  const ParameterDescriptor gain{0.0, 4.0, 1.0, false, Warp::decibel};
  const auto fade = prepare(Curve{Timebase::elapsed, {{0, 0.5, Shape::linear}, {200, 2.0}}}, gain);
  near(value_at(fade, 100), 1.0, 1e-12, "a gain's midpoint is halfway in decibels");
  const auto in = prepare(Curve{Timebase::elapsed, {{0, 0.0, Shape::linear}, {100, 1.0}}}, gain);
  near(value_at(in, 50), std::pow(10.0, -35.0 / 20.0), 1e-12, "a fade-in rises from -70 dB");
  near(value_at(in, 0), 0.0, 0.0, "a fade starts in silence");
  const auto out = prepare(Curve{Timebase::elapsed, {{0, 1.0, Shape::linear}, {100, 0.0}}}, gain);
  near(value_at(out, 99), std::pow(10.0, -70.0 * 0.99 / 20.0), 1e-12, "a fade-out falls to -70 dB");
  const ParameterDescriptor hz{20.0, 20'000.0, 1'000.0, false, Warp::logarithmic};
  const auto sweep =
      prepare(Curve{Timebase::elapsed, {{0, 100.0, Shape::linear}, {200, 10'000.0}}}, hz);
  near(value_at(sweep, 100), 1'000.0, 1e-9, "a log sweep's midpoint is the geometric mean");
  near(value_at(sweep, 50), std::sqrt(100.0 * 1'000.0), 1e-9, "a log sweep is straight in octaves");
  // The shape applies in the warped space too.
  const auto eased =
      prepare(Curve{Timebase::elapsed, {{0, 100.0, Shape::quadratic}, {200, 10'000.0}}}, hz);
  near(value_at(eased, 100), std::pow(10.0, 2.0 + 2.0 * 0.25), 1e-9, "shapes run in the warp");
  near(blend(Warp::linear, 0.2, 0.8, 0.5), 0.5, 1e-15, "linear blend");
}

void discrete_and_defaults() {
  auto discrete = prepare(Curve{Timebase::elapsed, {{0, 2.0, Shape::linear}, {10, 5.0}}},
                          {0.0, 8.0, 1.0, true});
  near(value_at(discrete, 9), 2.0, 0.0, "discrete forces hold");
  near(value_at(discrete, 10), 5.0, 0.0, "discrete right boundary");
  auto empty = prepare(Curve{Timebase::elapsed, {}}, {0.0, 8.0, 3.0, true});
  near(value_at(empty, 123), 3.0, 0.0, "empty curve uses descriptor default");
  auto missing = PreparedAutomation::create({-1.0, 1.0, 0.7, false}, std::nullopt, std::nullopt,
                                            make_tempo(), 48'000);
  REQUIRE_OK(missing, "missing curves allowed");
  near(value_at(missing.value(), 7), 0.7, 0.0, "missing curves use default");
}

void musical_timing_and_precedence() {
  const auto ramp = make_tempo(true);
  const auto half_beat = TickPosition{timeline::ticks_per_quarter / 2};
  const auto ramp_middle = ramp->tick_to_sample(half_beat, 48'000);
  REQUIRE_OK(ramp_middle, "ramp midpoint conversion");
  auto musical = prepare(
      Curve{Timebase::musical, {{0, 0.0, Shape::linear}, {timeline::ticks_per_quarter, 1.0}}},
      {0.0, 1.0, 0.0, false}, ramp);
  near(value_at(musical, ramp_middle.value().value), 0.5, 1e-4,
       "musical curve follows tempo ramp in tick domain");
  std::array<TickPosition, 16> ticks{};
  std::array<double, 16> shared_values{};
  std::array<double, 16> direct_values{};
  const auto block_start = SamplePosition{ramp_middle.value().value - 8};
  require(musical.fill_musical_ticks(block_start, ticks), "authoritative tick block");
  require(musical.evaluate_block(block_start, ticks, shared_values), "shared tick evaluation");
  require(musical.evaluate_block(block_start, direct_values), "direct timing evaluation");
  for (std::size_t index = 0; index < ticks.size(); ++index) {
    near(shared_values[index], direct_values[index], 0.0,
         "shared tick and direct timing semantics match");
  }
  // A linear BPM ramp spends more than half its duration in its latter half;
  // blindly interpolating between sample-converted endpoints would fail here.
  const auto end = ramp->tick_to_sample(TickPosition{timeline::ticks_per_quarter}, 48'000);
  REQUIRE_OK(end, "ramp end conversion");
  REQUIRE_LT(static_cast<double>(ramp_middle.value().value) /
                 static_cast<double>(end.value().value),
             0.5, "tempo fixture differs from elapsed-linear midpoint");

  ClipEnvelope clip{Curve{Timebase::elapsed, {{0, 0.9, Shape::hold}}}, SamplePosition{101},
                    SamplePosition{103}};
  auto prepared = PreparedAutomation::create(
      {0.0, 1.0, 0.0, false}, Curve{Timebase::elapsed, {{100, 0.0, Shape::linear}, {104, 1.0}}},
      std::move(clip), ramp, 48'000);
  REQUIRE_OK(prepared, "clip precedence fixture");
  std::array<double, 5> output{};
  require(prepared.value().evaluate_block(SamplePosition{100}, output), "precedence block");
  near(output[0], 0.0, 0.0, "track before clip");
  near(output[1], 0.9, 0.0, "clip begins inclusively");
  near(output[2], 0.9, 0.0, "clip remains active");
  near(output[3], 0.75, 1e-12, "track resumes at current value");
  near(output[4], 1.0, 0.0, "track right endpoint");
  std::array<TickPosition, 5> shared_ticks{};
  std::array<double, 5> shared_output{};
  require(prepared.value().fill_musical_ticks(SamplePosition{100}, shared_ticks),
          "clip block tick preparation");
  require(prepared.value().evaluate_block(SamplePosition{100}, shared_ticks, shared_output),
          "clip precedence with shared ticks");
  for (std::size_t index = 0; index < output.size(); ++index) {
    near(shared_output[index], output[index], 0.0, "clip precedence paths agree");
  }
  auto default_only =
      PreparedAutomation::create({0.0, 1.0, 0.3, false}, std::nullopt, std::nullopt, ramp, 48'000);
  REQUIRE_OK(default_only, "default-only preparation");
  require(default_only.value().evaluate_block(SamplePosition{100}, shared_ticks, shared_output),
          "shared tick default-only evaluation");
  near(shared_output[0], 0.3, 0.0, "shared tick default value");
  REQUIRE_EQ(prepared.value().tempo_revision(), 91, "prepared timing identity");
  REQUIRE_EQ(prepared.value().sample_rate(), 48'000, "prepared timing identity");
}

void reject_malformed() {
  const auto tempo = make_tempo();
  const auto rejected = [&](ParameterDescriptor descriptor, std::optional<Curve> curve,
                            std::optional<ClipEnvelope> clip = std::nullopt,
                            std::shared_ptr<const TempoMap> map = make_tempo(),
                            std::uint32_t rate = oscillate::test::sample_rate_48k) {
    return !PreparedAutomation::create(descriptor, std::move(curve), std::move(clip),
                                       std::move(map), rate);
  };
  constexpr ParameterDescriptor descriptor{0.0, 1.0, 0.2, false};
  require(rejected({1.0, 0.0, 0.2, false}, std::nullopt), "inverted range");
  require(rejected({-1e308, 1e308, 0.0, false}, std::nullopt), "overflowing range span");
  require(rejected({0.0, 1.0, std::numeric_limits<double>::quiet_NaN(), false}, std::nullopt),
          "NaN default");
  require(rejected({0.0, 1.0, 2.0, false}, std::nullopt), "out-of-range default");
  require(rejected({0.5, 2.0, 1.0, true}, std::nullopt), "fractional discrete range");
  require(rejected(descriptor, std::nullopt, std::nullopt, nullptr), "null tempo map");
  require(rejected(descriptor, std::nullopt, std::nullopt, tempo, 1), "unsupported rate");
  require(rejected(descriptor, Curve{Timebase::elapsed, {{-1, 0.5}}}), "negative point");
  require(rejected(descriptor, Curve{Timebase::elapsed, {{1, 0.5}, {1, 0.6}}}), "duplicate point");
  require(rejected(descriptor, Curve{Timebase::elapsed, {{2, 0.5}, {1, 0.6}}}), "unsorted points");
  require(rejected(descriptor, Curve{Timebase::elapsed, {{1, 2.0}}}), "out-of-range value");
  require(rejected(descriptor,
                   Curve{Timebase::elapsed, {{1, std::numeric_limits<double>::infinity()}}}),
          "infinite point");
  require(rejected(descriptor,
                   Curve{Timebase::elapsed, {{1, 0.2, Shape::bezier, {0.8, 0.0, 0.2, 1.0}}}}),
          "nonmonotone Bezier x");
  require(rejected(descriptor,
                   Curve{Timebase::elapsed, {{1, 0.2, Shape::bezier, {0.2, 2.0, 0.8, 0.0}}}}),
          "out-of-range Bezier y");
  require(rejected({0.0, 5.0, 0.0, true}, Curve{Timebase::elapsed, {{1, 1.5}}}),
          "fractional discrete point");
  require(rejected(descriptor, std::nullopt,
                   ClipEnvelope{Curve{Timebase::elapsed, {{0, 0.2}}}, SamplePosition{5},
                                SamplePosition{5}}),
          "empty active interval");
  require(
      rejected(descriptor, std::nullopt,
               ClipEnvelope{Curve{Timebase::elapsed, {}}, SamplePosition{5}, SamplePosition{6}}),
      "empty clip envelope cannot override track automation");
  require(rejected(descriptor, std::nullopt,
                   ClipEnvelope{Curve{Timebase::elapsed, {{0, 0.2}}}, SamplePosition{-1},
                                SamplePosition{5}}),
          "negative active interval");
  auto valid = prepare(Curve{Timebase::elapsed, {}});
  double output = 0.0;
  require(!valid.evaluate_block(SamplePosition{-1}, std::span{&output, 1}),
          "negative block rejected");
  std::array<double, 2> large{};
  require(!valid.evaluate_block(SamplePosition{std::numeric_limits<std::int64_t>::max()}, large),
          "overflowing block rejected");
  std::array<TickPosition, 2> ticks{};
  require(!valid.fill_musical_ticks(SamplePosition{-1}, ticks), "negative tick block rejected");
  require(
      !valid.fill_musical_ticks(SamplePosition{std::numeric_limits<std::int64_t>::max()}, ticks),
      "overflowing tick block rejected");
  const auto large_sample = SamplePosition{std::numeric_limits<std::int64_t>::max() - 2};
  require(!valid.fill_musical_ticks(large_sample, ticks), "timeline tick overflow rejected");
  auto musical = prepare(Curve{Timebase::musical, {{0, 0.4}}});
  require(!musical.evaluate_block(large_sample, std::span{&output, 1}),
          "direct timeline overflow rejected");
  require(!valid.evaluate_block(SamplePosition{0}, std::span{ticks}.first(1), large),
          "tick/output span mismatch rejected");
}

void callback_allocation_probe() {
  auto prepared = prepare(
      Curve{Timebase::musical,
            {{0, 0.1, Shape::bezier, {0.3, 0.1, 0.7, 0.9}}, {timeline::ticks_per_quarter, 0.8}}});
  std::array<double, 128> buffer{};
  std::array<TickPosition, 128> ticks{};
  process_allocations.store(0, std::memory_order_relaxed);
  counting_process_allocations.store(true, std::memory_order_relaxed);
  bool succeeded = true;
  for (int block = 0; block < 200; ++block) {
    const auto first = SamplePosition{static_cast<std::int64_t>(block) * 128};
    succeeded = prepared.fill_musical_ticks(first, ticks) && succeeded;
    succeeded = prepared.evaluate_block(first, ticks, buffer) && succeeded;
  }
  counting_process_allocations.store(false, std::memory_order_relaxed);
  require(succeeded, "callback simulation succeeded");
  REQUIRE_EQ(process_allocations.load(std::memory_order_relaxed), 0,
             "callback simulation allocated no heap memory");
}

} // namespace

int main() {
  try {
    interpolation_oracles();
    warped_lanes_blend_in_their_space();
    discrete_and_defaults();
    musical_timing_and_precedence();
    reject_malformed();
    callback_allocation_probe();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

#include "effect_test_support.hpp"

#include "oscillate/native_dsp/effects/trim.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

// Trim (ADR-0042): bit-exact at its defaults and back at them after a move;
// gain and the four pan laws exact; width, mono, swap, polarity and the
// four solos exact; the phase rotator flat in magnitude while it lowers a
// peaky waveform's crest; the DC block; click-free toggles.
namespace {
using namespace effect_test;
namespace trim = oscillate::native_dsp::effects::trim;

const EngineDescriptor& table() { return *trim::engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }
std::vector<double> unity() { return preset(table(), "Unity"); }

Stereo run(const std::vector<double>& values, const Stereo& input) {
  auto runtime = make_effect(trim::engine(), values);
  return process(*runtime, input);
}

double as_float(double x) { return static_cast<double>(static_cast<float>(x)); }

Stereo slice_of(const Stereo& s, std::size_t from, std::size_t to) {
  return {std::vector<double>(s.left.begin() + static_cast<std::ptrdiff_t>(from),
                              s.left.begin() + static_cast<std::ptrdiff_t>(to)),
          std::vector<double>(s.right.begin() + static_cast<std::ptrdiff_t>(from),
                              s.right.begin() + static_cast<std::ptrdiff_t>(to))};
}

void unity_is_bit_exact() {
  const auto input = noise(0.5, 48'000);
  const auto out = run(unity(), input);
  for (std::size_t i = 0; i < input.size(); ++i) {
    REQUIRE_EQ(out.left[i], as_float(input.left[i]), "Unity is bit-exact");
    REQUIRE_EQ(out.right[i], as_float(input.right[i]), "Unity is bit-exact");
  }
  // Away and back: exact again once settled.
  auto runtime = make_effect(trim::engine(), unity());
  auto moved = unity();
  moved[at("trim.gain")] = -6.0;
  moved[at("trim.pan")] = 0.4;
  moved[at("trim.width")] = 1.5;
  require(runtime->offer_values(moved, 1), "a move offered");
  (void)process(*runtime, slice_of(input, 0, 12'000));
  require(runtime->offer_values(unity(), 2), "back offered");
  (void)process(*runtime, slice_of(input, 12'000, 24'000), {}, 12'000);
  const auto later = process(*runtime, slice_of(input, 24'000, 48'000), {}, 24'000);
  for (std::size_t i = 0; i < later.size(); ++i) {
    REQUIRE_EQ(later.left[i], as_float(input.left[24'000 + i]), "back at unity, bit-exact again");
  }
}

void gain_and_pan_laws_are_exact() {
  const auto input = noise(0.3, 12'000, true);
  auto values = unity();
  values[at("trim.gain")] = -6.0;
  const auto out = run(values, input);
  const double k = std::pow(10.0, -6.0 / 20.0);
  for (std::size_t i = 0; i < input.size(); ++i) {
    REQUIRE_LT(std::fabs(out.left[i] - k * input.left[i]), tolerance::float_round_trip,
               "gain is exact");
  }
  struct Case {
    double law;
    double pan;
    double left;
    double right;
  };
  const double t = 3.0 * std::numbers::pi / 8.0; // pan +0.5
  const std::vector<Case> cases{
      {0, 0.0, 1.0, 1.0},
      {0, 1.0, 0.0, 1.0},
      {0, 0.5, 0.5, 1.0},
      {1, 0.0, 1.0, 1.0},
      {1, 1.0, 0.0, std::numbers::sqrt2},
      {1, 0.5, std::numbers::sqrt2 * std::cos(t), std::numbers::sqrt2 * std::sin(t)},
      {2, 1.0, 0.0, std::sqrt(std::numbers::sqrt2 * 2.0)},
      {3, 0.0, 1.0, 1.0},
      {3, 1.0, 0.0, 2.0},
      {3, 0.5, 0.5, 1.5},
  };
  for (const auto& c : cases) {
    auto v = unity();
    v[at("trim.law")] = c.law;
    v[at("trim.pan")] = c.pan;
    const auto o = run(v, input);
    for (std::size_t i = 0; i < input.size(); i += 97) {
      require(std::fabs(o.left[i] - c.left * input.left[i]) < tolerance::float_round_trip &&
                  std::fabs(o.right[i] - c.right * input.right[i]) < tolerance::float_round_trip,
              "law " + std::to_string(c.law) + " at pan " + std::to_string(c.pan));
    }
  }
}

void stereo_stages_are_exact() {
  const auto input = noise(0.3, 12'000);
  const auto check = [&](std::string_view id, double value, auto expected) {
    auto v = unity();
    v[at(id)] = value;
    const auto o = run(v, input);
    for (std::size_t i = 0; i < input.size(); ++i) {
      const auto [l, r] = expected(input.left[i], input.right[i]);
      require(std::fabs(o.left[i] - l) < tolerance::float_round_trip &&
                  std::fabs(o.right[i] - r) < tolerance::float_round_trip,
              std::string(id) + " = " + std::to_string(value) + " is exact");
    }
  };
  using P = std::pair<double, double>;
  check("trim.width", 0.0, [](double l, double r) { return P{0.5 * (l + r), 0.5 * (l + r)}; });
  check("trim.width", 2.0, [](double l, double r) {
    const double m = 0.5 * (l + r);
    const double s = l - r;
    return P{m + s, m - s};
  });
  check("trim.mono", 1.0, [](double l, double r) { return P{0.5 * (l + r), 0.5 * (l + r)}; });
  check("trim.swap", 1.0, [](double l, double r) { return P{r, l}; });
  check("trim.invert", 3.0, [](double l, double r) { return P{-l, -r}; });
  check("trim.invert", 1.0, [](double l, double r) { return P{-l, r}; });
  check("trim.solo", 1.0, [](double l, double) { return P{l, l}; });
  check("trim.solo", 2.0, [](double, double r) { return P{r, r}; });
  check("trim.solo", 3.0, [](double l, double r) { return P{0.5 * (l + r), 0.5 * (l + r)}; });
  check("trim.solo", 4.0, [](double l, double r) { return P{0.5 * (l - r), 0.5 * (l - r)}; });
}

void the_rotator_is_flat_and_lowers_peaks() {
  auto values = unity();
  values[at("trim.rotate")] = 1.0;
  for (const double hz : {30.0, 150.0, 1'000.0, 8'000.0, 18'000.0}) {
    const auto tone = sine(hz, 0.5, 48'000);
    const auto out = run(values, tone);
    const double change = db(rms(out.left, 24'000, 48'000) / rms(tone.left, 24'000, 48'000));
    REQUIRE_LT(std::fabs(change), 0.01,
               "phase rotation is flat at " + std::to_string(hz) +
                   " Hz: " + std::to_string(change) + " dB");
  }
  // A peaky, phase-aligned harmonic stack (a band-limited pulse at 55 Hz).
  Stereo stack{std::vector<double>(96'000), std::vector<double>(96'000)};
  for (std::size_t i = 0; i < stack.size(); ++i) {
    double x = 0.0;
    for (int h = 1; h <= 8; ++h) {
      x += std::cos(2.0 * std::numbers::pi * 55.0 * h * static_cast<double>(i) / 48'000.0) / 8.0;
    }
    stack.left[i] = stack.right[i] = 0.8 * x;
  }
  const auto out = run(values, stack);
  const double crest_in = db(peak(stack.left, 48'000) / rms(stack.left, 48'000, 96'000));
  const double crest_out = db(peak(out.left, 48'000) / rms(out.left, 48'000, 96'000));
  std::cout << "  phase rotation: crest " << crest_in << " -> " << crest_out << " dB\n";
  REQUIRE_LT(crest_out, crest_in - 1.0, "rotation lowers a peaky waveform's crest");
  REQUIRE_LT(std::fabs(db(rms(out.left, 48'000, 96'000) / rms(stack.left, 48'000, 96'000))), 0.05,
             "without changing its level");
}

void dc_is_blocked() {
  auto input = sine(200.0, 0.3, 96'000);
  for (std::size_t i = 0; i < input.size(); ++i) {
    input.left[i] += 0.2;
    input.right[i] += 0.2;
  }
  auto values = unity();
  values[at("trim.dc")] = 1.0;
  const auto out = run(values, input);
  double mean = 0.0;
  for (std::size_t i = 48'000; i < 96'000; ++i) {
    mean += out.left[i];
  }
  mean /= 48'000.0;
  REQUIRE_LT(std::fabs(mean), tolerance::near_silence,
             "the DC block removes the offset: " + std::to_string(mean));
}

void toggles_glide() {
  const auto input = sine(220.0, 0.4, 48'000);
  auto offset = input;
  for (std::size_t i = 0; i < offset.size(); ++i) {
    offset.right[i] =
        0.4 * std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(i) / 48'000.0 + 2.0);
  }
  for (const auto& [id, to] :
       std::vector<std::pair<std::string_view, double>>{{"trim.swap", 1.0},
                                                        {"trim.invert", 1.0},
                                                        {"trim.mono", 1.0},
                                                        {"trim.solo", 4.0},
                                                        {"trim.rotate", 1.0}}) {
    auto runtime = make_effect(trim::engine(), unity());
    auto next = unity();
    next[at(id)] = to;
    const auto out =
        process(*runtime, offset, {.at = {24'000}, .before = [&](std::uint64_t) {
                                     require(runtime->offer_values(next, 1), "offered");
                                   }});
    double steady = 0.0;
    for (std::size_t i = 12'001; i < 24'000; ++i) {
      steady = std::max(steady, std::fabs(out.left[i] - out.left[i - 1]));
    }
    double across = 0.0;
    for (std::size_t i = 24'000; i < 25'200; ++i) {
      across = std::max(across, std::fabs(out.left[i] - out.left[i - 1]));
    }
    REQUIRE_LT(across, steady * 1.3 + 1e-3, std::string(id) + " glides in");
  }
}

} // namespace

int main() {
  try {
    unity_is_bit_exact();
    gain_and_pan_laws_are_exact();
    stereo_stages_are_exact();
    the_rotator_is_flat_and_lowers_peaks();
    dc_is_blocked();
    toggles_glide();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "native trim contract passed\n";
  return 0;
}

#include "oscillate/native_dsp/synth/modal_dsp.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

// The shared modal library (ADR-0068): the felt contact law Knock's strikers
// use, and the bank Knock rings its objects with.
namespace {
using namespace oscillate::native_dsp::synth;

constexpr double pi = 3.14159265358979323846;

using oscillate::test::require;

double cents(double measured, double expected) { return 1200.0 * std::log2(measured / expected); }

// Rising zero crossings, interpolated.
double measured_hz(const std::vector<double>& x, std::size_t from, std::size_t to, double rate) {
  double first = -1.0;
  double last = -1.0;
  int crossings = 0;
  for (std::size_t i = from + 1; i < to; ++i) {
    if (x[i - 1] < 0.0 && x[i] >= 0.0) {
      const double t = static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
      if (first < 0.0) {
        first = t;
      } else {
        ++crossings;
      }
      last = t;
    }
  }
  return rate * crossings / (last - first);
}

// The Stulov felt law, written out independently.
double stulov(double k, double p, double alpha, double& last_xp, double x, double dt,
              double share) {
  const double xp = x > 0.0 ? std::pow(x, p) : 0.0;
  const double rate = (xp - last_xp) / dt;
  last_xp = xp;
  return std::max(0.0, share * k * (xp + alpha * rate));
}

void contact_law_is_stulov() {
  constexpr double k = 4.5e9;
  constexpr double p = 2.5;
  constexpr double alpha = 2e-4;
  modal::ContactLaw law{k, p, alpha};
  double reference_xp = 0.0;
  const double dt = 1.0 / (48'000.0 * 8.0);
  for (int step = 0; step < 400; ++step) {
    // Into the felt and out again, past release.
    const double x = 1e-3 * std::sin(pi * static_cast<double>(step) / 300.0);
    const double expected = stulov(k, p, alpha, reference_xp, x, dt, 0.5);
    REQUIRE_EQ(law.force(x, dt, 0.5), expected, "the law is the Stulov formula, bit for bit");
  }
  REQUIRE_EQ(law.last_xp, reference_xp, "the state matches");
  law.reset();
  REQUIRE_EQ(law.force(-1e-4, dt), 0.0, "a felt never pulls");
}

void rigid_contact_round_trips() {
  for (const double p : {1.0, 1.5, 2.5, 3.0}) {
    const modal::RigidStrike strike{.mass = 0.01, .exponent = p, .speed = 1.5};
    const double k = modal::rigid_contact_stiffness(strike, 0.001);
    REQUIRE_LT(std::fabs(modal::rigid_contact_seconds(strike, k) / 0.001 - 1.0),
               tolerance::double_accumulation, "stiffness and contact time are inverses");
  }
  // A linear spring holds for half a period.
  const double k =
      modal::rigid_contact_stiffness({.mass = 0.02, .exponent = 1.0, .speed = 2.0}, 0.004);
  REQUIRE_LT(std::fabs(pi * std::sqrt(0.02 / k) / 0.004 - 1.0), tolerance::double_accumulation,
             "p = 1 is half a period");
}

// One mode, struck: its frequency and T60 are as designed.
void a_mode_rings_as_designed() {
  for (const double rate : {44'100.0, 48'000.0, 96'000.0}) {
    for (const double hz : {55.0, 440.0, 9'000.0}) {
      modal::ModalBank bank;
      bank.prepare(4, rate);
      const double t60 = 0.6;
      require(bank.add({hz, t60, 1.0, 1e-3, 1.0, 0.0}), "the mode is added");
      require(!bank.add({0.6 * rate, t60, 1.0, 1e-3, 1.0, 0.0}), "a mode above Nyquist is not");
      const auto frames = static_cast<std::size_t>(0.5 * rate);
      std::vector<double> x(frames);
      x[0] = bank.step(1.0 * rate, 0.0); // A unit impulse of momentum.
      const double start = bank.amplitude();
      for (std::size_t i = 1; i < frames; ++i) {
        x[i] = bank.step(0.0, 0.0);
      }
      const double end = bank.amplitude();
      const double measured = measured_hz(x, frames / 10, frames, rate);
      REQUIRE_LT(std::fabs(cents(measured, hz)), 0.5, "the mode rings within half a cent");
      const double seconds = static_cast<double>(frames - 1) / rate;
      const double measured_t60 = seconds * modal::ln1000 / std::log(start / end);
      REQUIRE_LT(std::fabs(measured_t60 / t60 - 1.0), 0.02, "the mode's T60 is within 2%");
      REQUIRE_LT(std::fabs(bank.hz(0) - hz), tolerance::double_accumulation,
                 "the designed values are reported");
      REQUIRE_LT(std::fabs(bank.t60(0) - t60), tolerance::double_accumulation,
                 "the designed values are reported");
    }
  }
}

// An impulse of momentum J rings a mode at shape J / (mass omega) metres.
void an_impulse_moves_the_mode_by_its_mass() {
  modal::ModalBank bank;
  bank.prepare(1, 48'000.0);
  const double hz = 300.0;
  const double mass = 0.2;
  const double shape = 0.7;
  require(bank.add({hz, 100.0, shape, mass, 1.0, 0.0}), "the mode is added");
  (void)bank.step(0.01 * 48'000.0, 0.0);
  double peak = 0.0;
  for (int i = 0; i < 400; ++i) {
    peak = std::max(peak, std::fabs(static_cast<double>(bank.step(0.0, 0.0))));
  }
  const double expected = shape * 0.01 / (mass * 2.0 * pi * hz);
  REQUIRE_LT(std::fabs(peak / expected - 1.0), 0.01, "the displacement follows the modal mass");
  REQUIRE_LE(std::fabs(bank.displacement()), shape * expected * 1.0001,
             "the displacement at the strike point is shaped");
}

// A steady input at resonance comes out at `drive`.
void driven_gain_is_unity_at_resonance() {
  const double rate = oscillate::test::sample_rate_48k_hz;
  for (const double drive : {1.0, 0.4}) {
    modal::ModalBank bank;
    bank.prepare(2, rate);
    require(bank.add({1'000.0, 0.2, 0.0, 1.0, 1.0, drive}), "the mode is added");
    double peak = 0.0;
    const auto frames = static_cast<std::size_t>(1.5 * rate);
    for (std::size_t i = 0; i < frames; ++i) {
      const double u = std::cos(2.0 * pi * 1'000.0 * static_cast<double>(i) / rate);
      const double y = bank.step(0.0, u);
      if (i > frames - static_cast<std::size_t>(0.2 * rate)) {
        peak = std::max(peak, std::fabs(y));
      }
    }
    REQUIRE_LT(std::fabs(peak / drive - 1.0), 0.03, "unity gain at resonance, times drive");
  }
}

void retune_moves_every_mode() {
  const double rate = oscillate::test::sample_rate_48k_hz;
  for (const double factor : {1.5, 0.5, 2.0}) {
    modal::ModalBank bank;
    bank.prepare(1, rate);
    require(bank.add({440.0, 2.0, 1.0, 1e-3, 1.0, 0.0}), "the mode is added");
    bank.retune(factor);
    const auto frames = static_cast<std::size_t>(0.3 * rate);
    std::vector<double> x(frames);
    x[0] = bank.step(rate, 0.0);
    for (std::size_t i = 1; i < frames; ++i) {
      x[i] = bank.step(0.0, 0.0);
    }
    REQUIRE_LT(std::fabs(cents(measured_hz(x, 100, frames, rate), 440.0 * factor)), 0.5,
               "retune moves the frequency");
    REQUIRE_EQ(bank.factor(), factor, "the design is kept");
    REQUIRE_EQ(bank.hz(0), 440.0, "the design is kept");
  }
}

// A striker against a heavy bank stays in contact about as long as against
// a rigid surface, and comes off it.
void a_striker_bounces_off_a_heavy_bank() {
  const double rate = oscillate::test::sample_rate_48k_hz;
  for (const double seconds : {0.004, 0.001, 0.0003}) {
    modal::ModalBank bank;
    bank.prepare(8, rate);
    for (int n = 1; n <= 6; ++n) {
      // Each mode 40 strikers heavy times the six shapes: a heavy surface.
      require(bank.add({300.0 * n, 1.0, 1.0, 40.0 * 0.01 * 6.0, 1.0, 0.0}), "mode added");
    }
    modal::Striker striker;
    striker.mass = 0.01;
    striker.law.p = 2.0;
    striker.law.k = modal::rigid_contact_stiffness(
        {.mass = striker.mass, .exponent = 2.0, .speed = 1.5}, seconds);
    striker.launch(0.0, 1.5);
    int frames = 0;
    while (striker.active && frames < 4'800) {
      (void)bank.step_contact(striker, 0.0, 0.0);
      ++frames;
    }
    REQUIRE_TRUE(!striker.active, "the striker comes off");
    REQUIRE_LT(striker.speed, 0.0, "the striker comes off");
    REQUIRE_LT(std::fabs(striker.contact_seconds / seconds - 1.0), 0.1,
               "contact lasts about the rigid time");
    REQUIRE_GT(bank.energy(), 0.0, "the bank rings");
  }
}

void retire_silences() {
  modal::ModalBank bank;
  bank.prepare(2, 48'000.0);
  require(bank.add({500.0, 0.01, 1.0, 1.0, 1.0, 0.0}), "mode added");
  (void)bank.step(48'000.0, 0.0);
  require(!bank.retire_below(1e-12), "a ringing bank is not retired");
  for (int i = 0; i < 4'800; ++i) {
    (void)bank.step(0.0, 0.0);
  }
  REQUIRE_TRUE(bank.retire_below(1e-5), "a quiet bank is silenced");
  REQUIRE_EQ(bank.amplitude(), 0.0, "a quiet bank is silenced");
  bank.clear();
  REQUIRE_EQ(bank.size(), 0, "clear keeps the capacity");
  REQUIRE_EQ(bank.capacity(), 2, "clear keeps the capacity");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "modal dsp contract passed",
      {
          {"contact_law_is_stulov", [] { contact_law_is_stulov(); }},
          {"rigid_contact_round_trips", [] { rigid_contact_round_trips(); }},
          {"a_mode_rings_as_designed", [] { a_mode_rings_as_designed(); }},
          {"an_impulse_moves_the_mode_by_its_mass",
           [] { an_impulse_moves_the_mode_by_its_mass(); }},
          {"driven_gain_is_unity_at_resonance", [] { driven_gain_is_unity_at_resonance(); }},
          {"retune_moves_every_mode", [] { retune_moves_every_mode(); }},
          {"a_striker_bounces_off_a_heavy_bank", [] { a_striker_bounces_off_a_heavy_bank(); }},
          {"retire_silences", [] { retire_silences(); }},
      });
}

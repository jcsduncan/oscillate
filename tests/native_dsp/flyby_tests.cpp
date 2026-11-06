#include "effect_test_support.hpp"

#include "oscillate/native_dsp/effects/flyby.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

// Flyby (ADR-0067) against the physics: a source parked at the reference
// distance in front is the input delayed by the latency; the pitch heard is
// the Doppler shift of the source's speed toward the listener at the time
// it emitted; the ears hear Woodworth's time and Brown and Duda's shadow;
// the level follows the distance law, the air darkens with distance and
// the room grows against the direct sound; a loop wraps without a click;
// Once passes on each note; any block split renders the same.
namespace {
using namespace effect_test;
namespace flyby = oscillate::native_dsp::effects::flyby;

const effects::EffectEngine& engine() { return flyby::engine(); }
const EngineDescriptor& table() { return *engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr double c = flyby::speed_of_sound;
constexpr double kmh = 1.0 / 3.6;

std::vector<double> defaults() { return preset(table(), "Train"); }

// A straight path from (x1, y) to (x2, y), its handles on the line.
void line(std::vector<double>& v, double x1, double x2, double y) {
  const std::array<double, 4> xs{x1, x1 + (x2 - x1) / 3.0, x1 + 2.0 * (x2 - x1) / 3.0, x2};
  for (std::size_t i = 0; i < 4; ++i) {
    v[at("flyby.x" + std::to_string(i + 1))] = xs[i];
    v[at("flyby.y" + std::to_string(i + 1))] = y;
  }
}

// A source parked at (x, y): a path of no length, in Position mode, with
// nothing but the distance and the head.
std::vector<double> parked(double x, double y, double listen = 1.0) {
  auto v = defaults();
  line(v, x, x, y);
  v[at("flyby.run")] = 3;
  v[at("flyby.air")] = 0;
  v[at("flyby.room")] = 0;
  v[at("flyby.listen")] = listen;
  return v;
}

std::uint32_t latency(const std::vector<double>& v) { return engine().latency_frames(v, rate); }

Stereo render(const std::vector<double>& values, const Stereo& input, std::size_t block = 128) {
  auto runtime = make_effect(engine(), values, rate);
  return process(*runtime, input, {.block = block, .rate = rate});
}

Stereo impulse(std::size_t frames, std::size_t at_frame = 100) {
  Stereo out{std::vector<double>(frames), std::vector<double>(frames)};
  out.left[at_frame] = out.right[at_frame] = 0.5;
  return out;
}

std::size_t peak_at(const std::vector<double>& x) {
  return static_cast<std::size_t>(
      std::max_element(x.begin(), x.end(),
                       [](double a, double b) { return std::fabs(a) < std::fabs(b); }) -
      x.begin());
}

// Frequency from zero crossings, interpolated, over [from, to).
double frequency(const std::vector<double>& x, std::size_t from, std::size_t to) {
  double first = -1.0;
  double last = -1.0;
  int crossings = 0;
  for (std::size_t i = from + 1; i < to; ++i) {
    if (x[i - 1] < 0.0 && x[i] >= 0.0) {
      const double at_crossing = static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
      if (first < 0.0) {
        first = at_crossing;
      } else {
        ++crossings;
      }
      last = at_crossing;
    }
  }
  return crossings > 0 ? crossings * rate / (last - first) : 0.0;
}

void at_rest_in_front_is_the_input() {
  const auto v = parked(0.0, 5.0); // At the reference distance, speakers.
  const auto in = sine(440.0, 0.3, 24'000);
  const auto out = render(v, in, 0);
  const std::uint32_t d = latency(v);
  double worst = 0.0;
  for (std::size_t i = d + 2'400; i < in.size(); ++i) {
    worst = std::max({worst, std::fabs(out.left[i] - in.left[i - d]),
                      std::fabs(out.right[i] - in.right[i - d])});
  }
  REQUIRE_LT(worst, 1e-5,
             "a source parked at the reference distance in front is the input, delayed (" +
                 std::to_string(worst) + ")");
}

void the_latency_is_exact() {
  const auto v = parked(0.0, 5.0, 0.0); // Headphones: in front, no time between the ears.
  const std::uint32_t d = latency(v);
  REQUIRE_GT(d, 0, "the latency is the ears' margin");
  REQUIRE_LT(d, 64, "the latency is the ears' margin");
  const auto out = render(v, impulse(4'800));
  REQUIRE_EQ(peak_at(out.left), 100 + d,
             "an impulse from straight ahead arrives exactly one latency later");
  REQUIRE_EQ(peak_at(out.right), 100 + d,
             "an impulse from straight ahead arrives exactly one latency later");
}

void doppler_follows_the_physics() {
  const double speed = 300.0;
  const double v_ms = speed * kmh;
  const double y = 10.0;
  auto v = parked(0.0, y);
  line(v, -150.0, 150.0, y);
  v[at("flyby.run")] = 0; // Loop, on the song's clock from frame 0.
  v[at("flyby.speed")] = speed;
  const double f = 1'000.0;
  const auto out = render(v, sine(f, 0.3, static_cast<std::size_t>(3.6 * rate)));
  const double lead =
      static_cast<double>(latency(v)) / rate - y / c; // Output time - physical time.
  for (const double x : {-100.0, -40.0, 0.0, 40.0, 100.0}) {
    // Emitted at x (time from the start), heard d / c later, shifted by the
    // compensation and the latency.
    const double tau = (x + 150.0) / v_ms;
    const double d = std::hypot(x, y);
    const double heard = tau + d / c + lead;
    const auto centre = static_cast<std::size_t>(heard * rate);
    const std::size_t half = static_cast<std::size_t>(0.01 * rate);
    const double measured = frequency(out.left, centre - half, centre + half);
    const double expected = f / (1.0 + v_ms * (x / d) / c);
    REQUIRE_LT(std::fabs(measured / expected - 1.0), 0.003,
               "at x = " + std::to_string(x) + " m the pitch is " + std::to_string(measured) +
                   " Hz, the Doppler shift says " + std::to_string(expected) + " Hz");
  }
}

void the_ears_hear_the_head() {
  const auto v = parked(5.0, 0.0, 0.0); // To the right, headphones.
  const auto out = render(v, impulse(4'800));
  const double lag =
      static_cast<double>(peak_at(out.left)) - static_cast<double>(peak_at(out.right));
  const double woodworth = flyby::head_radius / c * (std::numbers::pi / 2.0 + 1.0) * rate;
  REQUIRE_LT(std::fabs(lag - woodworth), 1.5,
             "the right ear hears first by Woodworth's time (" + std::to_string(lag) +
                 " frames, expected " + std::to_string(woodworth) + ")");
  const auto tone = render(v, sine(8'000.0, 0.3, 24'000));
  const double shadow = db(rms(tone.right, 4'800, 24'000) / rms(tone.left, 4'800, 24'000));
  REQUIRE_GT(shadow, 10.0,
             "the head shadows the far ear at 8 kHz (" + std::to_string(shadow) + " dB)");
  const auto speakers = render(parked(5.0, 0.0, 1.0), impulse(4'800));
  REQUIRE_GT(std::fabs(out.left[peak_at(out.left)]), 0.0,
             "on speakers both channels arrive together (a pan, no ear timing)");
  REQUIRE_EQ(peak_at(speakers.left), peak_at(speakers.right),
             "on speakers both channels arrive together (a pan, no ear timing)");
}

void the_level_follows_distance() {
  const auto tone = sine(500.0, 0.3, 24'000);
  const auto level = [&](double y, double rolloff) {
    auto v = parked(0.0, y);
    v[at("flyby.rolloff")] = rolloff;
    const auto out = render(v, tone);
    return db(rms(out.left, 9'600, 24'000));
  };
  REQUIRE_LT(std::fabs(level(10.0, 1.0) - level(20.0, 1.0) - 6.02), 0.05,
             "twice as far is 6 dB quieter");
  REQUIRE_LT(std::fabs(level(10.0, 0.0) - level(20.0, 0.0)), 0.05,
             "with Distance at 0 it does not fall");
}

void the_air_darkens_with_distance() {
  const auto tone = sine(10'000.0, 0.3, 24'000);
  const auto loss = [&](double y) {
    auto v = parked(0.0, y);
    const auto dry = render(v, tone);
    v[at("flyby.air")] = 1.0;
    const auto wet = render(v, tone);
    return db(rms(dry.left, 9'600, 24'000) / rms(wet.left, 9'600, 24'000));
  };
  const double near = loss(10.0);
  const double far = loss(100.0);
  require(near < 3.0 && far > 5.0 && far < 12.0, "air takes " + std::to_string(near) +
                                                     " dB at 10 m and " + std::to_string(far) +
                                                     " dB at 100 m from 10 kHz");
}

void the_room_grows_with_distance() {
  const auto tone = noise(0.2, 48'000, true);
  const auto ratio = [&](double y) {
    auto v = parked(0.0, y);
    const auto direct = render(v, tone);
    v[at("flyby.room")] = 1.0;
    const auto both = render(v, tone);
    std::vector<double> room(both.size());
    for (std::size_t i = 0; i < room.size(); ++i) {
      room[i] = both.left[i] - direct.left[i];
    }
    return db(rms(room, 24'000, 48'000) / rms(direct.left, 24'000, 48'000));
  };
  REQUIRE_GT(ratio(80.0) - ratio(5.0), 20.0,
             "far away you hear more of the room than of the source");
}

void a_loop_wraps_without_a_click() {
  auto v = parked(0.0, 5.0);
  line(v, -20.0, 20.0, 5.0);
  v[at("flyby.run")] = 0;
  v[at("flyby.speed")] = 200.0;
  const double f = 220.0;
  const auto out = render(v, sine(f, 0.3, static_cast<std::size_t>(3.0 * rate)));
  double peak = 0.0;
  double step = 0.0;
  for (std::size_t i = 1; i < out.size(); ++i) {
    peak = std::max(peak, std::fabs(out.left[i]));
    step = std::max(step, std::fabs(out.left[i] - out.left[i - 1]));
  }
  const double top = f / (1.0 - 200.0 * kmh / c);
  const double bound = 1.5 * 2.0 * std::numbers::pi * top * peak / rate;
  REQUIRE_LT(step, bound,
             "the wrap from the end of the pass to its start does not click (" +
                 std::to_string(step) + " against " + std::to_string(bound) + ")");
}

void once_passes_on_each_note() {
  auto v = parked(0.0, 5.0);
  line(v, -30.0, 30.0, 5.0);
  v[at("flyby.run")] = 1;
  v[at("flyby.speed")] = 108.0; // 30 m/s: the 60 m pass takes 2 s.
  auto runtime = make_effect(engine(), v, rate);
  require(runtime->offer_key_notes({{static_cast<std::int64_t>(rate), 60, 100}}),
          "the note is offered");
  const auto out =
      process(*runtime, sine(300.0, 0.3, static_cast<std::size_t>(4.0 * rate)), {.rate = rate});
  const auto level = [&](double from, double to) {
    return rms(out.left, static_cast<std::size_t>(from * rate),
               static_cast<std::size_t>(to * rate));
  };
  REQUIRE_LT(level(0.1, 0.9), 1e-6, "silent before the note");
  REQUIRE_GT(level(1.8, 2.2), 0.05, "passing after it");
  REQUIRE_LT(level(3.6, 3.95), 1e-4, "and silent again once it has passed");
}

void any_block_split_renders_the_same() {
  for (const double run : {0.0, 2.0}) {
    auto v = defaults();
    v[at("flyby.run")] = run;
    const auto in = noise(0.2, 48'000);
    const auto a = render(v, in, 128);
    const auto b = render(v, in, 0);
    // Loop counts frames, so it is exact. Sync reads the song's beat, which
    // the host sums per block (start beat plus frames since), so it may
    // differ by that sum's rounding.
    const double tolerance = run == 0.0 ? 0.0 : 1e-9;
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
      worst =
          std::max({worst, std::fabs(a.left[i] - b.left[i]), std::fabs(a.right[i] - b.right[i])});
    }
    REQUIRE_LE(worst, tolerance,
               std::string(run == 0.0 ? "Loop" : "Sync") +
                   " renders the same however the blocks fall (" + std::to_string(worst) + ")");
  }
}

void telemetry_feeds_the_panel() {
  auto v = parked(-20.0, 7.0, 0.0);
  auto runtime = make_effect(engine(), v, rate);
  (void)process(*runtime, sine(440.0, 0.3, 9'600), {.rate = rate});
  std::vector<float> frame;
  REQUIRE_TRUE(runtime->read_telemetry(frame), "a telemetry frame is published");
  REQUIRE_EQ(frame.size(), flyby::telemetry::size, "a telemetry frame is published");
  REQUIRE_LT(std::fabs(frame[flyby::telemetry::x] + 20.0F), oscillate::test::tolerance::coarse,
             "the source's position");
  REQUIRE_LT(std::fabs(frame[flyby::telemetry::y] - 7.0F), oscillate::test::tolerance::coarse,
             "the source's position");
  REQUIRE_LT(
      std::fabs(frame[flyby::telemetry::distance] - static_cast<float>(std::hypot(20.0, 7.0))),
      oscillate::test::tolerance::coarse, "its distance");
  REQUIRE_LT(frame[flyby::telemetry::itd_us], -500.0F,
             "the left ear leads for a source on the left");
  REQUIRE_LT(frame[flyby::telemetry::level_left_db], 0.0F, "the level is below the reference's");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "flyby contract passed",
      {
          {"at_rest_in_front_is_the_input", [] { at_rest_in_front_is_the_input(); }},
          {"the_latency_is_exact", [] { the_latency_is_exact(); }},
          {"doppler_follows_the_physics", [] { doppler_follows_the_physics(); }},
          {"the_ears_hear_the_head", [] { the_ears_hear_the_head(); }},
          {"the_level_follows_distance", [] { the_level_follows_distance(); }},
          {"the_air_darkens_with_distance", [] { the_air_darkens_with_distance(); }},
          {"the_room_grows_with_distance", [] { the_room_grows_with_distance(); }},
          {"a_loop_wraps_without_a_click", [] { a_loop_wraps_without_a_click(); }},
          {"once_passes_on_each_note", [] { once_passes_on_each_note(); }},
          {"any_block_split_renders_the_same", [] { any_block_split_renders_the_same(); }},
          {"telemetry_feeds_the_panel", [] { telemetry_feeds_the_panel(); }},
      });
}

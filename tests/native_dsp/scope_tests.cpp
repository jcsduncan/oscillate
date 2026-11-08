#include "pocket_support.hpp"

#include "oscillate/native_dsp/effects/scope.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

// Scope (ADR-0042): audio untouched; the alignment meter finds the track's
// delay behind the sidechain to a fraction of a sample (integer, negative
// and half-sample delays), its polarity, and a real kick's offset under
// hats in the low band; the sweep locks to the bar at any tempo and block
// split, and places a free-time impulse in its column.
namespace {
using namespace pocket_test;
namespace scope = oscillate::native_dsp::effects::scope;

const EngineDescriptor& table() { return *scope::engine().descriptor; }

Stereo shifted(const Stereo& x, std::ptrdiff_t frames, double gain = 1.0) {
  Stereo out{std::vector<double>(x.size()), std::vector<double>(x.size())};
  for (std::size_t i = 0; i < x.size(); ++i) {
    const auto j = static_cast<std::ptrdiff_t>(i) - frames;
    if (j >= 0 && j < static_cast<std::ptrdiff_t>(x.size())) {
      out.left[i] = gain * x.left[static_cast<std::size_t>(j)];
      out.right[i] = gain * x.right[static_cast<std::size_t>(j)];
    }
  }
  return out;
}

std::vector<float> run(const std::vector<double>& values, const Stereo& track, const Stereo& key,
                       double bpm = 120.0, std::size_t block = 128) {
  auto runtime = make_effect(scope::engine(), values);
  (void)process(*runtime, track, {.block = block, .sidechain = &key, .bpm = bpm});
  std::vector<float> t;
  REQUIRE_TRUE(runtime->read_telemetry(t), "telemetry");
  REQUIRE_EQ(t.size(), scope::telemetry::size, "telemetry");
  return t;
}

void audio_passes_untouched() {
  const auto track = noise(0.4, 48'000);
  const auto key = noise(0.4, 48'000, false, 99);
  auto runtime = make_effect(scope::engine(), preset(table(), "Kick and Bass"));
  const auto out = process(*runtime, track, {.sidechain = &key});
  for (std::size_t i = 0; i < track.size(); ++i) {
    REQUIRE_EQ(out.left[i], static_cast<double>(static_cast<float>(track.left[i])),
               "Scope never touches the audio");
  }
}

void delays_are_found_to_a_fraction_of_a_sample() {
  auto full = preset(table(), "Full-Band Alignment");
  const auto key = noise(0.4, 2 * second, true, 5);
  for (const std::ptrdiff_t frames : {0, 7, -7, 240}) {
    const auto t = run(full, shifted(key, frames), key);
    const double measured = t[scope::telemetry::delay_ms] * 48.0;
    REQUIRE_LT(std::fabs(measured - static_cast<double>(frames)), 0.05,
               "a delay of " + std::to_string(frames) + " frames reads " +
                   std::to_string(measured));
    require(t[scope::telemetry::polarity] > 0.0F && t[scope::telemetry::correlation] > 0.95F,
            "in phase and alike: polarity " + std::to_string(t[scope::telemetry::polarity]) +
                ", correlation " + std::to_string(t[scope::telemetry::correlation]));
  }
  // Half a sample: the two-point mean is a linear-phase half-sample delay.
  Stereo half{std::vector<double>(key.size()), std::vector<double>(key.size())};
  for (std::size_t i = 1; i < key.size(); ++i) {
    half.left[i] = half.right[i] = 0.5 * (key.left[i] + key.left[i - 1]);
  }
  const auto h = run(full, half, key);
  REQUIRE_LT(std::fabs(h[scope::telemetry::delay_ms] * 48.0F - 0.5F), 0.1F,
             "half a sample reads " + std::to_string(h[scope::telemetry::delay_ms] * 48.0F));
  const auto inverted = run(full, shifted(key, 12, -1.0), key);
  REQUIRE_LT(inverted[scope::telemetry::polarity], 0.0F,
             "an inverted copy reads its delay and opposite polarity");
  REQUIRE_LT(std::fabs(inverted[scope::telemetry::delay_ms] * 48.0F - 12.0F), 0.05F,
             "an inverted copy reads its delay and opposite polarity");
  std::cout << "  delays within 0.05 frames; half a sample reads "
            << h[scope::telemetry::delay_ms] * 48.0F << "\n";
}

void a_real_kick_is_aligned_under_hats() {
  const auto key = kicks("909 House", four_on_the_floor(4), 2 * second);
  // The track: the kick 2 ms late with open hats over it.
  auto track = shifted(key, 96);
  const auto hats = noise(0.3, track.size(), false, 21);
  for (std::size_t i = 0; i < track.size(); ++i) {
    const double gate = (i % 12'000) > 6'000 ? 1.0 : 0.0;
    track.left[i] += gate * hats.left[i];
    track.right[i] += gate * hats.right[i];
  }
  const auto t = run(preset(table(), "Kick and Bass"), track, key);
  REQUIRE_LT(std::fabs(t[scope::telemetry::delay_ms] - 2.0F), 0.05F,
             "the low band finds the kick 2 ms late under hats: " +
                 std::to_string(t[scope::telemetry::delay_ms]) + " ms");
  REQUIRE_GT(t[scope::telemetry::polarity], 0.0F, "and in phase");
}

std::size_t loudest_column(const std::vector<float>& t) {
  std::size_t best = 0;
  for (std::size_t c = 0; c < scope::columns; ++c) {
    if (t[scope::telemetry::track + 2 * c + 1] > t[scope::telemetry::track + 2 * best + 1]) {
      best = c;
    }
  }
  return best;
}

void the_sweep_locks_to_the_bar() {
  auto values = preset(table(), "Bar View");
  for (const double bpm : {100.0, 128.0}) {
    // A click on every downbeat, nothing else.
    const double bar = 4.0 * 60.0 / bpm * rate;
    Stereo clicks{std::vector<double>(4 * second), std::vector<double>(4 * second)};
    // The sweep grid is the test input.
    // NOLINTNEXTLINE(bugprone-float-loop-counter,clang-analyzer-security.FloatLoopCounter)
    for (double at = 0.0; at < static_cast<double>(clicks.size()); at += bar) {
      const auto i = static_cast<std::size_t>(std::lround(at));
      clicks.left[i] = clicks.right[i] = 0.9;
    }
    const Stereo quiet{std::vector<double>(clicks.size()), std::vector<double>(clicks.size())};
    const auto a = run(values, clicks, quiet, bpm, 128);
    const auto b = run(values, clicks, quiet, bpm, 0);
    const auto column = loudest_column(a);
    REQUIRE_LE(column, 1,
               "the downbeat sits at the start of the sweep at " + std::to_string(bpm) +
                   " BPM: column " + std::to_string(column));
    REQUIRE_LT(std::fabs(a[scope::telemetry::window_ms] - static_cast<float>(4.0 * 60'000.0 / bpm)),
               0.5F, "the window is a bar long");
    require(std::equal(a.begin(),
                       a.begin() + static_cast<std::ptrdiff_t>(scope::telemetry::delay_ms),
                       b.begin()),
            "the sweep is the same for any block split");
  }
}

void free_time_places_an_impulse() {
  auto values = preset(table(), "Waveform"); // Free, 20 ms.
  // 30 ms into a 20 ms sweep: half-way across. The render stops a
  // quarter-sweep later, before the next pass overwrites the column.
  Stereo click{std::vector<double>(1'680), std::vector<double>(1'680)};
  click.left[1'440] = click.right[1'440] = 0.9;
  const Stereo quiet{std::vector<double>(1'680), std::vector<double>(1'680)};
  const auto t = run(values, click, quiet);
  const auto column = loudest_column(t);
  require(column >= scope::columns / 2 - 1 && column <= scope::columns / 2 + 1,
          "the impulse is half-way across: column " + std::to_string(column));
  const Stereo silence{std::vector<double>(second), std::vector<double>(second)};
  const auto tone = run(values, sine(440.0, 0.5, second), silence);
  double top = 0.0;
  for (std::size_t c = 0; c < scope::columns; ++c) {
    top = std::max(top, static_cast<double>(tone[scope::telemetry::track + 2 * c + 1]));
  }
  REQUIRE_LT(std::fabs(top - 0.5), 0.005, "the columns carry the waveform's peaks");
}

} // namespace

int main() {
  try {
    audio_passes_untouched();
    delays_are_found_to_a_fraction_of_a_sample();
    a_real_kick_is_aligned_under_hats();
    the_sweep_locks_to_the_bar();
    free_time_places_an_impulse();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "native scope contract passed\n";
  return 0;
}

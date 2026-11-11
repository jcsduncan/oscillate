#include "effect_test_support.hpp"

#include "oscillate/foundation/log.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

// A realistic master chain's cost per callback (ADR-0125): Temper, Ceiling
// with Clip, Gild at 4x, Lens, Keel, Shape and Loudness in series, as a
// master runs them after the parallel tracks (the reference project's
// --master heavy). Ten seconds of a mix (a bass
// line, a chord's partials, kicks and hiss) at 48 kHz, at 256, 512 and 1024
// frames. For each device and the whole chain: the mean and 99th percentile
// milliseconds per block, best of three runs.
namespace {
using namespace effect_test;

constexpr double rate = 48'000.0;
constexpr std::size_t seconds = 10;
constexpr std::size_t runs = 3;
constexpr std::array<std::size_t, 3> blocks{256, 512, 1'024};
constexpr double seconds_per_minute = 60.0;
constexpr double bpm = 124.0;
constexpr double hit_seconds = seconds_per_minute / bpm;
constexpr double hit_hz = 55.0;
constexpr double hit_decay_seconds = 0.12;
constexpr double hit_level = 0.7;
constexpr double bed_level = 0.05;
constexpr std::array partial_hz{55.0, 220.0, 330.0, 440.0, 1'320.0, 2'640.0, 5'280.0};
constexpr std::array partial_level{0.3, 0.1, 0.08, 0.06, 0.04, 0.02, 0.01};
constexpr double p99 = 0.99;
constexpr double beats_per_bar = 4.0;
constexpr double milliseconds = 1'000.0;

struct Device {
  std::string_view engine;
  std::string_view preset;
};
// The user's shape: a resonance suppressor, the limiter with its clipper,
// the exciter, the analyser, tonal balance, an EQ and the loudness meter.
constexpr std::array<Device, 7> chain{{{"temper", "Default"},
                                       {"ceiling", "Loud Club"},
                                       {"gild", "Warm Mids"},
                                       {"lens", "Mix"},
                                       {"keel", "Gentle Polish"},
                                       {"shape", "Presence"},
                                       {"loudness", "Streaming (Spotify, YouTube)"}}};

Stereo mix() {
  auto input = noise(bed_level, seconds * static_cast<std::size_t>(rate));
  const auto period = static_cast<std::size_t>(hit_seconds * rate);
  for (std::size_t i = 0; i < input.size(); ++i) {
    const double t = static_cast<double>(i % period) / rate;
    double x = hit_level * std::exp(-t / hit_decay_seconds) *
               std::sin(2.0 * std::numbers::pi * hit_hz * t);
    const double now = static_cast<double>(i) / rate;
    for (std::size_t p = 0; p < partial_hz.size(); ++p) {
      x += partial_level[p] * std::sin(2.0 * std::numbers::pi * partial_hz[p] * now);
    }
    input.left[i] += x;
    input.right[i] += x;
  }
  return input;
}

double percentile(std::vector<double> times, double at) {
  std::ranges::sort(times);
  return times[static_cast<std::size_t>(at * static_cast<double>(times.size() - 1))];
}

} // namespace

int main() {
  // As the audio callback is: analyses that run off the callback (ADR-0129)
  // are left to their reader and the analysis worker, not drained here.
  const foundation::RealtimeThreadScope realtime;
  const auto input = mix();
  std::printf("%-10s %6s %10s %10s\n", "device", "block", "mean ms", "p99 ms");
  for (const std::size_t block : blocks) {
    std::array<double, chain.size()> best_mean{};
    std::array<double, chain.size()> best_p99{};
    best_mean.fill(1e9);
    best_p99.fill(1e9);
    double best_total = 1e9;
    double best_total_p99 = 1e9;
    for (std::size_t run = 0; run < runs; ++run) {
      std::vector<std::unique_ptr<effects::NativeEffectRuntime>> devices;
      for (const auto& device : chain) {
        const auto* engine = effects::find_engine(device.engine);
        if (engine == nullptr) {
          std::fprintf(stderr, "no engine %s\n", std::string(device.engine).c_str());
          return 1;
        }
        devices.push_back(make_effect(*engine, preset(*engine->descriptor, device.preset), rate));
      }
      std::vector<engine::StereoFrame> buffer(block);
      std::array<std::vector<double>, chain.size()> times;
      std::vector<double> totals;
      for (std::size_t position = 0; position + block <= input.size(); position += block) {
        for (std::size_t i = 0; i < block; ++i) {
          buffer[i] = {static_cast<float>(input.left[position + i]),
                       static_cast<float>(input.right[position + i])};
        }
        const Transport transport{0.0, 0.0, 0.0, beats_per_bar};
        double total = 0.0;
        for (std::size_t d = 0; d < devices.size(); ++d) {
          const auto start = std::chrono::steady_clock::now();
          if (devices[d]->process(buffer, {}, position, 1, transport, {}) !=
              effects::EffectProcessStatus::rendered) {
            std::fprintf(stderr, "a block was rejected\n");
            return 1;
          }
          const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
          times[d].push_back(elapsed.count() * milliseconds);
          total += elapsed.count() * milliseconds;
        }
        totals.push_back(total);
      }
      for (std::size_t d = 0; d < devices.size(); ++d) {
        double sum = 0.0;
        for (const double t : times[d]) {
          sum += t;
        }
        best_mean[d] = std::min(best_mean[d], sum / static_cast<double>(times[d].size()));
        best_p99[d] = std::min(best_p99[d], percentile(times[d], p99));
      }
      double sum = 0.0;
      for (const double t : totals) {
        sum += t;
      }
      best_total = std::min(best_total, sum / static_cast<double>(totals.size()));
      best_total_p99 = std::min(best_total_p99, percentile(totals, p99));
    }
    for (std::size_t d = 0; d < chain.size(); ++d) {
      std::printf("%-10s %6zu %10.3f %10.3f\n", std::string(chain[d].engine).c_str(), block,
                  best_mean[d], best_p99[d]);
    }
    std::printf("%-10s %6zu %10.3f %10.3f\n", "chain", block, best_total, best_total_p99);
  }
}

#include "effect_test_support.hpp"

#include "oscillate/native_dsp/effects/strike.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <numbers>
#include <string>
#include <sys/resource.h>
#include <vector>

// Strike's cost per callback: ten seconds of drum-like hits over noise at
// 48 kHz, through each factory preset at 64 and 128 frames, light (one
// instance, as on a master) and heavy (sixteen in a chain, as on every drum
// track and bus of a dense project). For each: the mean as a share of real time,
// the 99th percentile and the longest block as a share of the block's
// period, and the whole process's CPU time (getrusage) against the audio
// rendered.
namespace {
using namespace effect_test;
namespace strike = oscillate::native_dsp::effects::strike;

constexpr double rate = 48'000.0;
constexpr std::size_t seconds = 10;
// Hits every quarter second: a decaying 120 Hz body over noise.
constexpr double hit_seconds = 0.25;
constexpr double hit_hz = 120.0;
constexpr double hit_decay_seconds = 0.06;
constexpr double hit_level = 0.5;
constexpr double bed_level = 0.05;
constexpr std::array<std::size_t, 2> blocks{64, 128};
constexpr std::array<std::size_t, 2> loads{1, 16};
constexpr double percent = 100.0;
constexpr double p99 = 0.99;
constexpr double beats_per_bar = 4.0;

double process_cpu_seconds() {
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  const auto seconds_of = [](const timeval& t) {
    constexpr double micro = 1e-6;
    return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_usec) * micro;
  };
  return seconds_of(usage.ru_utime) + seconds_of(usage.ru_stime);
}

} // namespace

int main() {
  auto input = noise(bed_level, seconds * static_cast<std::size_t>(rate));
  const auto period = static_cast<std::size_t>(hit_seconds * rate);
  for (std::size_t i = 0; i < input.size(); ++i) {
    const double t = static_cast<double>(i % period) / rate;
    const double x = hit_level * std::exp(-t / hit_decay_seconds) *
                     std::sin(2.0 * std::numbers::pi * hit_hz * t);
    input.left[i] += x;
    input.right[i] += x;
  }
  const auto& table = *strike::engine().descriptor;
  std::printf("%-14s %9s %6s %8s %8s %8s %9s\n", "preset", "instances", "block", "mean", "p99",
              "max", "process");
  for (const auto& entry : table.presets) {
    const auto values = preset(table, entry.name);
    for (const std::size_t instances : loads) {
      for (const std::size_t block : blocks) {
        std::vector<std::unique_ptr<effects::NativeEffectRuntime>> chain;
        for (std::size_t i = 0; i < instances; ++i) {
          chain.push_back(make_effect(strike::engine(), values, rate));
        }
        std::vector<engine::StereoFrame> buffer(block);
        std::vector<double> times;
        const double period = static_cast<double>(block) / rate;
        const double cpu_before = process_cpu_seconds();
        double total = 0.0;
        for (std::size_t position = 0; position + block <= input.size(); position += block) {
          for (std::size_t i = 0; i < block; ++i) {
            buffer[i] = {static_cast<float>(input.left[position + i]),
                         static_cast<float>(input.right[position + i])};
          }
          const Transport transport{0.0, 0.0, 0.0, beats_per_bar};
          const auto start = std::chrono::steady_clock::now();
          for (auto& runtime : chain) {
            if (runtime->process(buffer, {}, position, 1, transport, {}) !=
                effects::EffectProcessStatus::rendered) {
              std::fprintf(stderr, "a block was rejected\n");
              return 1;
            }
          }
          const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
          times.push_back(elapsed.count());
          total += elapsed.count();
        }
        const double cpu = process_cpu_seconds() - cpu_before;
        std::ranges::sort(times);
        const auto at99 = static_cast<std::size_t>(p99 * static_cast<double>(times.size()));
        std::printf("%-14s %9zu %6zu %7.2f%% %7.1f%% %7.1f%% %8.2f%%\n",
                    std::string(entry.name).c_str(), instances, block,
                    total / static_cast<double>(seconds) * percent, times[at99] / period * percent,
                    times.back() / period * percent, cpu / static_cast<double>(seconds) * percent);
      }
    }
  }
}

#include "effect_test_support.hpp"

#include "oscillate/native_dsp/effects/keel.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <sys/resource.h>
#include <vector>

// Keel's cost per callback: ten seconds of noise at 48 kHz, through each
// factory preset and the reference target (a second noise on the
// sidechain) at 64 and 128 frames, light (one instance, as on a master) and
// heavy (sixteen in a chain, as on every bus of a dense project). For each:
// the mean as a share of real time, the 99th percentile and the longest
// block as a share of the block's period, and the whole process's CPU time
// (getrusage) against the audio rendered.
namespace {
using namespace effect_test;
namespace keel = oscillate::native_dsp::effects::keel;

constexpr double rate = 48'000.0;
constexpr std::size_t seconds = 10;
constexpr double level = 0.3;
constexpr std::uint32_t reference_seed = 77;
constexpr double reference_target = 2.0;
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
  const auto frames = seconds * static_cast<std::size_t>(rate);
  const auto input = noise(level, frames);
  const auto reference = noise(level, frames, false, reference_seed);
  const auto& table = *keel::engine().descriptor;
  struct Case {
    std::string name;
    std::vector<double> values;
    bool keyed = false;
  };
  std::vector<Case> cases;
  for (const auto& entry : table.presets) {
    cases.push_back({std::string(entry.name), preset(table, entry.name)});
  }
  auto referenced = preset(table, "Default");
  referenced[index(table, "keel.target")] = reference_target;
  cases.push_back({"Reference", referenced, true});
  std::printf("%-18s %9s %6s %8s %8s %8s %9s\n", "preset", "instances", "block", "mean", "p99",
              "max", "process");
  for (const auto& entry : cases) {
    const auto& values = entry.values;
    for (const std::size_t instances : loads) {
      for (const std::size_t block : blocks) {
        std::vector<std::unique_ptr<effects::NativeEffectRuntime>> chain;
        for (std::size_t i = 0; i < instances; ++i) {
          chain.push_back(make_effect(keel::engine(), values, rate));
        }
        std::vector<engine::StereoFrame> buffer(block);
        std::vector<engine::StereoFrame> side(entry.keyed ? block : 0);
        std::vector<double> times;
        const double period = static_cast<double>(block) / rate;
        const double cpu_before = process_cpu_seconds();
        double total = 0.0;
        for (std::size_t position = 0; position + block <= input.size(); position += block) {
          for (std::size_t i = 0; i < block; ++i) {
            buffer[i] = {static_cast<float>(input.left[position + i]),
                         static_cast<float>(input.right[position + i])};
            if (entry.keyed) {
              side[i] = {static_cast<float>(reference.left[position + i]),
                         static_cast<float>(reference.right[position + i])};
            }
          }
          const Transport transport{0.0, 0.0, 0.0, beats_per_bar};
          const auto start = std::chrono::steady_clock::now();
          for (auto& runtime : chain) {
            if (runtime->process(buffer, side, position, 1, transport, {}) !=
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
        std::printf("%-18s %9zu %6zu %7.2f%% %7.1f%% %7.1f%% %8.2f%%\n", entry.name.c_str(),
                    instances, block, total / static_cast<double>(seconds) * percent,
                    times[at99] / period * percent, times.back() / period * percent,
                    cpu / static_cast<double>(seconds) * percent);
      }
    }
  }
}

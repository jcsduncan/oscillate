#include "effect_test_support.hpp"

#include "oscillate/native_dsp/effects/cast.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <vector>

// Cast's cost per callback: ten seconds of stereo noise at 48 kHz through
// each bundled IR at 64 and 128 frames, with the mean as a share of real
// time, the 99th percentile and the longest block as a share of the block's
// period, and the whole process's CPU time (getrusage) against the audio
// rendered.
namespace {
using namespace effect_test;
namespace cast = oscillate::native_dsp::effects::cast;

constexpr double rate = 48'000.0;
constexpr std::size_t seconds = 10;
constexpr double amplitude = 0.3;
constexpr std::array<std::size_t, 2> blocks{64, 128};
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
  const auto input = noise(amplitude, seconds * static_cast<std::size_t>(rate));
  const auto values = preset(*cast::engine().descriptor, "Default");
  std::printf("%-12s %6s %8s %8s %8s %9s\n", "IR", "block", "mean", "p99", "max", "process");
  for (const auto& ir : cast::builtin_irs()) {
    for (const std::size_t block : blocks) {
      auto created = effects::NativeEffectRuntime::create(cast::engine(), values,
                                                          cast::builtin_document(ir.id), rate);
      if (!created) {
        return 1;
      }
      auto runtime = std::move(created).value();
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
        (void)runtime->process(buffer, {}, position, 1, transport, {});
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
        times.push_back(elapsed.count());
        total += elapsed.count();
      }
      const double cpu = process_cpu_seconds() - cpu_before;
      std::ranges::sort(times);
      const auto at99 = static_cast<std::size_t>(p99 * static_cast<double>(times.size()));
      std::printf("%-12s %6zu %7.2f%% %7.1f%% %7.1f%% %8.2f%%\n", std::string(ir.id).c_str(), block,
                  total / static_cast<double>(seconds) * percent, times[at99] / period * percent,
                  times.back() / period * percent, cpu / static_cast<double>(seconds) * percent);
    }
  }
}

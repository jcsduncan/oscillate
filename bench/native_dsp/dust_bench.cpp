#include "effect_test_support.hpp"

#include "oscillate/native_dsp/effects/dust.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <span>
#include <string>
#include <vector>

// Dust (ADR-0069): each preset's callback cost, as a share of one core, at
// 48 kHz in 64- and 128-frame blocks: ten seconds of quiet programme with a
// kick on the sidechain every half second, the first second excluded, best
// of three. Run in an uninstrumented Release build; timing is not a CTest
// gate. See docs/performance/dust-effect.md.
namespace {
using namespace effect_test;
namespace dust = oscillate::native_dsp::effects::dust;

constexpr double rate = 48'000.0;
constexpr std::size_t seconds = 10;
constexpr std::size_t warm_up = 48'000;
constexpr int runs = 3;
constexpr double kick_level = 0.8;
constexpr double kick_hz = 60.0;
constexpr std::size_t kick_frames = 960;

struct Cost {
  double mean = 0.0;
  double p99 = 0.0;
};

Cost run(const std::vector<double>& values, std::size_t block, const Stereo& input,
         const Stereo& kick) {
  Cost best{1e9, 1e9};
  std::vector<engine::StereoFrame> buffer(block);
  std::vector<engine::StereoFrame> side(block);
  const double block_seconds = static_cast<double>(block) / rate;
  for (int attempt = 0; attempt < runs; ++attempt) {
    auto runtime = make_effect(dust::engine(), values, rate);
    std::vector<double> costs;
    for (std::size_t position = 0; position + block <= input.size(); position += block) {
      for (std::size_t i = 0; i < block; ++i) {
        buffer[i] = {static_cast<float>(input.left[position + i]),
                     static_cast<float>(input.right[position + i])};
        side[i] = {static_cast<float>(kick.left[position + i]),
                   static_cast<float>(kick.right[position + i])};
      }
      const double beats_per_frame = 120.0 / 60.0 / rate;
      const double beat = static_cast<double>(position) * beats_per_frame;
      const Transport transport{beat, beats_per_frame, beat / 4.0, 4.0};
      const auto start = std::chrono::steady_clock::now();
      (void)runtime->process(buffer, side, position, 1, transport, {});
      const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
      if (position >= warm_up) {
        costs.push_back(elapsed.count() / block_seconds);
      }
    }
    double sum = 0.0;
    for (const double c : costs) {
      sum += c;
    }
    std::ranges::sort(costs);
    const Cost cost{sum / static_cast<double>(costs.size()),
                    costs[static_cast<std::size_t>(0.99 * static_cast<double>(costs.size()))]};
    if (cost.mean < best.mean) {
      best = cost;
    }
  }
  return best;
}

} // namespace

int main() {
  const std::size_t frames = seconds * static_cast<std::size_t>(rate);
  const auto input = noise(0.05, frames);
  Stereo kick{std::vector<double>(frames), std::vector<double>(frames)};
  const auto period = static_cast<std::size_t>(rate / 2.0);
  for (std::size_t start = 0; start < frames; start += period) {
    for (std::size_t i = 0; i < kick_frames && start + i < frames; ++i) {
      const double x =
          kick_level * std::sin(2.0 * std::numbers::pi * kick_hz * static_cast<double>(i) / rate);
      kick.left[start + i] = x;
      kick.right[start + i] = x;
    }
  }
  const auto& table = *dust::engine().descriptor;
  std::printf("%-24s %8s %8s %8s %8s\n", "Preset", "64 mean", "64 p99", "128 mean", "128 p99");
  for (const auto& entry : table.presets) {
    const auto values = preset(table, entry.name);
    const Cost small = run(values, 64, input, kick);
    const Cost large = run(values, 128, input, kick);
    std::printf("%-24s %7.2f%% %7.2f%% %7.2f%% %7.2f%%\n", std::string(entry.name).c_str(),
                100.0 * small.mean, 100.0 * small.p99, 100.0 * large.mean, 100.0 * large.p99);
  }
}

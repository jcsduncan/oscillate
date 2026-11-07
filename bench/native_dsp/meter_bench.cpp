#include "oscillate/native_dsp/meter.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <numbers>
#include <numeric>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using namespace oscillate;
using namespace oscillate::native_dsp;

struct Measurement {
  double mean_us = 0.0;
  double p99_us = 0.0;
  double publication_p99_us = 0.0;
  double worst_us = 0.0;
};

Measurement measure(std::size_t frames) {
  StereoMeter meter;
  std::array<engine::StereoFrame, 128> block{};
  for (std::size_t index = 0; index < block.size(); ++index) {
    const auto phase = 2.0 * std::numbers::pi * static_cast<double>(index) / 48.0;
    block[index] = {static_cast<float>(0.5 * std::sin(phase)),
                    static_cast<float>(0.25 * std::sin(phase + 0.3))};
  }
  constexpr std::size_t warm_blocks = 2'000;
  constexpr std::size_t timed_blocks = 20'000;
  for (std::size_t iteration = 0; iteration < warm_blocks; ++iteration) {
    meter.observe(std::span(block).first(frames), {iteration * frames, 1});
  }
  std::vector<double> times;
  std::vector<double> publication_times;
  times.reserve(timed_blocks);
  publication_times.reserve(timed_blocks * frames / StereoMeter::window_frames + 1);
  for (std::size_t iteration = warm_blocks; iteration < warm_blocks + timed_blocks; ++iteration) {
    const auto start = std::chrono::steady_clock::now();
    meter.observe(std::span(block).first(frames), {iteration * frames, 1});
    const auto micros =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    times.push_back(micros);
    if ((iteration + 1) * frames % StereoMeter::window_frames == 0) {
      publication_times.push_back(micros);
    }
  }
  const auto last = meter.latest();
  if (!last || last->left_peak <= 0.0F || !std::isfinite(last->left_rms) ||
      publication_times.empty()) {
    throw std::runtime_error("meter benchmark did not publish a valid window");
  }
  const auto total = std::accumulate(times.begin(), times.end(), 0.0);
  std::ranges::sort(times);
  std::ranges::sort(publication_times);
  return {.mean_us = total / static_cast<double>(timed_blocks),
          .p99_us = times[times.size() * 99 / 100],
          .publication_p99_us = publication_times[publication_times.size() * 99 / 100],
          .worst_us = times.back()};
}
} // namespace

int main() {
  try {
    std::cout << "48 kHz, stereo, 20,000 timed blocks, 2,000 warm blocks\n";
    for (const auto frames : {64U, 128U}) {
      std::array<Measurement, 5> measurements{};
      for (auto& value : measurements) {
        value = measure(frames);
      }
      std::array<double, 5> means{};
      std::array<double, 5> p99s{};
      std::array<double, 5> publication_p99s{};
      double worst = 0.0;
      for (std::size_t index = 0; index < measurements.size(); ++index) {
        means[index] = measurements[index].mean_us;
        p99s[index] = measurements[index].p99_us;
        publication_p99s[index] = measurements[index].publication_p99_us;
        worst = std::max(worst, measurements[index].worst_us);
      }
      std::ranges::sort(means);
      std::ranges::sort(p99s);
      std::ranges::sort(publication_p99s);
      std::cout << "frames=" << frames << " median_mean_us=" << means[2]
                << " median_p99_us=" << p99s[2]
                << " median_publication_p99_us=" << publication_p99s[2] << " worst_us=" << worst
                << '\n';
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

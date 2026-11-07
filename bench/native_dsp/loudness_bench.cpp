#include "oscillate/native_dsp/loudness.hpp"

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
using oscillate::native_dsp::IntegratedLoudnessAnalyzer;
using oscillate::native_dsp::LoudnessLayout;
using oscillate::native_dsp::LoudnessStatus;

struct Measurement {
  double mean_us = 0.0;
  double p99_us = 0.0;
};

Measurement measure(std::size_t frames) {
  IntegratedLoudnessAnalyzer analyzer(48'000, LoudnessLayout::stereo);
  std::array<float, 256> block{};
  for (std::size_t frame = 0; frame < 128; ++frame) {
    const auto signal = static_cast<float>(
        0.5 * std::sin(2.0 * std::numbers::pi * 997.0 * static_cast<double>(frame) / 48'000.0));
    block[frame * 2] = signal;
    block[frame * 2 + 1] = signal;
  }
  constexpr std::size_t timed_blocks = 20'000;
  std::vector<double> times;
  times.reserve(timed_blocks);
  for (std::size_t iteration = 0; iteration < timed_blocks; ++iteration) {
    const auto start = std::chrono::steady_clock::now();
    if (analyzer.append(std::span(block).first(frames * 2)) != LoudnessStatus::ok) {
      throw std::runtime_error("loudness benchmark rejected its bounded input");
    }
    times.push_back(
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
            .count());
  }
  if (!analyzer.finish().integrated_lkfs) {
    throw std::runtime_error("loudness benchmark produced no measurement");
  }
  const double mean =
      std::accumulate(times.begin(), times.end(), 0.0) / static_cast<double>(times.size());
  std::ranges::sort(times);
  return {.mean_us = mean, .p99_us = times[times.size() * 99 / 100]};
}
} // namespace

int main() {
  try {
    std::cout << "48 kHz stereo, 20,000 measured append calls per run\n";
    for (const auto frames : {64U, 128U}) {
      std::array<double, 5> means{};
      std::array<double, 5> p99s{};
      for (std::size_t run = 0; run < means.size(); ++run) {
        const auto measurement = measure(frames);
        means[run] = measurement.mean_us;
        p99s[run] = measurement.p99_us;
      }
      std::ranges::sort(means);
      std::ranges::sort(p99s);
      std::cout << "frames=" << frames << " median_mean_us=" << means[2]
                << " median_p99_us=" << p99s[2] << '\n';
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

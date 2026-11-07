#include "oscillate/native_dsp/eq.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <numbers>
#include <span>
#include <stdexcept>

namespace {
using namespace oscillate;
using namespace oscillate::native_dsp;

constexpr std::size_t timed_blocks = 20'000;

double measure(std::size_t frames, std::span<const EqBandSettings> settings) {
  auto prepared = prepare_eq(settings, 48'000);
  if (!prepared) {
    throw std::runtime_error(prepared.error().message);
  }
  EqRuntime filter(std::move(prepared).value());
  std::array<engine::StereoFrame, 128> block{};
  std::array<engine::StereoFrame, 128> source{};
  for (std::size_t i = 0; i < source.size(); ++i) {
    const float sample =
        static_cast<float>(0.05 * std::sin(2.0 * std::numbers::pi * static_cast<double>(i) / 48.0));
    source[i] = {sample, -sample};
  }
  double checksum = 0.0;
  // Prime state before timing and keep signal finite at the worst filter gain.
  for (std::size_t warmup = 0; warmup < 2'000; ++warmup) {
    std::copy_n(source.begin(), frames, block.begin());
    filter.process(std::span(block).first(frames), {warmup * frames, 1});
  }
  const auto begin = std::chrono::steady_clock::now();
  for (std::size_t iteration = 0; iteration < timed_blocks; ++iteration) {
    std::copy_n(source.begin(), frames, block.begin());
    filter.process(std::span(block).first(frames), {iteration * frames, 1});
    checksum += block[0].left;
  }
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin);
  if (!std::isfinite(checksum) || filter.guarded_frames_after_quiesce() != 0) {
    throw std::runtime_error("EQ output became invalid during benchmark");
  }
  return elapsed.count() * 1'000'000.0 / static_cast<double>(timed_blocks);
}

} // namespace

int main() {
  try {
    const std::array settings{
        EqBandSettings{EqBandKind::high_pass, 40.0, 0.707, 0.0},
        EqBandSettings{EqBandKind::low_shelf, 120.0, 1.0, 3.0},
        EqBandSettings{EqBandKind::peaking, 250.0, 1.2, -2.0},
        EqBandSettings{EqBandKind::peaking, 500.0, 1.0, 1.5},
        EqBandSettings{EqBandKind::peaking, 1'200.0, 2.0, -3.0},
        EqBandSettings{EqBandKind::peaking, 3'000.0, 1.0, 2.0},
        EqBandSettings{EqBandKind::high_shelf, 9'000.0, 1.0, 2.5},
        EqBandSettings{EqBandKind::low_pass, 18'000.0, 0.707, 0.0},
    };
    std::cout << "48 kHz, stereo, 20,000 timed blocks, warm state, 1 kHz sine\n";
    for (std::size_t frames : {64U, 128U}) {
      for (std::size_t bands : {1U, 4U, 8U}) {
        std::array<double, 5> samples{};
        for (double& sample : samples) {
          sample = measure(frames, std::span(settings).first(bands));
        }
        std::ranges::sort(samples);
        std::cout << "frames=" << frames << " bands=" << bands
                  << " median_us_per_block=" << samples[2] << '\n';
      }
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

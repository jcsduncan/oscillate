#include "oscillate/native_dsp/compressor.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <numbers>
#include <span>
#include <stdexcept>
#include <utility>

namespace {
using namespace oscillate;
using namespace oscillate::native_dsp;

double measure(std::size_t frames, bool external_sidechain) {
  auto prepared = prepare_compressor({-18.0, 4.0, 6.0, 10.0, 100.0, 0.0}, 48'000);
  if (!prepared) {
    throw std::runtime_error(prepared.error().message);
  }
  CompressorRuntime compressor(std::move(prepared).value());
  std::array<engine::StereoFrame, 128> source{};
  std::array<engine::StereoFrame, 128> sidechain{};
  std::array<engine::StereoFrame, 128> block{};
  for (std::size_t index = 0; index < source.size(); ++index) {
    const auto phase = 2.0 * std::numbers::pi * static_cast<double>(index) / 48.0;
    source[index] = {static_cast<float>(0.5 * std::sin(phase)),
                     static_cast<float>(0.35 * std::sin(phase + 0.3))};
    sidechain[index] = {static_cast<float>(0.8 * std::sin(phase)), 0.0F};
  }
  auto run = [&](std::size_t iteration) {
    std::ranges::copy(source, block.begin());
    const engine::BlockContext context{iteration * frames, 1};
    if (external_sidechain) {
      compressor.process_with_sidechain(std::span(block).first(frames),
                                        std::span(sidechain).first(frames), context);
    } else {
      compressor.process(std::span(block).first(frames), context);
    }
  };
  for (std::size_t iteration = 0; iteration < 2'000; ++iteration) {
    run(iteration);
  }
  double checksum = 0.0;
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t iteration = 0; iteration < 20'000; ++iteration) {
    run(iteration);
    checksum += block[0].left;
  }
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
  if (!std::isfinite(checksum) || compressor.guarded_frames_after_quiesce() != 0) {
    throw std::runtime_error("invalid compressor output during benchmark");
  }
  return elapsed.count() * 1'000'000.0 / 20'000.0;
}

} // namespace

int main() {
  try {
    std::cout << "48 kHz, stereo, 20,000 timed blocks, 2,000 warm blocks\n";
    for (const auto frames : {64U, 128U}) {
      for (const bool sidechain : {false, true}) {
        std::array<double, 5> measurements{};
        for (auto& measurement : measurements) {
          measurement = measure(frames, sidechain);
        }
        std::ranges::sort(measurements);
        std::cout << "frames=" << frames << " external_sidechain=" << sidechain
                  << " median_us_per_block=" << measurements[2] << '\n';
      }
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

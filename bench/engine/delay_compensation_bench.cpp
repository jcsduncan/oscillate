#include "oscillate/engine/delay_compensation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <numeric>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using namespace oscillate::engine;
constexpr std::size_t routes = 32;
constexpr std::size_t warmup = 2'000;
constexpr std::size_t measured = 10'000;

void run(std::uint32_t budget, std::size_t frames) {
  std::vector<std::unique_ptr<DelayCompensationRuntime>> lines;
  lines.reserve(routes);
  const auto history_bytes = (static_cast<std::uint64_t>(budget) + 1) * sizeof(StereoFrame);
  for (std::size_t route = 0; route < routes; ++route) {
    const auto delay = route % 2 == 0 ? budget : budget / 2;
    auto prepared = DelayCompensationRuntime::prepare({route + 1, delay, budget, history_bytes});
    if (!prepared)
      throw std::runtime_error(prepared.error().message);
    lines.push_back(std::move(prepared).value());
  }
  std::array<StereoFrame, 128> block{};
  std::vector<double> samples;
  samples.reserve(measured);
  double checksum = 0.0;
  for (std::size_t iteration = 0; iteration < warmup + measured; ++iteration) {
    const auto started = std::chrono::steady_clock::now();
    for (auto& line : lines) {
      std::fill_n(block.begin(), frames, StereoFrame{0.125F, -0.25F});
      line->process(std::span(block).first(frames), {iteration * frames, 1});
      checksum += block[frames - 1].left;
    }
    const auto elapsed =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started)
            .count();
    if (iteration >= warmup)
      samples.push_back(elapsed);
  }
  std::ranges::sort(samples);
  const auto sum = std::accumulate(samples.begin(), samples.end(), 0.0);
  std::cout << "budget_frames=" << budget << " block_frames=" << frames << " routes=" << routes
            << " history_bytes=" << routes * history_bytes << " mean_us=" << sum / measured
            << " p99_us=" << samples[measured * 99 / 100 - 1] << " worst_us=" << samples.back()
            << " period_us=" << static_cast<double>(frames) * 1'000'000.0 / 48'000.0
            << " checksum=" << checksum << '\n';
}
} // namespace

int main() {
  try {
    for (const std::uint32_t budget : {64U, 512U, 2'048U, 8'192U}) {
      for (const std::size_t frames : {64U, 128U})
        run(budget, frames);
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}

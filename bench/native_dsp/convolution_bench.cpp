#include "oscillate/native_dsp/convolution.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using oscillate::engine::StereoFrame;
using oscillate::native_dsp::Convolution;

struct BenchCase {
  std::uint32_t rate;
  std::size_t instances;
  std::size_t callback_frames;
};

void run(BenchCase selected) {
  std::vector<StereoFrame> ir(2ULL * selected.rate);
  for (std::size_t index = 0; index < ir.size(); ++index) {
    const float envelope =
        std::exp(-5.0F * static_cast<float>(index) / static_cast<float>(2 * selected.rate));
    ir[index] = {envelope * std::sin(0.139F * static_cast<float>(index)) * 0.004F,
                 envelope * std::cos(0.187F * static_cast<float>(index)) * 0.004F};
  }
  std::vector<std::unique_ptr<Convolution>> processors;
  processors.reserve(selected.instances);
  for (std::size_t instance = 0; instance < selected.instances; ++instance) {
    auto prepared = Convolution::create(ir, selected.rate, 0.5F);
    if (!prepared) {
      std::cerr << prepared.error().message << '\n';
      std::exit(1);
    }
    processors.push_back(std::move(prepared).value());
  }
  std::vector<std::vector<StereoFrame>> blocks(selected.instances,
                                               std::vector<StereoFrame>(selected.callback_frames));
  std::vector<double> wall_microseconds;
  std::vector<double> cpu_microseconds;
  const std::size_t warm_blocks = (3ULL * selected.rate) / selected.callback_frames;
  const std::size_t measure_blocks = (2ULL * selected.rate) / selected.callback_frames;
  wall_microseconds.reserve(measure_blocks);
  cpu_microseconds.reserve(measure_blocks);
  std::uint32_t rng = 0x56a8d310U;
  const auto next = [&rng] {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return static_cast<float>(static_cast<int>(rng & 0xffffU) - 32768) / 65536.0F;
  };
  // Three seconds bring every IR partition and input-history slot into use.
  for (std::size_t iteration = 0; iteration < warm_blocks + measure_blocks; ++iteration) {
    for (auto& block : blocks) {
      for (auto& frame : block) {
        frame = {next(), next()};
      }
    }
    const auto start = std::chrono::steady_clock::now();
    timespec cpu_start{};
    timespec cpu_end{};
    (void)clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_start);
    for (std::size_t instance = 0; instance < selected.instances; ++instance) {
      processors[instance]->process(blocks[instance], 1);
    }
    (void)clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_end);
    const auto end = std::chrono::steady_clock::now();
    if (iteration >= warm_blocks) {
      wall_microseconds.push_back(std::chrono::duration<double, std::micro>(end - start).count());
      cpu_microseconds.push_back(static_cast<double>(cpu_end.tv_sec - cpu_start.tv_sec) * 1e6 +
                                 static_cast<double>(cpu_end.tv_nsec - cpu_start.tv_nsec) / 1e3);
    }
  }
  std::ranges::sort(wall_microseconds);
  std::ranges::sort(cpu_microseconds);
  const auto median_wall = wall_microseconds[wall_microseconds.size() / 2];
  const auto p99_wall = wall_microseconds[(wall_microseconds.size() * 99) / 100];
  const auto worst_wall = wall_microseconds.back();
  const auto median_cpu = cpu_microseconds[cpu_microseconds.size() / 2];
  const auto p99_cpu = cpu_microseconds[(cpu_microseconds.size() * 99) / 100];
  const auto worst_cpu = cpu_microseconds.back();
  const double period = static_cast<double>(selected.callback_frames) / selected.rate * 1e6;
  const auto over_budget =
      std::ranges::count_if(wall_microseconds, [period](double value) { return value > period; });
  std::cout << selected.rate << ',' << selected.instances << ',' << selected.callback_frames << ','
            << std::fixed << std::setprecision(2) << period << ',' << median_wall << ',' << p99_wall
            << ',' << worst_wall << ',' << median_cpu << ',' << p99_cpu << ',' << worst_cpu << ','
            << period - worst_wall << ',' << over_budget << '\n';
}
} // namespace

int main() {
  try {
    std::cout << "rate,instances,frames,period_us,wall_median_us,wall_p99_us,wall_worst_us,"
                 "cpu_median_us,cpu_p99_us,cpu_worst_us,worst_headroom_us,over_budget_blocks\n";
    for (const auto rate : {44'100U, 48'000U, 88'200U, 96'000U}) {
      for (const auto instances : {1U, 4U, 8U}) {
        for (const auto frames : {64U, 128U}) {
          run({rate, instances, frames});
        }
      }
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

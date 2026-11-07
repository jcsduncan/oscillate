#include "oscillate/native_dsp/convolution_runtime.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <span>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace {
using oscillate::engine::StereoFrame;
using oscillate::native_dsp::Convolution;
using oscillate::native_dsp::ConvolutionRuntime;

[[nodiscard]] std::size_t resident_bytes() {
  std::ifstream statm("/proc/self/statm");
  std::size_t size = 0;
  std::size_t resident = 0;
  statm >> size >> resident;
  return resident * static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
}

[[nodiscard]] double thread_microseconds() noexcept {
  timespec now{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return static_cast<double>(now.tv_sec) * 1'000'000.0 + static_cast<double>(now.tv_nsec) / 1'000.0;
}

void print_samples(std::string_view path, std::string_view phase, std::size_t instances,
                   std::size_t frames, std::size_t ir_frames, std::size_t resident_delta,
                   std::vector<double> samples) {
  if (samples.empty()) {
    return;
  }
  std::ranges::sort(samples);
  const auto total = std::accumulate(samples.begin(), samples.end(), 0.0);
  const auto period_us = static_cast<double>(frames) * 1'000'000.0 / 48'000.0;
  const auto over_period =
      std::ranges::count_if(samples, [period_us](double sample) { return sample > period_us; });
  std::cout << path << ',' << phase << ',' << instances << ',' << frames << ',' << ir_frames << ','
            << samples.size() << ',' << total << ',' << samples[samples.size() / 2] << ','
            << samples[samples.size() * 99 / 100] << ',' << samples.back() << ',' << resident_delta
            << ',' << over_period << '\n';
}

void run(std::size_t frames, std::size_t instances, bool adapter, std::size_t ir_frames) {
  constexpr std::uint32_t rate = 48'000;
  std::vector<StereoFrame> ir(ir_frames);
  for (std::size_t index = 0; index < ir.size(); ++index) {
    const float time = static_cast<float>(index);
    const float envelope = std::exp(-5.0F * time / static_cast<float>(ir.size()));
    ir[index] = {0.004F * envelope * std::sin(0.139F * time),
                 0.004F * envelope * std::cos(0.187F * time)};
  }
  const auto before = resident_bytes();
  std::vector<std::unique_ptr<Convolution>> cores;
  std::vector<std::unique_ptr<ConvolutionRuntime>> runtimes;
  for (std::size_t index = 0; index < instances; ++index) {
    auto prepared = Convolution::create(ir, rate, 0.5F);
    if (!prepared) {
      std::cerr << prepared.error().message << '\n';
      return;
    }
    if (adapter) {
      runtimes.push_back(std::make_unique<ConvolutionRuntime>(std::move(prepared).value()));
    } else {
      cores.push_back(std::move(prepared).value());
    }
  }
  const auto resident_delta = resident_bytes() - before;
  std::vector<StereoFrame> block(frames, {0.01F, -0.01F});
  const std::size_t callbacks = rate / frames;
  std::vector<double> boundary_samples;
  std::vector<double> other_samples;
  boundary_samples.reserve(callbacks / (1024 / frames) + 1);
  other_samples.reserve(callbacks);
  for (std::size_t pass = 0; pass < 3; ++pass) {
    for (std::size_t callback = 0; callback < callbacks; ++callback) {
      std::ranges::fill(block, StereoFrame{0.01F, -0.01F});
      const auto start = thread_microseconds();
      if (adapter) {
        for (auto& runtime : runtimes) {
          runtime->process(block, {callback * frames, 1});
        }
      } else {
        for (auto& core : cores) {
          core->process(block, 1);
        }
      }
      const auto end = thread_microseconds();
      if (pass == 2) {
        const bool long_tier_boundary = ((pass * callbacks + callback) * frames) % 1024 == 0;
        (long_tier_boundary ? boundary_samples : other_samples).push_back(end - start);
      }
    }
  }
  std::vector<double> all = boundary_samples;
  all.insert(all.end(), other_samples.begin(), other_samples.end());
  const auto path = adapter ? "adapter" : "core";
  print_samples(path, "all", instances, frames, ir_frames, resident_delta, std::move(all));
  print_samples(path, "1024_boundary", instances, frames, ir_frames, resident_delta,
                std::move(boundary_samples));
  print_samples(path, "other", instances, frames, ir_frames, resident_delta,
                std::move(other_samples));
}
} // namespace

int main(int argc, char** argv) {
  if (argc != 4 && argc != 5) {
    std::cerr << "usage: convolution_runtime_bench core|adapter 1|4 64|128 [ir_frames]\n";
    return 2;
  }
  const auto mode = std::string_view(argv[1]);
  std::size_t instances = 0;
  std::size_t frames = 0;
  std::size_t ir_frames = 96'000;
  const auto instance_text = std::string_view(argv[2]);
  const auto frame_text = std::string_view(argv[3]);
  const auto parsed_instances =
      std::from_chars(instance_text.data(), instance_text.data() + instance_text.size(), instances);
  const auto parsed_frames =
      std::from_chars(frame_text.data(), frame_text.data() + frame_text.size(), frames);
  std::from_chars_result parsed_ir{nullptr, std::errc{}};
  std::string_view ir_text;
  if (argc == 5) {
    ir_text = argv[4];
    parsed_ir = std::from_chars(ir_text.data(), ir_text.data() + ir_text.size(), ir_frames);
  }
  if ((mode != "core" && mode != "adapter") || parsed_instances.ec != std::errc{} ||
      parsed_instances.ptr != instance_text.data() + instance_text.size() ||
      parsed_frames.ec != std::errc{} ||
      parsed_frames.ptr != frame_text.data() + frame_text.size() ||
      (instances != 1 && instances != 4) || (frames != 64 && frames != 128) || ir_frames == 0 ||
      ir_frames > 96'000 ||
      (argc == 5 &&
       (parsed_ir.ec != std::errc{} || parsed_ir.ptr != ir_text.data() + ir_text.size()))) {
    return 2;
  }
  std::cout << "path,phase,instances,frames,ir_frames,callbacks,total_us,median_us,p99_us,worst_us,"
               "resident_delta_bytes,over_period_callbacks\n";
  run(frames, instances, mode == "adapter", ir_frames);
}

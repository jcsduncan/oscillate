#include "oscillate/warp_dsp/rate_view.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {
std::atomic<bool> counting{false};
std::atomic<std::size_t> allocations{0};
} // namespace

void* operator new(std::size_t size) {
  if (counting.load(std::memory_order_relaxed))
    allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* result = std::malloc(size == 0 ? 1 : size))
    return result;
  throw std::bad_alloc();
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }

namespace {
namespace warp = oscillate::warp_dsp;

template <std::size_t Frames> void run_pair(std::uint32_t source_rate, std::uint32_t project_rate) {
  // Seven seconds of deterministic stereo transients and harmonic material.
  std::vector<float> source_left(source_rate * 7U), source_right(source_rate * 7U);
  for (std::size_t frame = 0; frame < source_left.size(); ++frame) {
    const auto time = static_cast<double>(frame) / source_rate;
    const auto pulse = std::exp(-50.0 * std::fmod(time, 0.5));
    source_left[frame] =
        static_cast<float>(0.18 * std::sin(2.0 * std::numbers::pi * 110.0 * time) +
                           0.12 * std::sin(2.0 * std::numbers::pi * 880.0 * time) + 0.35 * pulse);
    source_right[frame] =
        static_cast<float>(0.19 * std::sin(2.0 * std::numbers::pi * 220.0 * time) +
                           0.11 * std::sin(2.0 * std::numbers::pi * 1'760.0 * time) - 0.25 * pulse);
  }
  warp::MemorySourceView memory(source_left, source_right);
  auto opened = warp::RateConvertedSourceView::create(memory, source_rate, project_rate);
  if (!opened)
    throw std::runtime_error(opened.error().message);
  auto view = std::move(opened).value();
  std::array<float, Frames> left{}, right{};
  constexpr std::size_t iterations = 20'000;
  const auto available = view->frames() - Frames - 1U;
  double checksum = 0.0;
  for (std::size_t i = 0; i < 256; ++i) {
    const auto first = static_cast<std::int64_t>((i * Frames) % available);
    if (!view->read(first, left, right))
      throw std::runtime_error("warm-up read failed");
  }
  allocations = 0;
  counting = true;
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < iterations; ++i) {
    const auto first = static_cast<std::int64_t>((i * Frames) % available);
    if (!view->read(first, left, right))
      throw std::runtime_error("timed read failed");
    checksum += left[i % Frames] + right[(i * 7U) % Frames];
  }
  const auto end = std::chrono::steady_clock::now();
  counting = false;
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  std::printf("%u->%u %zu frames: %.3f us/block, allocations=%zu, checksum=%.6f\n", source_rate,
              project_rate, Frames, static_cast<double>(nanos) / iterations / 1'000.0,
              allocations.load(), checksum);
}
} // namespace

int main() {
  try {
    run_pair<64>(44'100, 48'000);
    run_pair<128>(44'100, 48'000);
    run_pair<64>(96'000, 48'000);
    run_pair<128>(96'000, 48'000);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}

#include "dsp/offline_fft.hpp"
#include "oscillate/warp_dsp/real_fft.hpp"

#include <algorithm>
#include <chrono>
#include <complex>
#include <cstddef>
#include <cstdio>
#include <vector>

// The transforms per size (ADR-0126): the offline double complex transform
// on a real signal (as the effects ran it before), against the realtime
// float real transform, ordered and unordered. Each forward then inverse,
// microseconds per pair, best of several runs.
namespace {

using oscillate::native_dsp::dsp::OfflineFft;
using oscillate::warp_dsp::AlignedBuffer;
using oscillate::warp_dsp::RealFft;

constexpr std::size_t smallest = 256;
constexpr std::size_t largest = 65'536;
constexpr std::size_t runs = 5;
// Transform pairs per run: about the same work at every size.
constexpr std::size_t points_per_run = std::size_t{1} << 23U;
constexpr double microseconds = 1e6;
// A deterministic signal: a linear congruential generator's top bits.
constexpr std::uint32_t lcg_multiplier = 1'664'525U;
constexpr std::uint32_t lcg_increment = 1'013'904'223U;
constexpr float lcg_scale = 1.0F / 4'294'967'296.0F;

template <typename Pair> double best_microseconds(std::size_t pairs, Pair&& pair) {
  double best = 1e30;
  for (std::size_t run = 0; run < runs; ++run) {
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < pairs; ++i) {
      pair();
    }
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    best = std::min(best, elapsed.count() * microseconds / static_cast<double>(pairs));
  }
  return best;
}

} // namespace

int main() {
  std::printf("%8s %14s %14s %16s %10s\n", "size", "offline us", "realtime us", "unordered us",
              "speedup");
  for (std::size_t size = smallest; size <= largest; size *= 2) {
    std::vector<float> signal(size);
    std::uint32_t state = 1;
    for (float& value : signal) {
      state = state * lcg_multiplier + lcg_increment;
      value = static_cast<float>(state) * lcg_scale - 0.5F;
    }
    const std::size_t pairs = std::max<std::size_t>(1, points_per_run / size);

    const OfflineFft offline(size);
    std::vector<std::complex<double>> data(size);
    const double offline_us = best_microseconds(pairs, [&] {
      std::ranges::copy(signal, data.begin());
      offline.forward(data);
      offline.inverse(data);
    });

    RealFft realtime(size);
    AlignedBuffer buffer(size);
    const double ordered_us = best_microseconds(pairs, [&] {
      std::ranges::copy(signal, buffer.data());
      realtime.forward(buffer.data(), buffer.data());
      realtime.inverse(buffer.data(), buffer.data());
    });
    const double unordered_us = best_microseconds(pairs, [&] {
      std::ranges::copy(signal, buffer.data());
      realtime.forward_unordered(buffer.data(), buffer.data());
      realtime.inverse_unordered(buffer.data(), buffer.data());
    });
    std::printf("%8zu %14.3f %14.3f %16.3f %9.1fx\n", size, offline_us, ordered_us, unordered_us,
                offline_us / ordered_us);
  }
  return 0;
}

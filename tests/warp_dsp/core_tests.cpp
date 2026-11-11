#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"
#include "oscillate/warp_dsp/real_fft.hpp"
#include "oscillate/warp_dsp/source_view.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

namespace {

using oscillate::test::require;

void fft_sizes_are_validated() {
  for (const std::size_t size : {32U, 96U, 160U, 480U, 512U, 4096U, 8192U})
    require(oscillate::warp_dsp::RealFft::supported(size), "valid FFT size rejected");
  for (const std::size_t size : {0U, 16U, 100U, 224U, 1000U})
    require(!oscillate::warp_dsp::RealFft::supported(size), "invalid FFT size accepted");
  bool refused = false;
  try {
    oscillate::warp_dsp::RealFft fft(std::numeric_limits<std::size_t>::max());
  } catch (const std::invalid_argument&) {
    refused = true;
  }
  require(refused, "oversized FFT was accepted");
}

void source_view_handles_extreme_positions() {
  const std::vector<float> left{1.0F, 2.0F};
  const std::vector<float> right{3.0F, 4.0F};
  oscillate::warp_dsp::MemorySourceView view(left, right);
  float output_left[3]{};
  float output_right[3]{};
  require(view.read(std::numeric_limits<std::int64_t>::max(), output_left, output_right),
          "large positive source read failed");
  REQUIRE_EQ(output_left[0], 0.0F, "large positive source read was not silent");
  REQUIRE_EQ(output_left[1], 0.0F, "large positive source read was not silent");
  REQUIRE_EQ(output_left[2], 0.0F, "large positive source read was not silent");
  require(view.read(std::numeric_limits<std::int64_t>::min(), output_left, output_right),
          "large negative source read failed");
  REQUIRE_EQ(output_left[0], 0.0F, "large negative source read was not silent");
  REQUIRE_EQ(output_left[1], 0.0F, "large negative source read was not silent");
  REQUIRE_EQ(output_left[2], 0.0F, "large negative source read was not silent");
  require(view.read(-1, output_left, output_right), "negative source read failed");
  REQUIRE_EQ(output_left[0], 0.0F, "negative source read did not align channels");
  REQUIRE_EQ(output_left[1], 1.0F, "negative source read did not align channels");
  REQUIRE_EQ(output_left[2], 2.0F, "negative source read did not align channels");
  REQUIRE_EQ(output_right[0], 0.0F, "negative source read did not align channels");
  REQUIRE_EQ(output_right[1], 3.0F, "negative source read did not align channels");
  REQUIRE_EQ(output_right[2], 4.0F, "negative source read did not align channels");
}

// Naive double-precision DFT oracle in the packed spectrum order.
std::vector<double> reference_dft(const std::vector<float>& input) {
  const auto size = input.size();
  std::vector<double> spectrum(size, 0.0);
  for (std::size_t bin = 0; bin <= size / 2; ++bin) {
    double real = 0.0;
    double imaginary = 0.0;
    for (std::size_t index = 0; index < size; ++index) {
      const double angle = -2.0 * std::numbers::pi * static_cast<double>(bin * index % size) /
                           static_cast<double>(size);
      real += input[index] * std::cos(angle);
      imaginary += input[index] * std::sin(angle);
    }
    if (bin == 0)
      spectrum[0] = real;
    else if (bin == size / 2)
      spectrum[1] = real;
    else {
      spectrum[2 * bin] = real;
      spectrum[2 * bin + 1] = imaginary;
    }
  }
  return spectrum;
}

void fft_matches_dft_and_round_trips() {
  for (const std::size_t size : {32U, 96U, 160U, 512U, 1024U, 4096U}) {
    oscillate::warp_dsp::RealFft fft(size);
    oscillate::warp_dsp::AlignedBuffer input(size);
    oscillate::warp_dsp::AlignedBuffer spectrum(size);
    oscillate::warp_dsp::AlignedBuffer output(size);
    std::vector<float> samples(size);
    std::uint32_t state = static_cast<std::uint32_t>(size);
    for (std::size_t index = 0; index < size; ++index) {
      state = state * 1664525U + 1013904223U;
      samples[index] = static_cast<float>(state >> 8) / 16777216.0F - 0.5F;
      input[index] = samples[index];
    }
    fft.forward(input.data(), spectrum.data());
    const auto expected = reference_dft(samples);
    double worst = 0.0;
    for (std::size_t index = 0; index < size; ++index)
      worst = std::max(worst, std::fabs(spectrum[index] - expected[index]));
    REQUIRE_LT(worst, 2e-6 * static_cast<double>(size), "FFT differs from DFT oracle");
    fft.inverse(spectrum.data(), output.data());
    for (std::size_t index = 0; index < size; ++index)
      REQUIRE_LT(std::fabs(output[index] / static_cast<float>(size) - samples[index]),
                 tolerance::float_processing_f, "inverse FFT does not round-trip");
  }
}

// A random signal in [-0.5, 0.5) from a linear congruential generator.
std::vector<float> random_signal(std::size_t size, std::uint32_t seed) {
  constexpr std::uint32_t multiplier = 1664525U;
  constexpr std::uint32_t increment = 1013904223U;
  constexpr float top_bits_scale = 1.0F / 16777216.0F;
  constexpr unsigned dropped_bits = 8;
  std::vector<float> samples(size);
  for (float& sample : samples) {
    seed = seed * multiplier + increment;
    sample = static_cast<float>(seed >> dropped_bits) * top_bits_scale - 0.5F;
  }
  return samples;
}

// The unordered spectra's products are the circular convolution: against a
// direct double sum, with multiply() and multiply_accumulate() (which adds
// onto what is there) and the scale applied.
void unordered_products_convolve() {
  // Float rounding through a forward, a product and an inverse of up to
  // 2048 points, relative to the signals' sizes (about 20 ulps of a sum of
  // hundreds of products).
  constexpr double convolution_tolerance = 2e-5;
  constexpr float half = 0.5F;
  for (const std::size_t size : {32U, 96U, 512U, 2048U}) {
    const auto a = random_signal(size, static_cast<std::uint32_t>(size));
    const auto b = random_signal(size, static_cast<std::uint32_t>(size) + 1U);
    std::vector<double> expected(size, 0.0);
    for (std::size_t n = 0; n < size; ++n)
      for (std::size_t m = 0; m < size; ++m)
        expected[(n + m) % size] += static_cast<double>(a[n]) * b[m];
    oscillate::warp_dsp::RealFft fft(size);
    oscillate::warp_dsp::AlignedBuffer x(size);
    oscillate::warp_dsp::AlignedBuffer y(size);
    oscillate::warp_dsp::AlignedBuffer product(size);
    std::ranges::copy(a, x.data());
    std::ranges::copy(b, y.data());
    fft.forward_unordered(x.data(), x.data());
    fft.forward_unordered(y.data(), y.data());
    const float unscale = 1.0F / static_cast<float>(size);
    // Half the product twice over: multiply, then accumulate the same.
    fft.multiply(x.data(), y.data(), product.data(), half * unscale);
    fft.multiply_accumulate(x.data(), y.data(), product.data(), half * unscale);
    fft.inverse_unordered(product.data(), product.data());
    double scale = 0.0;
    for (const double value : expected)
      scale = std::max(scale, std::fabs(value));
    for (std::size_t n = 0; n < size; ++n)
      REQUIRE_LT(std::fabs(product[n] - expected[n]), convolution_tolerance * scale,
                 "the product's inverse is the circular convolution at " + std::to_string(size));
  }
}

// The staged transform is the whole one, to float rounding, at every
// supported size, and its steps all run; sizes it cannot split are refused.
void staged_transform_matches_the_whole_one() {
  // Two float transforms of up to 2^16 points computed in different orders:
  // a few ulps of the spectrum's largest value.
  constexpr double staged_tolerance = 1e-6;
  constexpr std::size_t smallest = 512;
  constexpr std::size_t largest = 65'536;
  for (std::size_t size = smallest; size <= largest; size *= 2) {
    const auto samples = random_signal(size, static_cast<std::uint32_t>(size));
    oscillate::warp_dsp::RealFft whole(size);
    oscillate::warp_dsp::AlignedBuffer input(size);
    oscillate::warp_dsp::AlignedBuffer expected(size);
    std::ranges::copy(samples, input.data());
    whole.forward(input.data(), expected.data());
    oscillate::warp_dsp::StagedRealFft staged(size);
    std::ranges::copy(samples, staged.input().begin());
    oscillate::warp_dsp::AlignedBuffer spectrum(size);
    for (std::size_t step = 0; step < staged.steps(); ++step)
      staged.step(step, spectrum.span());
    double peak = 0.0;
    for (std::size_t i = 0; i < size; ++i)
      peak = std::max(peak, static_cast<double>(std::fabs(expected[i])));
    for (std::size_t i = 0; i < size; ++i)
      REQUIRE_LT(std::fabs(spectrum[i] - expected[i]), staged_tolerance * peak,
                 "the staged transform is the whole one at " + std::to_string(size) + ", " +
                     std::to_string(i));
  }
  for (const std::size_t size : {256U, 768U, 1000U})
    require(!oscillate::warp_dsp::StagedRealFft::supported(size),
            "unsplittable staged size accepted");
}

// bin_power reads DC and Nyquist from their packed places.
void bin_power_reads_the_packed_bins() {
  constexpr std::size_t size = 8;
  const std::array<float, size> spectrum{3.0F, 4.0F, 1.0F, 2.0F, 0.0F, 0.0F, 0.0F, 5.0F};
  constexpr float dc = 9.0F;
  constexpr float nyquist = 16.0F;
  constexpr float first = 5.0F;
  constexpr float third = 25.0F;
  REQUIRE_EQ(oscillate::warp_dsp::bin_power(spectrum.data(), size, 0), dc, "DC");
  REQUIRE_EQ(oscillate::warp_dsp::bin_power(spectrum.data(), size, size / 2), nyquist, "Nyquist");
  REQUIRE_EQ(oscillate::warp_dsp::bin_power(spectrum.data(), size, 1), first, "bin 1");
  REQUIRE_EQ(oscillate::warp_dsp::bin_power(spectrum.data(), size, 3), third, "bin 3");
}

} // namespace

int main() {
  try {
    fft_sizes_are_validated();
    source_view_handles_extreme_positions();
    fft_matches_dft_and_round_trips();
    unordered_products_convolve();
    staged_transform_matches_the_whole_one();
    bin_power_reads_the_packed_bins();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}

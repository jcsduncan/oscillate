#include "core/real_fft.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"
#include "oscillate/warp_dsp/source_view.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

namespace {
namespace detail = oscillate::warp_dsp::detail;

using oscillate::test::require;

void fft_sizes_are_validated() {
  for (const std::size_t size : {32U, 96U, 160U, 480U, 512U, 4096U, 8192U})
    require(detail::RealFft::supported(size), "valid FFT size rejected");
  for (const std::size_t size : {0U, 16U, 100U, 224U, 1000U})
    require(!detail::RealFft::supported(size), "invalid FFT size accepted");
  try {
    detail::RealFft fft(std::numeric_limits<std::size_t>::max());
    throw std::runtime_error("oversized FFT was accepted");
  } catch (const std::invalid_argument&) {
  }
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
    detail::RealFft fft(size);
    detail::AlignedBuffer input(size);
    detail::AlignedBuffer spectrum(size);
    detail::AlignedBuffer output(size);
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

} // namespace

int main() {
  try {
    fft_sizes_are_validated();
    source_view_handles_extreme_positions();
    fft_matches_dft_and_round_trips();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}

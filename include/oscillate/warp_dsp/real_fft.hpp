#pragma once

#include <cstddef>
#include <memory>
#include <span>

struct PFFFT_Setup;

// The realtime FFT (ADR-0126): the one transform for the audio thread, on
// the pinned PFFFT. Warp, Clay, Cast and every native effect that analyses
// or convolves on the callback use it. The double-precision
// native_dsp::dsp::OfflineFft is for offline and control-thread work only.
namespace oscillate::warp_dsp {

// SIMD-aligned float storage for transform buffers, zero-initialised.
// Allocates on construction only (control).
class AlignedBuffer {
public:
  // The alignment of every buffer, in bytes: a cache line, which covers the
  // 16 bytes the SSE transforms need.
  static constexpr std::size_t alignment = 64;

  AlignedBuffer() = default;
  explicit AlignedBuffer(std::size_t count);
  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;
  AlignedBuffer(AlignedBuffer&& other) noexcept;
  AlignedBuffer& operator=(AlignedBuffer&& other) noexcept;
  ~AlignedBuffer();

  [[nodiscard]] float* data() noexcept { return data_; }
  [[nodiscard]] const float* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::span<float> span() noexcept { return {data_, size_}; }
  [[nodiscard]] std::span<const float> span() const noexcept { return {data_, size_}; }
  float& operator[](std::size_t index) noexcept { return data_[index]; }
  const float& operator[](std::size_t index) const noexcept { return data_[index]; }
  void zero() noexcept;

private:
  float* data_ = nullptr;
  std::size_t size_ = 0;
};

// A single-precision real transform of one size. Sizes are multiples of 32
// with only 2, 3 and 5 as prime factors. The constructor allocates (control
// only); every transform allocates nothing, takes no lock and is safe on the
// callback. Results depend only on the input, so they are the same for any
// split of the audio into callbacks.
//
// Every buffer holds size() floats at a 16-byte boundary (AlignedBuffer, or
// a multiple of four floats into one); input and output may be the same.
// Transforms are unscaled both ways: inverse(forward(x)) == size() * x.
class RealFft {
public:
  explicit RealFft(std::size_t size);
  RealFft(const RealFft&) = delete;
  RealFft& operator=(const RealFft&) = delete;
  RealFft(RealFft&& other) noexcept;
  RealFft& operator=(RealFft&& other) noexcept;
  ~RealFft();

  [[nodiscard]] static bool supported(std::size_t size) noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  // Ordered spectra: DC, Nyquist, then the real and imaginary parts of
  // bins 1 to size() / 2 - 1.
  void forward(const float* input, float* spectrum) noexcept;
  void inverse(const float* spectrum, float* output) noexcept;

  // Spectra in PFFFT's internal order, which skips the reordering: for a
  // path that only multiplies spectra (convolution, correlation) and
  // transforms the product back.
  void forward_unordered(const float* input, float* spectrum) noexcept;
  void inverse_unordered(const float* spectrum, float* output) noexcept;
  // product += a * b * scale, and product = a * b * scale, bin by bin, on
  // unordered spectra. Any of the three may be the same buffer.
  void multiply_accumulate(const float* a, const float* b, float* product,
                           float scale) const noexcept;
  void multiply(const float* a, const float* b, float* product, float scale) const noexcept;

private:
  struct SetupDeleter {
    void operator()(PFFFT_Setup* setup) const noexcept;
  };
  std::size_t size_;
  std::unique_ptr<PFFFT_Setup, SetupDeleter> setup_;
  AlignedBuffer work_;
};

// RealFft's forward transform in steps, so one long transform can be
// spread over many callbacks with no callback carrying all of it (Keel's
// analysis, ADR-0119). The size is a power of two from 512: its size / 2
// complex points (consecutive input pairs) as rows by columns, a short
// complex transform per column, then twiddled, a short one per row (the
// four-step transform), then the real spectrum split off in chunks. Every
// step costs about the same. The result is RealFft::forward's to float
// rounding. Construction allocates (control); steps allocate nothing.
class StagedRealFft {
public:
  explicit StagedRealFft(std::size_t size);
  StagedRealFft(const StagedRealFft&) = delete;
  StagedRealFft& operator=(const StagedRealFft&) = delete;
  StagedRealFft(StagedRealFft&& other) noexcept;
  StagedRealFft& operator=(StagedRealFft&& other) noexcept;
  ~StagedRealFft();

  [[nodiscard]] static bool supported(std::size_t size) noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t steps() const noexcept { return columns_ + rows_ + chunks_; }

  // The input: size() floats, written before step 0 and read until the
  // transform ends.
  [[nodiscard]] std::span<float> input() noexcept { return input_.span(); }
  // Step `index` of steps(), each once and in order. The split steps write
  // `spectrum` (size() floats), which after the last holds the ordered
  // spectrum, as RealFft::forward gives it.
  void step(std::size_t index, std::span<float> spectrum) noexcept;

private:
  void column(std::size_t c) noexcept;
  void row(std::size_t r) noexcept;
  void split(std::size_t chunk, std::span<float> spectrum) const noexcept;

  std::size_t size_;
  std::size_t points_;  // size / 2 complex points.
  std::size_t rows_;    // R: each column's length.
  std::size_t columns_; // C: each row's length.
  std::size_t chunks_;  // Steps that split off the real spectrum.
  std::unique_ptr<PFFFT_Setup, void (*)(PFFFT_Setup*)> column_setup_;
  std::unique_ptr<PFFFT_Setup, void (*)(PFFFT_Setup*)> row_setup_;
  AlignedBuffer input_;      // Then the column transforms, in place.
  AlignedBuffer points_out_; // The complex transform, in order.
  AlignedBuffer line_;       // One column or row.
  AlignedBuffer work_;
  AlignedBuffer twiddles_;       // e^(-2 pi i c k / points), [c][k].
  AlignedBuffer split_twiddles_; // e^(-2 pi i k / size), k < points / 2 + 1.
};

// The power (squared magnitude) of bin `bin`, 0 to size / 2, of an ordered
// spectrum of `size` points.
[[nodiscard]] inline float bin_power(const float* spectrum, std::size_t size,
                                     std::size_t bin) noexcept {
  if (bin == 0) {
    return spectrum[0] * spectrum[0];
  }
  if (2 * bin == size) {
    return spectrum[1] * spectrum[1];
  }
  const float re = spectrum[2 * bin];
  const float im = spectrum[2 * bin + 1];
  return re * re + im * im;
}

} // namespace oscillate::warp_dsp

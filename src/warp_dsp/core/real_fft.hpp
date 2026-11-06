#pragma once

#include <cstddef>
#include <memory>

struct PFFFT_Setup;

namespace oscillate::warp_dsp::detail {

// Aligned float storage for FFT buffers (SIMD-aligned, zero-initialised).
class AlignedBuffer {
public:
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
  float& operator[](std::size_t index) noexcept { return data_[index]; }
  const float& operator[](std::size_t index) const noexcept { return data_[index]; }

private:
  float* data_ = nullptr;
  std::size_t size_ = 0;
};

// Single-precision real FFT (pinned PFFFT). Sizes must be multiples of 32
// with only 2, 3 and 5 as prime factors. Spectra use the packed order
// [DC, Nyquist, re1, im1, ..., re(n/2-1), im(n/2-1)]. The inverse is
// unnormalised: inverse(forward(x)) == n * x.
class RealFft {
public:
  explicit RealFft(std::size_t size);
  RealFft(const RealFft&) = delete;
  RealFft& operator=(const RealFft&) = delete;
  RealFft(RealFft&&) noexcept;
  RealFft& operator=(RealFft&&) noexcept;
  ~RealFft();

  [[nodiscard]] static bool supported(std::size_t size) noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  // Buffers must hold size() aligned floats and may alias for in-place use.
  void forward(const float* input, float* spectrum) noexcept;
  void inverse(const float* spectrum, float* output) noexcept;

private:
  struct SetupDeleter {
    void operator()(PFFFT_Setup* setup) const noexcept;
  };
  std::size_t size_;
  std::unique_ptr<PFFFT_Setup, SetupDeleter> setup_;
  AlignedBuffer work_;
};

} // namespace oscillate::warp_dsp::detail

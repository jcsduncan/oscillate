#include "real_fft.hpp"

#include <pffft/pffft.h>

#include <algorithm>
#include <new>
#include <stdexcept>
#include <utility>

namespace oscillate::warp_dsp::detail {
namespace {
[[nodiscard]] std::size_t validated_fft_size(std::size_t size) {
  if (!RealFft::supported(size))
    throw std::invalid_argument("Unsupported real FFT size");
  return size;
}
} // namespace

AlignedBuffer::AlignedBuffer(std::size_t count) : size_(count) {
  data_ =
      static_cast<float*>(pffft_aligned_malloc(std::max<std::size_t>(count, 1) * sizeof(float)));
  if (data_ == nullptr)
    throw std::bad_alloc();
  std::fill_n(data_, count, 0.0F);
}

AlignedBuffer::AlignedBuffer(AlignedBuffer&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

AlignedBuffer& AlignedBuffer::operator=(AlignedBuffer&& other) noexcept {
  if (this != &other) {
    pffft_aligned_free(data_);
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

AlignedBuffer::~AlignedBuffer() { pffft_aligned_free(data_); }

void RealFft::SetupDeleter::operator()(PFFFT_Setup* setup) const noexcept {
  pffft_destroy_setup(setup);
}

bool RealFft::supported(std::size_t size) noexcept {
  if (size < 32 || size % 32 != 0 || size > (std::size_t{1} << 20))
    return false;
  auto remaining = size;
  for (const std::size_t factor : {2U, 3U, 5U}) {
    while (remaining % factor == 0)
      remaining /= factor;
  }
  return remaining == 1;
}

RealFft::RealFft(std::size_t size) : size_(validated_fft_size(size)), work_(size_) {
  setup_.reset(pffft_new_setup(static_cast<int>(size), PFFFT_REAL));
  if (!setup_)
    throw std::bad_alloc();
}

RealFft::RealFft(RealFft&&) noexcept = default;
RealFft& RealFft::operator=(RealFft&&) noexcept = default;
RealFft::~RealFft() = default;

void RealFft::forward(const float* input, float* spectrum) noexcept {
  pffft_transform_ordered(setup_.get(), input, spectrum, work_.data(), PFFFT_FORWARD);
}

void RealFft::inverse(const float* spectrum, float* output) noexcept {
  pffft_transform_ordered(setup_.get(), spectrum, output, work_.data(), PFFFT_BACKWARD);
}

} // namespace oscillate::warp_dsp::detail

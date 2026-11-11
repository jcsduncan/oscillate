#include "oscillate/warp_dsp/real_fft.hpp"

#include <pffft/pffft.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace oscillate::warp_dsp {
namespace {

// PFFFT's smallest real size: twice the square of its four-float vectors.
constexpr std::size_t minimum_size = 32;
// The largest size a setup is built for (2^20 points).
constexpr std::size_t maximum_size = std::size_t{1} << 20U;
// The SSE transforms load and store four floats at a 16-byte boundary.
constexpr std::uintptr_t transform_alignment = 16;

[[nodiscard]] std::size_t validated_fft_size(std::size_t size) {
  if (!RealFft::supported(size)) {
    throw std::invalid_argument("Unsupported real FFT size");
  }
  return size;
}

// The staged transform's smallest size: rows and columns of at least 16
// complex points, PFFFT's smallest complex size.
constexpr std::size_t staged_minimum = 512;
constexpr std::size_t smallest_complex = 16;
// Each chunk of the real split takes this many rows' worth of bins, about
// a row transform's work.
constexpr std::size_t split_rows_per_chunk = 2;

[[nodiscard]] [[maybe_unused]] bool aligned(const float* buffer) noexcept {
  return reinterpret_cast<std::uintptr_t>(buffer) % transform_alignment == 0;
}

} // namespace

static_assert(AlignedBuffer::alignment % transform_alignment == 0);

AlignedBuffer::AlignedBuffer(std::size_t count) : size_(count) {
  data_ =
      static_cast<float*>(pffft_aligned_malloc(std::max<std::size_t>(count, 1) * sizeof(float)));
  if (data_ == nullptr) {
    throw std::bad_alloc();
  }
  zero();
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

void AlignedBuffer::zero() noexcept {
  if (data_ != nullptr) {
    std::memset(data_, 0, size_ * sizeof(float));
  }
}

void RealFft::SetupDeleter::operator()(PFFFT_Setup* setup) const noexcept {
  pffft_destroy_setup(setup);
}

bool RealFft::supported(std::size_t size) noexcept {
  if (size < minimum_size || size % minimum_size != 0 || size > maximum_size) {
    return false;
  }
  auto remaining = size;
  for (const std::size_t factor : {2U, 3U, 5U}) {
    while (remaining % factor == 0) {
      remaining /= factor;
    }
  }
  return remaining == 1;
}

RealFft::RealFft(std::size_t size) : size_(validated_fft_size(size)), work_(size_) {
  setup_.reset(pffft_new_setup(static_cast<int>(size), PFFFT_REAL));
  if (!setup_) {
    throw std::bad_alloc();
  }
}

RealFft::RealFft(RealFft&& other) noexcept = default;
RealFft& RealFft::operator=(RealFft&& other) noexcept = default;
RealFft::~RealFft() = default;

void RealFft::forward(const float* input, float* spectrum) noexcept {
  assert(aligned(input) && aligned(spectrum));
  pffft_transform_ordered(setup_.get(), input, spectrum, work_.data(), PFFFT_FORWARD);
}

void RealFft::inverse(const float* spectrum, float* output) noexcept {
  assert(aligned(spectrum) && aligned(output));
  pffft_transform_ordered(setup_.get(), spectrum, output, work_.data(), PFFFT_BACKWARD);
}

void RealFft::forward_unordered(const float* input, float* spectrum) noexcept {
  assert(aligned(input) && aligned(spectrum));
  pffft_transform(setup_.get(), input, spectrum, work_.data(), PFFFT_FORWARD);
}

void RealFft::inverse_unordered(const float* spectrum, float* output) noexcept {
  assert(aligned(spectrum) && aligned(output));
  pffft_transform(setup_.get(), spectrum, output, work_.data(), PFFFT_BACKWARD);
}

void RealFft::multiply_accumulate(const float* a, const float* b, float* product,
                                  float scale) const noexcept {
  assert(aligned(a) && aligned(b) && aligned(product));
  pffft_zconvolve_accumulate(setup_.get(), a, b, product, scale);
}

void RealFft::multiply(const float* a, const float* b, float* product, float scale) const noexcept {
  assert(aligned(a) && aligned(b) && aligned(product));
  pffft_zconvolve_no_accu(setup_.get(), a, b, product, scale);
}

bool StagedRealFft::supported(std::size_t size) noexcept {
  return std::has_single_bit(size) && size >= staged_minimum && size <= maximum_size;
}

namespace {

[[nodiscard]] std::size_t validated_staged_size(std::size_t size) {
  if (!StagedRealFft::supported(size)) {
    throw std::invalid_argument("Unsupported staged real FFT size");
  }
  return size;
}

[[nodiscard]] PFFFT_Setup* complex_setup(std::size_t points) {
  PFFFT_Setup* setup = pffft_new_setup(static_cast<int>(points), PFFFT_COMPLEX);
  if (setup == nullptr) {
    throw std::bad_alloc();
  }
  return setup;
}

void destroy_setup(PFFFT_Setup* setup) { pffft_destroy_setup(setup); }

// rows * columns == points, rows the largest power of two at or below its
// square root.
[[nodiscard]] std::size_t rows_for(std::size_t points) noexcept {
  return std::size_t{1} << (static_cast<unsigned>(std::countr_zero(points)) / 2U);
}

} // namespace

StagedRealFft::StagedRealFft(std::size_t size)
    : size_(validated_staged_size(size)), points_(size_ / 2), rows_(rows_for(points_)),
      columns_(points_ / rows_), chunks_((points_ - 1 + split_rows_per_chunk * columns_ - 1) /
                                         (split_rows_per_chunk * columns_)),
      column_setup_(complex_setup(rows_), &destroy_setup),
      row_setup_(complex_setup(columns_), &destroy_setup), input_(size_), points_out_(size_),
      line_(2 * std::max(rows_, columns_)), work_(2 * std::max(rows_, columns_)),
      twiddles_(2 * points_), split_twiddles_(2 * points_) {
  static_assert(staged_minimum / 2 >= smallest_complex * smallest_complex);
  const double turn = -2.0 * std::numbers::pi;
  for (std::size_t c = 0; c < columns_; ++c) {
    for (std::size_t k = 0; k < rows_; ++k) {
      // c k < points, so the angle is exact before the division.
      const double angle = turn * static_cast<double>(c * k) / static_cast<double>(points_);
      twiddles_[2 * (c * rows_ + k)] = static_cast<float>(std::cos(angle));
      twiddles_[2 * (c * rows_ + k) + 1] = static_cast<float>(std::sin(angle));
    }
  }
  for (std::size_t k = 0; k < points_; ++k) {
    const double angle = turn * static_cast<double>(k) / static_cast<double>(size_);
    split_twiddles_[2 * k] = static_cast<float>(std::cos(angle));
    split_twiddles_[2 * k + 1] = static_cast<float>(std::sin(angle));
  }
}

StagedRealFft::StagedRealFft(StagedRealFft&& other) noexcept = default;
StagedRealFft& StagedRealFft::operator=(StagedRealFft&& other) noexcept = default;
StagedRealFft::~StagedRealFft() = default;

void StagedRealFft::step(std::size_t index, std::span<float> spectrum) noexcept {
  assert(spectrum.size() >= size_);
  if (index < columns_) {
    column(index);
  } else if (index < columns_ + rows_) {
    row(index - columns_);
  } else {
    split(index - columns_ - rows_, spectrum);
  }
}

// Point n = c + columns * r. Column c's transform over r, each output k
// times e^(-2 pi i c k / points), back in place (row k, column c).
void StagedRealFft::column(std::size_t c) noexcept {
  float* const z = input_.data();
  float* const line = line_.data();
  for (std::size_t r = 0; r < rows_; ++r) {
    line[2 * r] = z[2 * (c + columns_ * r)];
    line[2 * r + 1] = z[2 * (c + columns_ * r) + 1];
  }
  pffft_transform_ordered(column_setup_.get(), line, line, work_.data(), PFFFT_FORWARD);
  const float* const w = twiddles_.data() + 2 * c * rows_;
  for (std::size_t k = 0; k < rows_; ++k) {
    const float re = line[2 * k];
    const float im = line[2 * k + 1];
    const float w_re = w[2 * k];
    const float w_im = w[2 * k + 1];
    z[2 * (c + columns_ * k)] = re * w_re - im * w_im;
    z[2 * (c + columns_ * k) + 1] = re * w_im + im * w_re;
  }
}

// Row k's transform over c: point k + rows * j of the whole transform.
void StagedRealFft::row(std::size_t k) noexcept {
  float* const line = line_.data();
  pffft_transform_ordered(row_setup_.get(), input_.data() + 2 * columns_ * k, line, work_.data(),
                          PFFFT_FORWARD);
  float* const out = points_out_.data();
  for (std::size_t j = 0; j < columns_; ++j) {
    out[2 * (k + rows_ * j)] = line[2 * j];
    out[2 * (k + rows_ * j) + 1] = line[2 * j + 1];
  }
}

// The real input's spectrum from Z, the transform of its consecutive pairs:
// X[k] = (Z[k] + conj Z[-k]) / 2 - i e^(-2 pi i k / size) (Z[k] - conj Z[-k]) / 2.
void StagedRealFft::split(std::size_t chunk, std::span<float> spectrum) const noexcept {
  const float* const z = points_out_.data();
  const float* const w = split_twiddles_.data();
  float* const x = spectrum.data();
  const std::size_t length = split_rows_per_chunk * columns_;
  const std::size_t first = 1 + chunk * length;
  const std::size_t last = std::min(points_, first + length);
  if (chunk == 0) {
    x[0] = z[0] + z[1];
    x[1] = z[0] - z[1];
  }
  for (std::size_t k = first; k < last; ++k) {
    const std::size_t m = points_ - k;
    const float a_re = z[2 * k];
    const float a_im = z[2 * k + 1];
    const float b_re = z[2 * m];
    const float b_im = -z[2 * m + 1];
    const float even_re = 0.5F * (a_re + b_re);
    const float even_im = 0.5F * (a_im + b_im);
    const float odd_re = 0.5F * (a_re - b_re);
    const float odd_im = 0.5F * (a_im - b_im);
    // -i w odd = (Im(w odd), -Re(w odd)).
    const float turned_re = w[2 * k] * odd_re - w[2 * k + 1] * odd_im;
    const float turned_im = w[2 * k] * odd_im + w[2 * k + 1] * odd_re;
    x[2 * k] = even_re + turned_im;
    x[2 * k + 1] = even_im - turned_re;
  }
}

} // namespace oscillate::warp_dsp

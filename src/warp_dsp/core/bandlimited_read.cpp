#include "bandlimited_read.hpp"

#include "oscillate/warp_dsp/time_map.hpp"

#include <array>
#include <cmath>

namespace oscillate::warp_dsp {
namespace detail {
namespace {

using bandlimited_detail::Lanes;
using bandlimited_detail::lanes;
using bandlimited_detail::phase_mask;
using bandlimited_detail::phase_scale;
using bandlimited_detail::phase_shift;
using bandlimited_detail::reduce;

// One wing of taps walking away from the read point.
struct Wing {
  const float* left;
  const float* right;
  std::int64_t direction;
};

// Table positions for one scaled wing in double precision (oracle path).
struct FloatWalk {
  double first_offset;
  double cutoff;
};

void reference_unity_wing(const BandlimitedKernel& kernel, const Wing& wing,
                          std::uint32_t offset_q16, StereoSample& sum) noexcept {
  const float* left = wing.left;
  const float* right = wing.right;
  const auto direction = wing.direction;
  const auto index = offset_q16 >> phase_shift;
  const auto fraction = static_cast<float>(offset_q16 & phase_mask) * phase_scale;
  const auto limit = kernel.table_size();
  std::int64_t tap = 0;
  for (auto table = index; table < limit; table += BandlimitedKernel::table_resolution) {
    const float weight = kernel.at(table, fraction);
    sum.left += weight * left[tap];
    sum.right += weight * right[tap];
    tap += direction;
  }
}

void reference_scaled_wing(const BandlimitedKernel& kernel, const Wing& wing, FloatWalk walk,
                           StereoSample& sum) noexcept {
  const float* left = wing.left;
  const float* right = wing.right;
  const auto direction = wing.direction;
  const double step = walk.cutoff * BandlimitedKernel::table_resolution;
  const double limit = static_cast<double>(kernel.table_size());
  double table = walk.first_offset * step;
  std::int64_t tap = 0;
  while (table < limit) {
    const auto index = static_cast<std::uint32_t>(table);
    const float weight = kernel.at(index, static_cast<float>(table - index));
    sum.left += weight * left[tap];
    sum.right += weight * right[tap];
    table += step;
    tap += direction;
  }
}

} // namespace
} // namespace detail

BandlimitedReadPlan make_bandlimited_read_plan(double speed) noexcept {
  if (speed <= 1.0)
    return {speed, 1.0F, 0};
  const double cutoff = 1.0 / speed;
  const auto step = static_cast<std::uint64_t>(
      std::llround(cutoff * BandlimitedKernel::table_resolution *
                   static_cast<double>(1U << bandlimited_detail::table_fraction_bits)));
  return {speed, static_cast<float>(cutoff), step};
}

namespace detail {

StereoSample bandlimited_read(const SourceWindow& window, const BandlimitedKernel& kernel,
                              std::uint64_t position, const BandlimitedReadPlan& plan) noexcept {
  const auto frame = static_cast<std::int64_t>(position >> source_fraction_bits);
  const auto fraction = static_cast<std::uint32_t>(position & (source_unit - 1));
  return warp_dsp::bandlimited_read(window.left(frame), window.right(frame), fraction, kernel,
                                    plan);
}

StereoSample bandlimited_read(const SourceWindow& window, const BandlimitedKernel& kernel,
                              ReadAt read) noexcept {
  return bandlimited_read(window, kernel, read.position, make_bandlimited_read_plan(read.speed));
}

FixedRateReader::FixedRateReader(const KernelDesign& design, double speed)
    : speed_(speed), half_width_(0), taps_(0) {
  const double cutoff = speed > 1.0 ? 1.0 / speed : 1.0;
  half_width_ = static_cast<std::int64_t>(std::ceil(design.zero_crossings / cutoff));
  const auto width = static_cast<std::size_t>(2 * half_width_);
  taps_ = (width + lanes - 1) / lanes * lanes;
  constexpr std::size_t phases = BandlimitedKernel::table_resolution;
  std::vector<float> rows((phases + 1) * taps_, 0.0F);
  for (std::size_t phase = 0; phase <= phases; ++phase) {
    const double fraction = static_cast<double>(phase) / phases;
    for (std::size_t tap = 0; tap < width; ++tap) {
      // Distance from the read point to frame n - half_width + 1 + tap.
      const auto index = static_cast<std::int64_t>(tap);
      const double distance = index < half_width_
                                  ? fraction + static_cast<double>(half_width_ - 1 - index)
                                  : static_cast<double>(index - half_width_ + 1) - fraction;
      rows[phase * taps_ + tap] =
          static_cast<float>(cutoff * reference_kernel(distance * cutoff, design));
    }
  }
  rows_.assign(rows.begin(), rows.end() - static_cast<std::ptrdiff_t>(taps_));
  deltas_.resize(phases * taps_);
  for (std::size_t index = 0; index < deltas_.size(); ++index)
    deltas_[index] = rows[index + taps_] - rows[index];
}

StereoSample FixedRateReader::read(const SourceWindow& window,
                                   std::uint64_t position) const noexcept {
  const auto frame = static_cast<std::int64_t>(position >> source_fraction_bits);
  const auto fraction = static_cast<std::uint32_t>(position & (source_unit - 1));
  return read_frame(window.left(frame), window.right(frame), fraction);
}

StereoSample FixedRateReader::read_frame(const float* frame_left, const float* frame_right,
                                         std::uint32_t fraction) const noexcept {
  if (speed_ <= 1.0 && fraction == 0)
    return {*frame_left, *frame_right};
  const auto phase = fraction >> phase_shift;
  const float blend = static_cast<float>(fraction & phase_mask) * phase_scale;
  const float* row = rows_.data() + static_cast<std::size_t>(phase) * taps_;
  const float* delta = deltas_.data() + static_cast<std::size_t>(phase) * taps_;
  const float* left = frame_left - half_width_ + 1;
  const float* right = frame_right - half_width_ + 1;
  Lanes left_sum{};
  Lanes right_sum{};
  const std::size_t blocks = taps_ / lanes;
  for (std::size_t block = 0; block < blocks; ++block) {
    const std::size_t base = block * lanes;
    for (std::size_t lane = 0; lane < lanes; ++lane) {
      const float weight = row[base + lane] + blend * delta[base + lane];
      left_sum[lane] += weight * left[base + lane];
      right_sum[lane] += weight * right[base + lane];
    }
  }
  return {reduce(left_sum), reduce(right_sum)};
}

StereoSample reference_bandlimited_read(const SourceWindow& window, const BandlimitedKernel& kernel,
                                        ReadAt read) noexcept {
  const auto position = read.position;
  const double speed = read.speed;
  const auto frame = static_cast<std::int64_t>(position >> source_fraction_bits);
  const auto fraction = static_cast<std::uint32_t>(position & (source_unit - 1));
  StereoSample sum;
  const float* left = window.left(frame);
  const float* right = window.right(frame);
  if (speed <= 1.0) {
    reference_unity_wing(kernel, {left, right, -1}, fraction, sum);
    reference_unity_wing(kernel, {left + 1, right + 1, 1},
                         static_cast<std::uint32_t>(source_unit - fraction), sum);
    return sum;
  }
  const double cutoff = 1.0 / speed;
  const double offset = static_cast<double>(fraction) / static_cast<double>(source_unit);
  reference_scaled_wing(kernel, {left, right, -1}, {offset, cutoff}, sum);
  reference_scaled_wing(kernel, {left + 1, right + 1, 1}, {1.0 - offset, cutoff}, sum);
  sum.left *= static_cast<float>(cutoff);
  sum.right *= static_cast<float>(cutoff);
  return sum;
}

} // namespace detail
} // namespace oscillate::warp_dsp

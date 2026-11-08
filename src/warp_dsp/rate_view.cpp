#include "oscillate/warp_dsp/rate_view.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>
#include <numeric>

namespace oscillate::warp_dsp {
namespace {

using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

constexpr std::size_t lanes = 8;
using Lanes = std::array<float, lanes>;

[[nodiscard]] bool supported_project_rate(std::uint32_t rate) noexcept {
  return rate == 44'100 || rate == 48'000 || rate == 88'200 || rate == 96'000;
}

[[nodiscard]] bool supported_source_rate(std::uint32_t rate) noexcept {
  return rate >= 8'000 && rate <= 384'000;
}

[[nodiscard]] double bessel_i0(double x) noexcept {
  double sum = 1.0;
  double term = 1.0;
  const double quarter_square = x * x / 4.0;
  for (int k = 1; k < 128; ++k) {
    term *= quarter_square / (static_cast<double>(k) * static_cast<double>(k));
    sum += term;
    if (term < sum * 1e-17)
      break;
  }
  return sum;
}

// Filter geometry in source frames for one rate pair.
struct Design {
  double cutoff = 1.0;    // fraction of the source Nyquist
  double half_span = 0.0; // window half-length in source frames
  std::int64_t reach = 0; // whole source frames each side of the read point
};

[[nodiscard]] Design design_for(std::uint32_t source_rate, std::uint32_t project_rate) noexcept {
  const double lower = std::min(1.0, static_cast<double>(project_rate) / source_rate);
  Design design;
  design.cutoff = RateConvertedSourceView::cutoff * lower;
  design.half_span = RateConvertedSourceView::zero_crossings / lower;
  design.reach = static_cast<std::int64_t>(std::ceil(design.half_span));
  return design;
}

// Unnormalised windowed sinc at distance t source frames from the read point.
[[nodiscard]] double tap_weight(double t, const Design& design) noexcept {
  const double magnitude = std::fabs(t);
  if (magnitude >= design.half_span)
    return 0.0;
  const double ratio = magnitude / design.half_span;
  const double window =
      bessel_i0(RateConvertedSourceView::kaiser_beta * std::sqrt(1.0 - ratio * ratio)) /
      bessel_i0(RateConvertedSourceView::kaiser_beta);
  const double angle = std::numbers::pi * design.cutoff * magnitude;
  const double sinc = magnitude == 0.0 ? 1.0 : std::sin(angle) / angle;
  return design.cutoff * sinc * window;
}

[[nodiscard]] std::int64_t floor_div(std::int64_t numerator, std::int64_t denominator) noexcept {
  const auto quotient = numerator / denominator;
  return (numerator % denominator != 0 && numerator < 0) ? quotient - 1 : quotient;
}

[[nodiscard]] float reduce(const Lanes& sum) noexcept {
  return ((sum[0] + sum[4]) + (sum[1] + sum[5])) + ((sum[2] + sum[6]) + (sum[3] + sum[7]));
}

void silence(std::span<float> left, std::span<float> right) noexcept {
  std::ranges::fill(left, 0.0F);
  std::ranges::fill(right, 0.0F);
}

} // namespace

Result<std::uint64_t> project_rate_frames(std::uint64_t source_frames, std::uint32_t source_rate,
                                          std::uint32_t project_rate) {
  if (!supported_source_rate(source_rate) || !supported_project_rate(project_rate)) {
    return Result<std::uint64_t>::failure(
        {ErrorCode::invalid_argument, "Source or project sample rate is unsupported"});
  }
  const auto whole = source_frames / source_rate;
  const auto remainder = source_frames % source_rate;
  const auto fractional = (remainder * project_rate + source_rate / 2U) / source_rate;
  if (whole > (std::numeric_limits<std::uint64_t>::max() - fractional) / project_rate) {
    return Result<std::uint64_t>::failure(
        {ErrorCode::resource_exhausted, "Source-rate duration exceeds project frame range"});
  }
  return Result<std::uint64_t>::success(whole * project_rate + fractional);
}

Result<void> RateConvertedSourceView::supports(std::uint32_t source_rate,
                                               std::uint32_t project_rate) {
  if (!supported_source_rate(source_rate) || !supported_project_rate(project_rate)) {
    return Result<void>::failure(
        {ErrorCode::invalid_argument, "Source or project sample rate is unsupported"});
  }
  const auto divisor = std::gcd(source_rate, project_rate);
  if (project_rate / divisor > max_phases || source_rate / divisor > max_phases) {
    return Result<void>::failure(
        {ErrorCode::unsupported, "Source-rate phase period exceeds the bounded converter"});
  }
  const auto design = design_for(source_rate, project_rate);
  const auto taps = (static_cast<std::size_t>(2 * design.reach) + lanes - 1) / lanes * lanes;
  if (source_rate != project_rate && taps * (project_rate / divisor) > max_bank_coefficients) {
    return Result<void>::failure(
        {ErrorCode::unsupported, "Source-rate converter bank exceeds its memory bound"});
  }
  return Result<void>::success();
}

Result<std::unique_ptr<RateConvertedSourceView>>
RateConvertedSourceView::create(SourceView& source, std::uint32_t source_rate,
                                std::uint32_t project_rate) {
  using Created = Result<std::unique_ptr<RateConvertedSourceView>>;
  if (auto supported = supports(source_rate, project_rate); !supported)
    return Created::failure(supported.error());
  auto frames = project_rate_frames(source.frames(), source_rate, project_rate);
  if (!frames)
    return Created::failure(frames.error());
  const auto divisor = std::gcd(source_rate, project_rate);
  const auto phases = project_rate / divisor;
  const auto step = source_rate / divisor;
  const auto design = design_for(source_rate, project_rate);
  const auto raw_taps = static_cast<std::size_t>(2 * design.reach);
  const auto taps = (raw_taps + lanes - 1) / lanes * lanes;
  try {
    auto view = std::unique_ptr<RateConvertedSourceView>(
        new RateConvertedSourceView(source, frames.value()));
    if (source_rate == project_rate)
      return Created::success(std::move(view));
    view->step_ = step;
    view->phases_ = phases;
    view->taps_ = static_cast<std::uint32_t>(taps);
    view->half_width_ = design.reach;
    // n * step must not overflow for any frame a read may touch.
    view->max_position_ =
        std::numeric_limits<std::int64_t>::max() / static_cast<std::int64_t>(step) - 1;
    view->bank_.assign(taps * phases, 0.0F);
    std::vector<double> row(raw_taps);
    for (std::uint32_t phase = 0; phase < phases; ++phase) {
      const double offset = static_cast<double>(phase) / phases;
      double sum = 0.0;
      for (std::size_t tap = 0; tap < raw_taps; ++tap) {
        const double distance =
            static_cast<double>(static_cast<std::int64_t>(tap) - design.reach + 1) - offset;
        row[tap] = tap_weight(distance, design);
        sum += row[tap];
      }
      auto* out = view->bank_.data() + static_cast<std::size_t>(phase) * taps;
      for (std::size_t tap = 0; tap < raw_taps; ++tap)
        out[tap] = static_cast<float>(row[tap] / sum);
    }
    const auto scratch =
        static_cast<std::size_t>(block_frames * step / phases) + raw_taps + lanes + 2;
    view->scratch_left_.assign(scratch, 0.0F);
    view->scratch_right_.assign(scratch, 0.0F);
    return Created::success(std::move(view));
  } catch (const std::bad_alloc&) {
    return Created::failure(
        {ErrorCode::resource_exhausted, "Source-rate converter allocation failed"});
  }
}

bool RateConvertedSourceView::read(std::int64_t first, std::span<float> left,
                                   std::span<float> right) noexcept {
  if (left.size() != right.size()) {
    silence(left, right);
    return false;
  }
  if (bypass()) {
    if (source_.read(first, left, right))
      return true;
    silence(left, right);
    return false;
  }
  for (std::size_t done = 0; done < left.size();) {
    const auto count = std::min(block_frames, left.size() - done);
    if (!convert(first + static_cast<std::int64_t>(done), left.subspan(done, count),
                 right.subspan(done, count))) {
      silence(left, right);
      return false;
    }
    done += count;
  }
  return true;
}

bool RateConvertedSourceView::convert(std::int64_t first, std::span<float> left,
                                      std::span<float> right) noexcept {
  const auto count = static_cast<std::int64_t>(left.size());
  if (first < -max_position_ || first > max_position_ - count)
    return false;
  silence(left, right);
  // Only frames inside [0, frames()) are converted; the rest stay silent.
  const auto end = static_cast<std::int64_t>(
      std::min<std::uint64_t>(frames_, static_cast<std::uint64_t>(max_position_)));
  const auto begin_frame = std::max<std::int64_t>(first, 0);
  const auto end_frame = std::min<std::int64_t>(first + count, end);
  if (begin_frame >= end_frame)
    return true;
  const auto phases = static_cast<std::int64_t>(phases_);
  const auto first_index = floor_div(begin_frame * step_, phases);
  const auto last_index = floor_div((end_frame - 1) * step_, phases);
  const auto source_first = first_index - half_width_ + 1;
  const auto source_count = static_cast<std::size_t>(last_index - first_index + 2 * half_width_);
  const auto padded = source_count + (taps_ - static_cast<std::size_t>(2 * half_width_));
  if (padded > scratch_left_.size())
    return false;
  auto scratch_left = std::span(scratch_left_).first(padded);
  auto scratch_right = std::span(scratch_right_).first(padded);
  // Zero the lane padding past the last real tap; its weights are zero too,
  // but uninitialised reads would still be undefined.
  std::ranges::fill(scratch_left.subspan(source_count), 0.0F);
  std::ranges::fill(scratch_right.subspan(source_count), 0.0F);
  if (!source_.read(source_first, scratch_left.first(source_count),
                    scratch_right.first(source_count)))
    return false;
  const std::size_t blocks = taps_ / lanes;
  // The source position advances by the same rational step for each output
  // frame. Decompose it once instead of dividing every frame; carry the
  // residue so seeking and block partitioning still use the exact phase.
  const auto first_position = begin_frame * step_;
  auto index = floor_div(first_position, phases);
  auto phase = first_position - index * phases;
  const auto whole_step = step_ / phases;
  const auto phase_step = step_ % phases;
  for (auto frame = begin_frame; frame < end_frame; ++frame) {
    const float* row = bank_.data() + static_cast<std::size_t>(phase) * taps_;
    const auto base = static_cast<std::size_t>(index - first_index);
    const float* source_left = scratch_left_.data() + base;
    const float* source_right = scratch_right_.data() + base;
    Lanes left_sum{};
    Lanes right_sum{};
    for (std::size_t block = 0; block < blocks; ++block) {
      const std::size_t offset = block * lanes;
      for (std::size_t lane = 0; lane < lanes; ++lane) {
        left_sum[lane] += row[offset + lane] * source_left[offset + lane];
        right_sum[lane] += row[offset + lane] * source_right[offset + lane];
      }
    }
    const auto out = static_cast<std::size_t>(frame - first);
    left[out] = reduce(left_sum);
    right[out] = reduce(right_sum);
    index += whole_step;
    phase += phase_step;
    if (phase >= phases) {
      phase -= phases;
      ++index;
    }
  }
  return true;
}

double reference_rate_converted_sample(std::span<const float> source, std::uint32_t source_rate,
                                       std::uint32_t project_rate, std::int64_t output_frame) {
  const auto frames = project_rate_frames(source.size(), source_rate, project_rate);
  if (!frames || output_frame < 0 || static_cast<std::uint64_t>(output_frame) >= frames.value())
    return 0.0;
  if (source_rate == project_rate)
    return source[static_cast<std::size_t>(output_frame)];
  const auto design = design_for(source_rate, project_rate);
  const auto divisor = static_cast<std::int64_t>(std::gcd(source_rate, project_rate));
  const auto step = static_cast<std::int64_t>(source_rate) / divisor;
  const auto phases = static_cast<std::int64_t>(project_rate) / divisor;
  const auto index = floor_div(output_frame * step, phases);
  const double offset =
      static_cast<double>(output_frame * step - index * phases) / static_cast<double>(phases);
  double sum = 0.0;
  double weight_sum = 0.0;
  for (auto frame = index - design.reach + 1; frame <= index + design.reach; ++frame) {
    const double weight = tap_weight(static_cast<double>(frame - index) - offset, design);
    weight_sum += weight;
    if (frame >= 0 && static_cast<std::size_t>(frame) < source.size())
      sum += weight * source[static_cast<std::size_t>(frame)];
  }
  return sum / weight_sum;
}

} // namespace oscillate::warp_dsp

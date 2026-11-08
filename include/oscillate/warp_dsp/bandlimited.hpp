#pragma once

#include "oscillate/warp_dsp/stream.hpp"
#include "oscillate/warp_dsp/time_map.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// The warp engine's band-limited interpolator as a public, allocation-free
// reader (spec 09 §1.3): a shared kernel per quality tier, a read plan for any
// speed (the cutoff falls to 1/speed of the source Nyquist above unity) and a
// read from planar or interleaved stereo at a Q16 fractional position. Every
// read is noexcept and callback-safe; only building a kernel allocates. At a
// whole position and speed <= 1 a read is the source sample, bit for bit.
namespace oscillate::warp_dsp {

struct KernelDesign {
  std::uint32_t zero_crossings = 32;
  double kaiser_beta = 9.0;
};

// One-sided Kaiser-windowed sinc h(x) = sinc(x) * kaiser(x / zero_crossings),
// tabulated at table_resolution points per zero crossing (Smith band-limited
// interpolation). Integer x > 0 is exactly zero, so unity-speed reads at whole
// source frames reproduce the source bit-exactly.
class BandlimitedKernel {
public:
  static constexpr std::uint32_t table_resolution = 512;
  // Q48.16 fraction bits below the table index.
  static constexpr std::uint32_t interpolation_bits = 16 - 9;

  // Throws std::invalid_argument for a design with no zero crossings.
  explicit BandlimitedKernel(const KernelDesign& design);

  [[nodiscard]] const KernelDesign& design() const noexcept { return design_; }
  [[nodiscard]] std::uint32_t zero_crossings() const noexcept { return zero_crossings_; }
  // Kernel value at x = index / table_resolution + fraction / table_resolution.
  [[nodiscard]] float at(std::uint32_t index, float fraction) const noexcept {
    return values_[index] + fraction * deltas_[index];
  }
  // Unity-cutoff polyphase rows: row p holds all 2 * zero_crossings taps for a
  // read at fraction p / table_resolution, ordered from source frame
  // (n - zero_crossings + 1) to (n + zero_crossings); delta row p is
  // row p + 1 minus row p for linear phase interpolation.
  [[nodiscard]] std::uint32_t taps() const noexcept { return 2 * zero_crossings_; }
  [[nodiscard]] const float* phase_row(std::uint32_t phase) const noexcept {
    return rows_.data() + static_cast<std::size_t>(phase) * taps();
  }
  [[nodiscard]] const float* phase_delta(std::uint32_t phase) const noexcept {
    return row_deltas_.data() + static_cast<std::size_t>(phase) * taps();
  }
  [[nodiscard]] std::uint32_t table_size() const noexcept {
    return static_cast<std::uint32_t>(values_.size()) - 1;
  }

  // Shared immutable kernel for a quality tier; built on first use (control
  // side). Null for a tier that valid_quality_tier rejects.
  [[nodiscard]] static std::shared_ptr<const BandlimitedKernel> for_tier(QualityTier tier);

private:
  KernelDesign design_;
  std::uint32_t zero_crossings_;
  std::vector<float> values_;
  std::vector<float> deltas_;
  std::vector<float> rows_;
  std::vector<float> row_deltas_;
};

// Reference evaluation in double precision for oracle tests.
[[nodiscard]] double reference_kernel(double x, const KernelDesign& design);

struct StereoSample {
  float left = 0.0F;
  float right = 0.0F;
};

// Source frames on each side of the read position that a read may touch.
[[nodiscard]] inline std::int64_t bandlimited_reach(const BandlimitedKernel& kernel,
                                                    double speed) noexcept {
  const double scale = speed > 1.0 ? speed : 1.0;
  return static_cast<std::int64_t>(std::ceil(kernel.zero_crossings() * scale)) + 1;
}

// A read speed's scaled kernel walk, prepared once outside the per-sample
// loop. Speed is a magnitude: a reversed read uses the same plan.
struct BandlimitedReadPlan {
  double speed = 1.0;
  float cutoff = 1.0F;
  std::uint64_t step = 0;
};

[[nodiscard]] BandlimitedReadPlan make_bandlimited_read_plan(double speed) noexcept;

namespace bandlimited_detail {

inline constexpr std::uint32_t phase_shift = BandlimitedKernel::interpolation_bits;
inline constexpr std::uint32_t phase_mask = (1U << phase_shift) - 1U;
inline constexpr float phase_scale = 1.0F / static_cast<float>(1U << phase_shift);
// Fixed-point table positions for the scaled (anti-aliasing) path.
inline constexpr std::uint32_t table_fraction_bits = 16;
inline constexpr float table_fraction_scale = 1.0F / static_cast<float>(1U << table_fraction_bits);
// Fixed lane count: the summation order is explicit, so the compiler may
// vectorise it without reassociation and results match at every ISA level.
inline constexpr std::size_t lanes = 8;

using Lanes = std::array<float, lanes>;

[[nodiscard]] inline float reduce(const Lanes& lane) noexcept {
  return ((lane[0] + lane[1]) + (lane[2] + lane[3])) + ((lane[4] + lane[5]) + (lane[6] + lane[7]));
}

// Source samples relative to one frame: planar buffers...
struct Planar {
  const float* left;
  const float* right;
  [[nodiscard]] float l(std::int64_t offset) const noexcept { return left[offset]; }
  [[nodiscard]] float r(std::int64_t offset) const noexcept { return right[offset]; }
};
// ...or interleaved frames with left and right members.
template <typename Frame> struct Interleaved {
  const Frame* frame;
  [[nodiscard]] float l(std::int64_t offset) const noexcept { return frame[offset].left; }
  [[nodiscard]] float r(std::int64_t offset) const noexcept { return frame[offset].right; }
};

// Unity cutoff: one contiguous polyphase row of 2Z taps, interpolated
// linearly between adjacent phases.
template <typename Source>
[[nodiscard]] StereoSample unity_read(const Source& at_frame, const BandlimitedKernel& kernel,
                                      std::uint32_t fraction) noexcept {
  const auto phase = fraction >> phase_shift;
  const float blend = static_cast<float>(fraction & phase_mask) * phase_scale;
  const float* row = kernel.phase_row(phase);
  const float* delta = kernel.phase_delta(phase);
  const auto first = 1 - static_cast<std::int64_t>(kernel.zero_crossings());
  Lanes left_sum{};
  Lanes right_sum{};
  const std::size_t blocks = kernel.taps() / lanes;
  for (std::size_t block = 0; block < blocks; ++block) {
    const std::size_t base = block * lanes;
    for (std::size_t lane = 0; lane < lanes; ++lane) {
      const float weight = row[base + lane] + blend * delta[base + lane];
      const auto tap = first + static_cast<std::int64_t>(base + lane);
      left_sum[lane] += weight * at_frame.l(tap);
      right_sum[lane] += weight * at_frame.r(tap);
    }
  }
  // A custom design whose taps are not a whole number of lane blocks puts
  // its remaining taps in the leading lanes, keeping the order fixed.
  const std::size_t tail = blocks * lanes;
  for (std::size_t index = tail; index < kernel.taps(); ++index) {
    const float weight = row[index] + blend * delta[index];
    const auto tap = first + static_cast<std::int64_t>(index);
    left_sum[index - tail] += weight * at_frame.l(tap);
    right_sum[index - tail] += weight * at_frame.r(tap);
  }
  return {reduce(left_sum), reduce(right_sum)};
}

// A wing's first tap, relative to the frame, and the way it walks.
struct WingTaps {
  std::int64_t origin = 0;
  std::int64_t direction = 1;
};

// Scaled cutoff: tap positions advance by cutoff * resolution table steps in
// 16-bit fixed point, so every read is a pure function of its position.
template <typename Source>
void scaled_wing(const BandlimitedKernel& kernel, const Source& at_frame, WingTaps taps,
                 std::uint64_t first, std::uint64_t step, StereoSample& sum) noexcept {
  const auto limit = static_cast<std::uint64_t>(kernel.table_size()) << table_fraction_bits;
  std::int64_t tap = taps.origin;
  for (auto position = first; position < limit; position += step) {
    const auto index = static_cast<std::uint32_t>(position >> table_fraction_bits);
    const float fraction =
        static_cast<float>(position & ((1U << table_fraction_bits) - 1U)) * table_fraction_scale;
    const float weight = kernel.at(index, fraction);
    sum.left += weight * at_frame.l(tap);
    sum.right += weight * at_frame.r(tap);
    tap += taps.direction;
  }
}

template <typename Source>
[[nodiscard]] StereoSample read(const Source& at_frame, std::uint32_t fraction,
                                const BandlimitedKernel& kernel,
                                const BandlimitedReadPlan& plan) noexcept {
  // A whole-frame unity-band read is exactly the source sample (the kernel is
  // 1 at zero and 0 at every other integer), so skip the dot product.
  if (plan.speed <= 1.0 && fraction == 0)
    return {at_frame.l(0), at_frame.r(0)};
  if (plan.speed <= 1.0)
    return unity_read(at_frame, kernel, fraction);
  const auto step = plan.step;
  // Left wing reads frame, frame-1, ... at distance fraction + j; right wing
  // reads frame+1, frame+2, ... at distance (1 - fraction) + j.
  StereoSample sum;
  scaled_wing(kernel, at_frame, {.origin = 0, .direction = -1},
              (static_cast<std::uint64_t>(fraction) * step) >> source_fraction_bits, step, sum);
  scaled_wing(kernel, at_frame, {.origin = 1, .direction = 1},
              ((source_unit - fraction) * step) >> source_fraction_bits, step, sum);
  sum.left *= plan.cutoff;
  sum.right *= plan.cutoff;
  return sum;
}

} // namespace bandlimited_detail

// Band-limited read from planar stereo. `left` and `right` address the whole
// source frame the Q16 `fraction` (0 .. source_unit - 1) is measured from;
// bandlimited_reach(kernel, plan.speed) frames must be readable on each side.
[[nodiscard]] inline StereoSample bandlimited_read(const float* left, const float* right,
                                                   std::uint32_t fraction,
                                                   const BandlimitedKernel& kernel,
                                                   const BandlimitedReadPlan& plan) noexcept {
  return bandlimited_detail::read(bandlimited_detail::Planar{left, right}, fraction, kernel, plan);
}

// The same read from interleaved frames (any type with float `left` and
// `right` members, such as engine::StereoFrame); `frame` addresses the whole
// frame. Planar and interleaved reads of the same audio are bit-identical.
template <typename Frame>
[[nodiscard]] StereoSample bandlimited_read(const Frame* frame, std::uint32_t fraction,
                                            const BandlimitedKernel& kernel,
                                            const BandlimitedReadPlan& plan) noexcept {
  return bandlimited_detail::read(bandlimited_detail::Interleaved<Frame>{frame}, fraction, kernel,
                                  plan);
}

} // namespace oscillate::warp_dsp

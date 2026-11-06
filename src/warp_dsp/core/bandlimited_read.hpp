#pragma once

#include "bandlimited_kernel.hpp"
#include "source_window.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace oscillate::warp_dsp::detail {

using warp_dsp::bandlimited_reach;
using warp_dsp::BandlimitedReadPlan;
using warp_dsp::make_bandlimited_read_plan;
using warp_dsp::StereoSample;

// Where (Q48.16 source position) and how fast a band-limited read samples.
struct ReadAt {
  std::uint64_t position = 0;
  double speed = 1.0;
};

// Band-limited read at a Q48.16 source position. At speed <= 1 the kernel is
// a pure interpolator (identity at whole frames); above it, the cutoff falls
// to 1/speed of the source Nyquist so the faster read does not alias.
[[nodiscard]] StereoSample bandlimited_read(const SourceWindow& window,
                                            const BandlimitedKernel& kernel, ReadAt read) noexcept;
[[nodiscard]] StereoSample bandlimited_read(const SourceWindow& window,
                                            const BandlimitedKernel& kernel, std::uint64_t position,
                                            const BandlimitedReadPlan& plan) noexcept;

// Contiguous polyphase bank for one fixed read speed, built once (it
// allocates) and then read allocation-free with the vectorised row loop. Used
// wherever the speed is constant for a stream's life, such as transposition.
class FixedRateReader {
public:
  FixedRateReader(const KernelDesign& design, double speed);

  // Source frames on each side of the read position a read may touch.
  // Includes the zero-weight padding that rounds rows to whole lanes.
  [[nodiscard]] std::int64_t reach() const noexcept { return half_width_ + 8; }
  [[nodiscard]] double speed() const noexcept { return speed_; }
  [[nodiscard]] StereoSample read(const SourceWindow& window,
                                  std::uint64_t position) const noexcept;
  // Same read from planar buffers; the pointers address the whole frame the
  // Q16 `fraction` is measured from, with reach() frames valid on each side.
  [[nodiscard]] StereoSample read_frame(const float* left, const float* right,
                                        std::uint32_t fraction) const noexcept;

private:
  double speed_;
  std::int64_t half_width_;
  std::size_t taps_;
  std::vector<float> rows_;
  std::vector<float> deltas_;
};

// Straightforward two-wing evaluation kept as the oracle for the optimised read.
[[nodiscard]] StereoSample reference_bandlimited_read(const SourceWindow& window,
                                                      const BandlimitedKernel& kernel,
                                                      ReadAt read) noexcept;

} // namespace oscillate::warp_dsp::detail

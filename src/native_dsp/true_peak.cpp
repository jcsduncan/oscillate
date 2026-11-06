#include "oscillate/native_dsp/true_peak.hpp"

#include "dsp/bs1770.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <span>

namespace oscillate::native_dsp {

namespace bs1770 = dsp::bs1770;
static_assert(StereoTruePeakMeter::kernel_radius == bs1770::TruePeak::radius);
static_assert(StereoTruePeakMeter::kernel_taps == bs1770::TruePeak::taps);

struct StereoTruePeakMeter::Interpolators {
  bs1770::TruePeak left;
  bs1770::TruePeak right;
};

StereoTruePeakMeter::StereoTruePeakMeter() noexcept = default;
StereoTruePeakMeter::~StereoTruePeakMeter() = default;
StereoTruePeakMeter::StereoTruePeakMeter(StereoTruePeakMeter&&) noexcept = default;
StereoTruePeakMeter& StereoTruePeakMeter::operator=(StereoTruePeakMeter&&) noexcept = default;

bool StereoTruePeakMeter::prepare(std::uint32_t sample_rate) noexcept {
  if (sample_rate != 44100 && sample_rate != 48000 && sample_rate != 88200 &&
      sample_rate != 96000) {
    return false;
  }
  if (!interpolators_) {
    interpolators_.reset(new (std::nothrow) Interpolators{});
    if (!interpolators_) {
      return false;
    }
  }
  sample_rate_ = sample_rate;
  reset();
  return true;
}

void StereoTruePeakMeter::reset() noexcept {
  if (interpolators_) {
    interpolators_->left.reset();
    interpolators_->right.reset();
  }
  reading_ = {};
  finished_ = false;
}

void StereoTruePeakMeter::consume(engine::StereoFrame frame) noexcept {
  const auto finite_peak = [](double value) noexcept {
    return static_cast<float>(
        std::min(value, static_cast<double>(std::numeric_limits<float>::max())));
  };
  reading_.left_true_peak =
      std::max(reading_.left_true_peak, finite_peak(interpolators_->left.push(frame.left)));
  reading_.right_true_peak =
      std::max(reading_.right_true_peak, finite_peak(interpolators_->right.push(frame.right)));
}

bool StereoTruePeakMeter::process(std::span<const engine::StereoFrame> frames) noexcept {
  if (sample_rate_ == 0 || finished_ || frames.size() > max_block_frames ||
      frames.size() > std::numeric_limits<std::uint64_t>::max() - reading_.frames) {
    return false;
  }
  for (const auto& raw : frames) {
    const bool valid = std::isfinite(raw.left) && std::isfinite(raw.right);
    if (!valid && reading_.invalid_frames != std::numeric_limits<std::uint64_t>::max()) {
      ++reading_.invalid_frames;
    }
    const engine::StereoFrame frame{std::isfinite(raw.left) ? raw.left : 0.0F,
                                    std::isfinite(raw.right) ? raw.right : 0.0F};
    reading_.left_sample_peak = std::max(reading_.left_sample_peak, std::abs(frame.left));
    reading_.right_sample_peak = std::max(reading_.right_sample_peak, std::abs(frame.right));
    reading_.left_true_peak = std::max(reading_.left_true_peak, reading_.left_sample_peak);
    reading_.right_true_peak = std::max(reading_.right_true_peak, reading_.right_sample_peak);
    consume(frame);
  }
  reading_.frames += frames.size();
  return true;
}

bool StereoTruePeakMeter::finish() noexcept {
  if (sample_rate_ == 0) {
    return false;
  }
  if (!finished_) {
    for (std::size_t index = 0; index < kernel_radius; ++index) {
      consume({0.0F, 0.0F});
    }
    finished_ = true;
  }
  return true;
}

} // namespace oscillate::native_dsp

#include "oscillate/native_dsp/loudness.hpp"

#include "dsp/bs1770.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

namespace oscillate::native_dsp {

namespace bs1770 = dsp::bs1770;

struct IntegratedLoudnessAnalyzer::Core {
  explicit Core(std::size_t max_blocks) : gate(max_blocks) {}
  std::array<bs1770::KWeighting, 6> weighting{};
  bs1770::StepWindows windows;
  bs1770::Gate gate;
  double latest_momentary_power = 0.0;
};

IntegratedLoudnessAnalyzer::IntegratedLoudnessAnalyzer(std::uint32_t sample_rate,
                                                       LoudnessLayout layout,
                                                       std::size_t max_blocks)
    : layout_(layout), max_blocks_(max_blocks) {
  if (sample_rate != 44'100 && sample_rate != 48'000 && sample_rate != 88'200 &&
      sample_rate != 96'000) {
    status_ = LoudnessStatus::unsupported_rate;
    return;
  }
  switch (layout) {
  case LoudnessLayout::mono:
    channels_ = 1;
    break;
  case LoudnessLayout::stereo:
    channels_ = 2;
    break;
  case LoudnessLayout::surround_5_1:
    channels_ = 6;
    break;
  default:
    status_ = LoudnessStatus::invalid_configuration;
    return;
  }
  if (max_blocks == 0 || max_blocks > 864'000) {
    status_ = LoudnessStatus::invalid_configuration;
    return;
  }
  step_frames_ = sample_rate / 10;
  window_frames_ = step_frames_ * bs1770::momentary_steps;
  core_ = std::make_unique<Core>(max_blocks_);
  for (auto& filter : core_->weighting) {
    filter.prepare(static_cast<double>(sample_rate));
  }
  core_->windows.prepare(step_frames_);
}

IntegratedLoudnessAnalyzer::~IntegratedLoudnessAnalyzer() = default;
IntegratedLoudnessAnalyzer::IntegratedLoudnessAnalyzer(IntegratedLoudnessAnalyzer&&) noexcept =
    default;
IntegratedLoudnessAnalyzer&
IntegratedLoudnessAnalyzer::operator=(IntegratedLoudnessAnalyzer&&) noexcept = default;

void IntegratedLoudnessAnalyzer::reset() noexcept {
  if (status_ != LoudnessStatus::ok) {
    return;
  }
  for (auto& filter : core_->weighting) {
    filter.reset();
  }
  core_->windows.reset();
  core_->gate.clear();
  core_->latest_momentary_power = 0.0;
  frames_ = 0;
}

LoudnessStatus IntegratedLoudnessAnalyzer::append(std::span<const float> samples) {
  if (status_ != LoudnessStatus::ok) {
    return status_;
  }
  if (samples.size() % channels_ != 0 ||
      std::ranges::any_of(samples, [](float sample) { return !std::isfinite(sample); })) {
    return LoudnessStatus::malformed_input;
  }
  const auto incoming_frames = samples.size() / channels_;
  if (incoming_frames > std::numeric_limits<std::uint64_t>::max() - frames_) {
    return LoudnessStatus::capacity_exceeded;
  }
  const auto future_frames = frames_ + incoming_frames;
  const std::uint64_t future_blocks =
      future_frames < window_frames_ ? 0U : 1U + (future_frames - window_frames_) / step_frames_;
  if (future_blocks > max_blocks_) {
    return LoudnessStatus::capacity_exceeded;
  }

  auto& core = *core_;
  for (std::size_t frame = 0; frame < incoming_frames; ++frame) {
    double weighted_power = 0.0;
    for (std::size_t channel = 0; channel < channels_; ++channel) {
      if (layout_ == LoudnessLayout::surround_5_1 && channel == 3) {
        continue; // LFE is excluded from BS.1770-5 Annex 1.
      }
      const double k_weighted =
          core.weighting[channel].process(samples[frame * channels_ + channel]);
      const double weight = layout_ == LoudnessLayout::surround_5_1 && channel >= 4 ? 1.41 : 1.0;
      weighted_power += weight * k_weighted * k_weighted;
    }
    ++frames_;
    if (core.windows.add(weighted_power) && core.windows.steps() >= bs1770::momentary_steps) {
      core.latest_momentary_power = core.windows.momentary();
      // Capacity was checked for the whole append above.
      (void)core.gate.add(core.latest_momentary_power);
    }
  }
  return LoudnessStatus::ok;
}

LoudnessResult IntegratedLoudnessAnalyzer::finish() const noexcept {
  LoudnessResult result{.status = status_,
                        .integrated_lkfs = std::nullopt,
                        .momentary_lkfs = std::nullopt,
                        .short_term_lkfs = std::nullopt,
                        .total_blocks = 0,
                        .absolute_gated_blocks = 0,
                        .relative_gated_blocks = 0};
  if (status_ != LoudnessStatus::ok || core_->gate.blocks() == 0) {
    return result;
  }
  result.total_blocks = core_->gate.blocks();
  if (core_->latest_momentary_power > 0.0) {
    result.momentary_lkfs = bs1770::lkfs(core_->latest_momentary_power);
  }
  const double short_term = core_->windows.short_term();
  if (short_term > 0.0) {
    result.short_term_lkfs = bs1770::lkfs(short_term);
  }
  const auto gated = core_->gate.integrated();
  result.integrated_lkfs = gated.integrated_lkfs;
  result.absolute_gated_blocks = gated.absolute_gated_blocks;
  result.relative_gated_blocks = gated.relative_gated_blocks;
  return result;
}

} // namespace oscillate::native_dsp

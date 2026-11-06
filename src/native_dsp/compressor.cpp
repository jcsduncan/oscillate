#include "oscillate/native_dsp/compressor.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace oscillate::native_dsp {
namespace {

[[nodiscard]] bool within(double value, double lower, double upper) noexcept {
  return std::isfinite(value) && value >= lower && value <= upper;
}

[[nodiscard]] float finite_output(double sample) noexcept {
  constexpr double maximum = static_cast<double>(std::numeric_limits<float>::max());
  return std::isfinite(sample) ? static_cast<float>(std::clamp(sample, -maximum, maximum)) : 0.0F;
}

[[nodiscard]] double reduction_db(double detector_db, const CompressorSettings& settings) noexcept {
  const double over = detector_db - settings.threshold_db;
  const double slope = 1.0 - 1.0 / settings.ratio;
  const double half_knee = settings.knee_db * 0.5;
  if (over <= -half_knee) {
    return 0.0;
  }
  if (over >= half_knee || settings.knee_db == 0.0) {
    return slope * std::max(over, 0.0);
  }
  const double knee_position = over + half_knee;
  return slope * knee_position * knee_position / (2.0 * settings.knee_db);
}

} // namespace

double compressor_reduction_db(double detector_db, const CompressorSettings& settings) noexcept {
  return reduction_db(detector_db, settings);
}

foundation::Result<PreparedCompressor> prepare_compressor(CompressorSettings settings,
                                                          std::uint32_t sample_rate) {
  using foundation::Error;
  using foundation::ErrorCode;
  if (sample_rate < 44'100 || sample_rate > 192'000 || !within(settings.threshold_db, -80.0, 0.0) ||
      !within(settings.ratio, 1.0, 20.0) || !within(settings.knee_db, 0.0, 24.0) ||
      !within(settings.attack_ms, 0.1, 200.0) || !within(settings.release_ms, 1.0, 2'000.0) ||
      !within(settings.makeup_db, -24.0, 24.0) ||
      (settings.sidechain_highpass_hz != 0.0 &&
       !within(settings.sidechain_highpass_hz, 20.0, 2'000.0))) {
    return foundation::Result<PreparedCompressor>::failure(Error{
        ErrorCode::invalid_argument, "compressor controls or sample rate are outside bounds"});
  }
  const auto coefficient = [sample_rate](double milliseconds) {
    return std::exp(-1'000.0 / (milliseconds * static_cast<double>(sample_rate)));
  };
  return foundation::Result<PreparedCompressor>::success(
      PreparedCompressor(settings, sample_rate,
                         {coefficient(settings.attack_ms), coefficient(settings.release_ms),
                          oscillate::native_dsp::dsp::db_to_gain(settings.makeup_db),
                          settings.sidechain_highpass_hz == 0.0
                              ? 0.0
                              : static_cast<double>(sample_rate) /
                                    (static_cast<double>(sample_rate) +
                                     2.0 * std::numbers::pi * settings.sidechain_highpass_hz)}));
}

bool CompressorRuntime::offer_prepared(PreparedCompressor prepared,
                                       std::uint64_t revision) noexcept {
  if (prepared.sample_rate() != sample_rate_ || revision == 0 || revision <= offered_revision_)
    return false;
  const auto write = pending_write_.load(std::memory_order_relaxed);
  if (write - pending_read_.load(std::memory_order_acquire) >= mailbox_capacity)
    return false;
  pending_[write % mailbox_capacity].emplace(PendingCoefficients{prepared, revision});
  offered_revision_ = revision;
  pending_write_.store(write + 1, std::memory_order_release);
  return true;
}

void CompressorRuntime::adopt_pending() noexcept {
  // Finish one short gain transition before taking a newer offered value.
  // The bounded mailbox retains newer revisions in the meantime.
  if (transition_remaining_ != 0)
    return;
  const auto read = pending_read_.load(std::memory_order_relaxed);
  const auto write = pending_write_.load(std::memory_order_acquire);
  if (read == write)
    return;
  const auto& newest = pending_[(write - 1) % mailbox_capacity];
  if (!newest)
    return;
  fading_from_ = prepared_;
  transition_remaining_ = gain_transition_frames;
  prepared_ = newest->prepared;
  applied_revision_.store(newest->revision, std::memory_order_release);
  pending_read_.store(write, std::memory_order_release);
}

double CompressorRuntime::reduction_gain_for(double envelope,
                                             const PreparedCompressor& prepared) noexcept {
  const double slope = 1.0 - 1.0 / prepared.settings_.ratio;
  if (slope == 0.0 || envelope <= 0.0)
    return 1.0;
  const double threshold = oscillate::native_dsp::dsp::db_to_gain(prepared.settings_.threshold_db);
  const double knee_floor = threshold * std::pow(10.0, -prepared.settings_.knee_db / 40.0);
  if (envelope <= knee_floor)
    return 1.0;
  const double knee_ceiling = threshold * std::pow(10.0, prepared.settings_.knee_db / 40.0);
  if (envelope >= knee_ceiling)
    return std::pow(envelope / threshold, -slope);
  const double detector_db = 20.0 * std::log10(envelope);
  return std::pow(10.0, -reduction_db(detector_db, prepared.settings_) / 20.0);
}

void CompressorRuntime::publish_reduction(double lowest_reduction_gain) noexcept {
  // One logarithm per block, never per sample.
  const double reduction = lowest_reduction_gain > 0.0 && lowest_reduction_gain < 1.0
                               ? -20.0 * std::log10(lowest_reduction_gain)
                               : 0.0;
  published_reduction_db_.store(std::isfinite(reduction) ? static_cast<float>(reduction) : 0.0F,
                                std::memory_order_release);
}

void CompressorRuntime::process(std::span<engine::StereoFrame> stereo,
                                engine::BlockContext context) noexcept {
  process_frames(stereo, stereo, context);
}

void CompressorRuntime::process_with_sidechain(std::span<engine::StereoFrame> stereo,
                                               std::span<const engine::StereoFrame> sidechain,
                                               engine::BlockContext context) noexcept {
  process_frames(stereo, sidechain, context);
}

void CompressorRuntime::process_frames(std::span<engine::StereoFrame> stereo,
                                       std::span<const engine::StereoFrame> sidechain,
                                       engine::BlockContext context) noexcept {
  if (stereo.size() != sidechain.size() ||
      (context.state_generation() != 0 && context.state_generation() < generation_)) {
    std::ranges::fill(stereo, engine::StereoFrame{});
    guarded_frames_ += std::min<std::uint64_t>(
        stereo.size(), std::numeric_limits<std::uint64_t>::max() - guarded_frames_);
    publish_reduction(1.0);
    return;
  }
  adopt_pending();
  if (context.state_generation() != 0 && context.state_generation() != generation_) {
    envelope_ = 0.0;
    sidechain_left_ = {};
    sidechain_right_ = {};
    generation_ = context.state_generation();
  }
  const double slope = 1.0 - 1.0 / prepared_.settings_.ratio;
  const double threshold_level =
      oscillate::native_dsp::dsp::db_to_gain(prepared_.settings_.threshold_db);
  const double knee_floor = threshold_level * std::pow(10.0, -prepared_.settings_.knee_db / 40.0);
  const double knee_ceiling = threshold_level * std::pow(10.0, prepared_.settings_.knee_db / 40.0);
  double lowest_reduction_gain = 1.0;
  for (std::size_t index = 0; index < stereo.size(); ++index) {
    const bool invalid =
        !std::isfinite(stereo[index].left) || !std::isfinite(stereo[index].right) ||
        !std::isfinite(sidechain[index].left) || !std::isfinite(sidechain[index].right);
    double sidechain_left = std::isfinite(sidechain[index].left) ? sidechain[index].left : 0.0;
    double sidechain_right = std::isfinite(sidechain[index].right) ? sidechain[index].right : 0.0;
    if (prepared_.sidechain_highpass_ != 0.0) {
      const auto filter = [coefficient = prepared_.sidechain_highpass_](
                              double input, HighPassState& state) noexcept {
        const double output = coefficient * (state.output + input - state.input);
        state.input = input;
        state.output = std::abs(output) < 1e-30 ? 0.0 : output;
        return state.output;
      };
      sidechain_left = filter(sidechain_left, sidechain_left_);
      sidechain_right = filter(sidechain_right, sidechain_right_);
    }
    const auto detector = std::max(std::abs(sidechain_left), std::abs(sidechain_right));
    const double coefficient = detector > envelope_ ? prepared_.attack_ : prepared_.release_;
    envelope_ = detector + coefficient * (envelope_ - detector);
    if (envelope_ < 1e-30 || !std::isfinite(envelope_)) {
      envelope_ = 0.0;
    }
    double reduction_gain = 1.0;
    if (slope != 0.0 && envelope_ > knee_floor) {
      if (envelope_ >= knee_ceiling) {
        // Above the knee, the dB transfer is (level/threshold_level)^(-slope).
        // exp/log evaluates that positive-base power faster than libm pow on
        // the measured x86-64 callback path; both retain the same envelope.
        reduction_gain = std::exp(-slope * std::log(envelope_ / threshold_level));
      } else {
        const double detector_db = 20.0 * std::log10(envelope_);
        const double reduction = reduction_db(detector_db, prepared_.settings_);
        reduction_gain = std::pow(10.0, -reduction / 20.0);
      }
    }
    double gain = prepared_.makeup_ * reduction_gain;
    if (transition_remaining_ != 0 && fading_from_) {
      const double previous_reduction_gain = reduction_gain_for(envelope_, *fading_from_);
      const double previous_gain = fading_from_->makeup_ * previous_reduction_gain;
      const double progress =
          static_cast<double>(gain_transition_frames - transition_remaining_ + 1) /
          static_cast<double>(gain_transition_frames);
      gain = previous_gain + progress * (gain - previous_gain);
      reduction_gain =
          previous_reduction_gain + progress * (reduction_gain - previous_reduction_gain);
      --transition_remaining_;
      if (transition_remaining_ == 0)
        fading_from_.reset();
    }
    lowest_reduction_gain = std::min(lowest_reduction_gain, reduction_gain);
    auto& frame = stereo[index];
    frame = {finite_output(static_cast<double>(frame.left) * gain),
             finite_output(static_cast<double>(frame.right) * gain)};
    if (invalid && guarded_frames_ != std::numeric_limits<std::uint64_t>::max()) {
      ++guarded_frames_;
    }
  }
  publish_reduction(lowest_reduction_gain);
}

void CompressorRuntime::reset_after_quiesce() noexcept {
  envelope_ = 0.0;
  sidechain_left_ = {};
  sidechain_right_ = {};
  generation_ = 0;
  guarded_frames_ = 0;
  pending_write_.store(0, std::memory_order_relaxed);
  pending_read_.store(0, std::memory_order_relaxed);
  applied_revision_.store(0, std::memory_order_relaxed);
  offered_revision_ = 0;
  fading_from_.reset();
  transition_remaining_ = 0;
  publish_reduction(1.0);
}

} // namespace oscillate::native_dsp

#include "oscillate/native_dsp/eq.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>

namespace oscillate::native_dsp {
namespace {

constexpr double denormal_floor = 1e-30;
constexpr std::uint64_t max_eq_tail_seconds = 10;
constexpr EqCoefficients identity{1.0, 0.0, 0.0, 0.0, 0.0};

[[nodiscard]] double finite_state(double value) noexcept {
  return std::isfinite(value) && std::abs(value) >= denormal_floor ? value : 0.0;
}

[[nodiscard]] float finite_sample(double value) noexcept {
  constexpr double maximum = static_cast<double>(std::numeric_limits<float>::max());
  if (!std::isfinite(value)) {
    return 0.0F;
  }
  return static_cast<float>(std::clamp(value, -maximum, maximum));
}

[[nodiscard]] bool valid_kind(EqBandKind kind) noexcept {
  switch (kind) {
  case EqBandKind::low_pass:
  case EqBandKind::high_pass:
  case EqBandKind::low_shelf:
  case EqBandKind::high_shelf:
  case EqBandKind::peaking:
    return true;
  }
  return false;
}

[[nodiscard]] EqCoefficients normalized(double b0, double b1, double b2, double a0, double a1,
                                        double a2) noexcept {
  return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

[[nodiscard]] EqCoefficients build_coefficients(const EqBandSettings& band,
                                                std::uint32_t sample_rate) noexcept {
  const double angle =
      2.0 * std::numbers::pi * band.frequency_hz / static_cast<double>(sample_rate);
  const double cosine = std::cos(angle);
  const double sine = std::sin(angle);
  const double alpha = sine / (2.0 * band.shape);
  if (band.kind == EqBandKind::low_pass) {
    return normalized((1.0 - cosine) / 2.0, 1.0 - cosine, (1.0 - cosine) / 2.0, 1.0 + alpha,
                      -2.0 * cosine, 1.0 - alpha);
  }
  if (band.kind == EqBandKind::high_pass) {
    return normalized((1.0 + cosine) / 2.0, -(1.0 + cosine), (1.0 + cosine) / 2.0, 1.0 + alpha,
                      -2.0 * cosine, 1.0 - alpha);
  }

  const double amplitude = std::pow(10.0, band.gain_db / 40.0);
  if (band.kind == EqBandKind::peaking) {
    return normalized(1.0 + alpha * amplitude, -2.0 * cosine, 1.0 - alpha * amplitude,
                      1.0 + alpha / amplitude, -2.0 * cosine, 1.0 - alpha / amplitude);
  }

  const double shelf_alpha =
      sine / 2.0 * std::sqrt((amplitude + 1.0 / amplitude) * (1.0 / band.shape - 1.0) + 2.0);
  const double root_term = 2.0 * std::sqrt(amplitude) * shelf_alpha;
  if (band.kind == EqBandKind::low_shelf) {
    return normalized(amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosine + root_term),
                      2.0 * amplitude * ((amplitude - 1.0) - (amplitude + 1.0) * cosine),
                      amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosine - root_term),
                      (amplitude + 1.0) + (amplitude - 1.0) * cosine + root_term,
                      -2.0 * ((amplitude - 1.0) + (amplitude + 1.0) * cosine),
                      (amplitude + 1.0) + (amplitude - 1.0) * cosine - root_term);
  }
  return normalized(amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosine + root_term),
                    -2.0 * amplitude * ((amplitude - 1.0) + (amplitude + 1.0) * cosine),
                    amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosine - root_term),
                    (amplitude + 1.0) - (amplitude - 1.0) * cosine + root_term,
                    2.0 * ((amplitude - 1.0) - (amplitude + 1.0) * cosine),
                    (amplitude + 1.0) - (amplitude - 1.0) * cosine - root_term);
}

[[nodiscard]] bool stable(const EqCoefficients& c) noexcept {
  // Jury stability conditions for 1 + a1*z^-1 + a2*z^-2.
  return std::isfinite(c.b0) && std::isfinite(c.b1) && std::isfinite(c.b2) && std::isfinite(c.a1) &&
         std::isfinite(c.a2) && 1.0 + c.a1 + c.a2 > 1e-12 && 1.0 - c.a1 + c.a2 > 1e-12 &&
         1.0 - c.a2 > 1e-12;
}

} // namespace

std::size_t eq_pass_stages(EqBandKind kind, double frequency_hz, double q, std::size_t stages,
                           std::span<EqBandSettings> out) noexcept {
  stages = std::clamp<std::size_t>(stages, 1, std::min(eq_max_pass_stages, out.size()));
  if (stages == 1) {
    out[0] = {kind, frequency_hz, q, 0.0};
    return 1;
  }
  // The poles of an order-2n Butterworth filter pair into n sections, the
  // k-th at angle (2k - 1) * pi / 4n from the real axis: Q = 1 / (2 cos).
  const double order = 2.0 * static_cast<double>(stages);
  for (std::size_t k = 0; k < stages; ++k) {
    const double angle = (2.0 * static_cast<double>(k) + 1.0) * std::numbers::pi / (2.0 * order);
    out[k] = {kind, frequency_hz, 1.0 / (2.0 * std::cos(angle)), 0.0};
  }
  auto& last = out[stages - 1];
  last.shape = std::clamp(last.shape * q / oscillate::native_dsp::dsp::butterworth_q, 0.1,
                          eq_pass_section_q_limit);
  return stages;
}

foundation::Result<PreparedEq> prepare_eq(std::span<const EqBandSettings> bands,
                                          std::uint32_t sample_rate) {
  using foundation::Error;
  using foundation::ErrorCode;
  if (sample_rate < 44'100 || sample_rate > 192'000 || bands.size() > eq_section_limit) {
    return foundation::Result<PreparedEq>::failure(
        Error{ErrorCode::invalid_argument, "EQ rate or band count is outside supported bounds"});
  }
  std::array<EqCoefficients, eq_section_limit> coefficients{};
  const double maximum_frequency =
      std::min(20'000.0, native_dsp::dsp::nyquist_guard * static_cast<double>(sample_rate));
  for (std::size_t index = 0; index < bands.size(); ++index) {
    const auto& band = bands[index];
    const bool pass_filter =
        band.kind == EqBandKind::low_pass || band.kind == EqBandKind::high_pass;
    if (!valid_kind(band.kind) || !std::isfinite(band.frequency_hz) || band.frequency_hz < 10.0 ||
        band.frequency_hz > maximum_frequency || !std::isfinite(band.shape) || band.shape < 0.1 ||
        band.shape > (pass_filter ? eq_pass_section_q_limit : 10.0) ||
        ((band.kind == EqBandKind::low_shelf || band.kind == EqBandKind::high_shelf) &&
         band.shape > 1.0) ||
        !std::isfinite(band.gain_db) || band.gain_db < -24.0 || band.gain_db > 24.0 ||
        (pass_filter && band.gain_db != 0.0)) {
      return foundation::Result<PreparedEq>::failure(
          Error{ErrorCode::invalid_argument, "EQ band contains invalid controls"});
    }
    coefficients[index] = build_coefficients(band, sample_rate);
    if (!stable(coefficients[index])) {
      return foundation::Result<PreparedEq>::failure(
          Error{ErrorCode::invalid_argument, "EQ band cannot be represented stably"});
    }
  }
  return foundation::Result<PreparedEq>::success(
      PreparedEq{coefficients, {.band_count = bands.size(), .sample_rate = sample_rate}});
}

foundation::Result<std::uint64_t> estimate_eq_tail_frames(const PreparedEq& prepared) {
  // Bound the response of each stable section to roughly -180 dB, allowing
  // additional settling for a cascade. Reject settings outside the finite
  // export budget instead of silently cutting the processor's tail.
  double worst_radius = 0.0;
  for (std::size_t index = 0; index < prepared.band_count(); ++index) {
    const auto c = prepared.coefficients(index);
    const double discriminant = c.a1 * c.a1 - 4.0 * c.a2;
    const double radius = discriminant < 0.0
                              ? std::sqrt(c.a2)
                              : std::max(std::abs((-c.a1 + std::sqrt(discriminant)) / 2.0),
                                         std::abs((-c.a1 - std::sqrt(discriminant)) / 2.0));
    worst_radius = std::max(worst_radius, radius);
  }
  if (!(worst_radius >= 0.0 && worst_radius < 1.0)) {
    return foundation::Result<std::uint64_t>::failure(
        {foundation::ErrorCode::invalid_argument, "Master EQ has an unstable tail"});
  }
  if (worst_radius == 0.0) {
    return foundation::Result<std::uint64_t>::success(prepared.band_count() * 2);
  }
  const double estimate =
      std::ceil(-21.0 / std::log(worst_radius)) * static_cast<double>(prepared.band_count() + 1) +
      256.0;
  const auto budget = static_cast<double>(prepared.sample_rate()) * max_eq_tail_seconds;
  if (!std::isfinite(estimate) || estimate > budget) {
    return foundation::Result<std::uint64_t>::failure(
        {foundation::ErrorCode::resource_exhausted,
         "Master EQ tail exceeds the ten-second WAV export budget"});
  }
  return foundation::Result<std::uint64_t>::success(static_cast<std::uint64_t>(estimate));
}

std::optional<double> eq_magnitude_db(const PreparedEq& prepared, double frequency_hz) noexcept {
  if (!std::isfinite(frequency_hz) || frequency_hz < 0.0 ||
      frequency_hz > static_cast<double>(prepared.sample_rate()) / 2.0 ||
      prepared.sample_rate() == 0)
    return std::nullopt;
  const auto angle = -2.0 * std::numbers::pi * frequency_hz / prepared.sample_rate();
  const auto z = std::polar(1.0, angle);
  const auto z2 = z * z;
  double total_db = 0.0;
  for (std::size_t band = 0; band < prepared.band_count(); ++band) {
    const auto& c = prepared.coefficients(band);
    const auto numerator = std::abs(c.b0 + c.b1 * z + c.b2 * z2);
    const auto denominator = std::abs(1.0 + c.a1 * z + c.a2 * z2);
    if (!std::isfinite(numerator) || !std::isfinite(denominator) || denominator <= 0.0)
      return std::nullopt;
    total_db += 20.0 * std::log10(std::max(numerator / denominator, 1e-12));
  }
  return std::isfinite(total_db) ? std::optional<double>{total_db} : std::nullopt;
}

bool EqRuntime::offer_prepared(PreparedEq prepared, std::uint64_t revision) noexcept {
  if (prepared.band_count() != acknowledged_band_count_.load(std::memory_order_acquire) ||
      prepared.sample_rate() != sample_rate_ || revision == 0 || revision <= offered_revision_ ||
      (structural_offered_revision_ != 0 &&
       applied_revision_.load(std::memory_order_acquire) < structural_offered_revision_))
    return false;
  const auto write = pending_write_.load(std::memory_order_relaxed);
  if (write - pending_read_.load(std::memory_order_acquire) >= mailbox_capacity)
    return false;
  pending_[write % mailbox_capacity].emplace(PendingCoefficients{prepared, revision, std::nullopt});
  offered_revision_ = revision;
  pending_write_.store(write + 1, std::memory_order_release);
  return true;
}

bool EqRuntime::offer_band_change(PreparedEq prepared, std::size_t changed_band,
                                  std::uint64_t revision) noexcept {
  const auto count = acknowledged_band_count_.load(std::memory_order_acquire);
  const bool append = prepared.band_count() == count + 1 && changed_band == count;
  const bool remove = count > 1 && prepared.band_count() + 1 == count && changed_band < count;
  if ((!append && !remove) || prepared.sample_rate() != sample_rate_ || revision == 0 ||
      revision <= offered_revision_ ||
      (structural_offered_revision_ != 0 &&
       applied_revision_.load(std::memory_order_acquire) < structural_offered_revision_) ||
      applied_revision_.load(std::memory_order_acquire) != offered_revision_)
    return false;
  const auto write = pending_write_.load(std::memory_order_relaxed);
  if (write != pending_read_.load(std::memory_order_acquire))
    return false;
  pending_[write % mailbox_capacity].emplace(PendingCoefficients{prepared, revision, changed_band});
  offered_revision_ = revision;
  structural_offered_revision_ = revision;
  pending_write_.store(write + 1, std::memory_order_release);
  return true;
}

void EqRuntime::adopt_pending() noexcept {
  const auto read = pending_read_.load(std::memory_order_relaxed);
  const auto write = pending_write_.load(std::memory_order_acquire);
  if (read == write)
    return;
  // A later control value supersedes intermediate knob positions. The
  // producer cannot reuse this slot until pending_read_ is released below.
  const auto& newest = pending_[(write - 1) % mailbox_capacity];
  if (!newest)
    return;
  structural_transition_ = newest->changed_band.has_value();
  removing_band_.reset();
  if (structural_transition_) {
    if (newest->prepared.band_count() == band_count_ + 1) {
      current_[band_count_] = identity;
      state_[band_count_] = {};
      ++band_count_;
    } else {
      removing_band_ = newest->changed_band;
    }
  }
  prepared_ = newest->prepared;
  transition_remaining_ = transition_frames;
  transition_revision_ = newest->revision;
  pending_read_.store(write, std::memory_order_release);
}

void EqRuntime::advance_transition() noexcept {
  if (transition_remaining_ == 0)
    return;
  // The Jury inequalities used in stable() are linear in a1/a2, so a
  // convex step between two admitted denominator pairs remains stable.
  const double fraction = 1.0 / static_cast<double>(transition_remaining_);
  for (std::size_t band = 0; band < band_count_; ++band) {
    auto& current = current_[band];
    const auto& target =
        removing_band_ && band >= *removing_band_
            ? (band == *removing_band_ ? identity : prepared_.coefficients(band - 1))
            : prepared_.coefficients(band);
    current.b0 += (target.b0 - current.b0) * fraction;
    current.b1 += (target.b1 - current.b1) * fraction;
    current.b2 += (target.b2 - current.b2) * fraction;
    current.a1 += (target.a1 - current.a1) * fraction;
    current.a2 += (target.a2 - current.a2) * fraction;
  }
  if (--transition_remaining_ == 0) {
    if (!structural_transition_) {
      for (std::size_t band = 0; band < band_count_; ++band)
        current_[band] = prepared_.coefficients(band);
      applied_revision_.store(transition_revision_, std::memory_order_release);
    }
  }
}

void EqRuntime::finish_structural_transition() noexcept {
  if (!structural_transition_ || transition_remaining_ != 0)
    return;
  if (removing_band_) {
    for (std::size_t band = *removing_band_; band + 1 < band_count_; ++band) {
      current_[band] = current_[band + 1];
      state_[band] = state_[band + 1];
    }
    --band_count_;
    state_[band_count_] = {};
  }
  for (std::size_t band = 0; band < band_count_; ++band)
    current_[band] = prepared_.coefficients(band);
  removing_band_.reset();
  structural_transition_ = false;
  acknowledged_band_count_.store(band_count_, std::memory_order_release);
  applied_revision_.store(transition_revision_, std::memory_order_release);
}

void EqRuntime::process(std::span<engine::StereoFrame> stereo,
                        engine::BlockContext context) noexcept {
  if (context.state_generation() != 0 && context.state_generation() < generation_) {
    std::ranges::fill(stereo, engine::StereoFrame{});
    return;
  }
  adopt_pending();
  if (context.state_generation() != 0 && context.state_generation() != generation_) {
    state_ = {};
    generation_ = context.state_generation();
  }
  for (auto& frame : stereo) {
    advance_transition();
    const bool bad_left = !std::isfinite(frame.left);
    const bool bad_right = !std::isfinite(frame.right);
    double left = bad_left ? 0.0 : static_cast<double>(frame.left);
    double right = bad_right ? 0.0 : static_cast<double>(frame.right);
    if (bad_left || bad_right) {
      for (std::size_t band = 0; band < band_count_; ++band) {
        if (bad_left) {
          state_[band][0] = {};
        }
        if (bad_right) {
          state_[band][1] = {};
        }
      }
    }
    for (std::size_t band = 0; band < band_count_; ++band) {
      const auto& c = current_[band];
      auto& l = state_[band][0];
      auto& r = state_[band][1];
      const double output_left = c.b0 * left + l.z1;
      const double output_right = c.b0 * right + r.z1;
      const double next_l1 = c.b1 * left - c.a1 * output_left + l.z2;
      const double next_r1 = c.b1 * right - c.a1 * output_right + r.z2;
      l.z2 = finite_state(c.b2 * left - c.a2 * output_left);
      r.z2 = finite_state(c.b2 * right - c.a2 * output_right);
      l.z1 = finite_state(next_l1);
      r.z1 = finite_state(next_r1);
      if (removing_band_ && band == *removing_band_) {
        const double fade =
            static_cast<double>(transition_remaining_) / static_cast<double>(transition_frames);
        l.z1 *= fade;
        l.z2 *= fade;
        r.z1 *= fade;
        r.z2 *= fade;
      }
      left = finite_state(output_left);
      right = finite_state(output_right);
    }
    if (bad_left || bad_right || !std::isfinite(left) || !std::isfinite(right) ||
        std::abs(left) > static_cast<double>(std::numeric_limits<float>::max()) ||
        std::abs(right) > static_cast<double>(std::numeric_limits<float>::max())) {
      if (guarded_frames_ != std::numeric_limits<std::uint64_t>::max()) {
        ++guarded_frames_;
      }
    }
    frame = {finite_sample(left), finite_sample(right)};
    finish_structural_transition();
  }
}

void EqRuntime::reset_after_quiesce() noexcept {
  state_ = {};
  band_count_ = prepared_.band_count();
  acknowledged_band_count_.store(band_count_, std::memory_order_relaxed);
  generation_ = 0;
  guarded_frames_ = 0;
  transition_remaining_ = 0;
  transition_revision_ = 0;
  structural_transition_ = false;
  removing_band_.reset();
  for (std::size_t band = 0; band < band_count_; ++band)
    current_[band] = prepared_.coefficients(band);
  pending_write_.store(0, std::memory_order_relaxed);
  pending_read_.store(0, std::memory_order_relaxed);
  applied_revision_.store(0, std::memory_order_relaxed);
  offered_revision_ = 0;
  structural_offered_revision_ = 0;
}

} // namespace oscillate::native_dsp

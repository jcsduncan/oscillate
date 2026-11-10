#include "oscillate/native_dsp/rebalance.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <cmath>

namespace oscillate::native_dsp {
namespace {

// prepare_eq's frequency range.
constexpr double lowest_hz = 10.0;
constexpr double highest_hz = 20'000.0;
constexpr double decibels_per_gain_decade = 20.0;
constexpr double decade = 10.0;

[[nodiscard]] double in_band(double hz, double sample_rate) noexcept {
  return std::clamp(hz, lowest_hz, std::min(highest_hz, dsp::nyquist_guard * sample_rate));
}

} // namespace

std::array<EqBandSettings, rebalance_bands> rebalance_bands_for(const RoleTone& tone,
                                                                double sample_rate) {
  return {{
      {EqBandKind::low_shelf, in_band(tone.low_hz, sample_rate), rebalance_shelf_slope,
       tone.low_db},
      {EqBandKind::peaking, in_band(tone.mid_hz, sample_rate), tone.mid_q, tone.mid_db},
      {EqBandKind::high_shelf, in_band(tone.high_hz, sample_rate), rebalance_shelf_slope,
       tone.high_db},
  }};
}

foundation::Result<RoleTarget> role_target(const RoleTone& tone, std::uint32_t sample_rate) {
  using R = foundation::Result<RoleTarget>;
  const auto bands = rebalance_bands_for(tone, static_cast<double>(sample_rate));
  auto flat_bands = bands;
  for (auto& band : flat_bands)
    band.gain_db = 0.0;
  auto eq = prepare_eq(bands, sample_rate);
  if (!eq)
    return R::failure(eq.error());
  auto flat = prepare_eq(flat_bands, sample_rate);
  if (!flat)
    return R::failure(flat.error());
  const bool flat_eq = tone.low_db == 0.0 && tone.mid_db == 0.0 && tone.high_db == 0.0;
  const auto gain = static_cast<float>(std::pow(decade, tone.gain_db / decibels_per_gain_decade));
  // How long the flat cascade takes to ring out what the EQ left in it.
  auto settle = estimate_eq_tail_frames(flat.value());
  if (!settle)
    return R::failure(settle.error());
  // How long the EQ keeps sounding after its input stops (export tails).
  std::uint64_t tail = 0;
  if (!flat_eq) {
    auto ring = estimate_eq_tail_frames(eq.value());
    if (!ring)
      return R::failure(ring.error());
    tail = ring.value();
  }
  return R::success(RoleTarget{eq.value(), flat.value(), gain, flat_eq, flat_eq && gain == 1.0F,
                               settle.value(), tail});
}

foundation::Result<RoleTarget> neutral_target(std::uint32_t sample_rate) {
  // Any frequencies in range: a neutral stage never runs its EQ.
  constexpr double low_hz = 100.0;
  constexpr double mid_hz = 1'000.0;
  constexpr double high_hz = 8'000.0;
  constexpr double mid_q = 0.7;
  return role_target({0.0, 0.0, low_hz, 0.0, mid_hz, mid_q, 0.0, high_hz}, sample_rate);
}

RoleStage::RoleStage(const RoleTarget& initial) noexcept
    : buffers_{initial, initial, initial}, gain_(initial.gain), gain_from_(initial.gain) {
  if (!initial.flat_eq)
    eq_.emplace(initial.eq);
}

void RoleStage::offer(const RoleTarget& target) noexcept {
  buffers_[back_] = target;
  const auto previous =
      middle_.exchange(static_cast<std::uint8_t>(back_ | fresh_bit), std::memory_order_acq_rel);
  back_ = static_cast<std::uint8_t>(previous & index_mask);
}

void RoleStage::adopt() noexcept {
  if ((middle_.load(std::memory_order_acquire) & fresh_bit) == 0)
    return;
  const auto previous = middle_.exchange(front_, std::memory_order_acq_rel);
  front_ = static_cast<std::uint8_t>(previous & index_mask);
  const auto& target = buffers_[front_];
  // The gain glides from wherever it is now.
  float now = gain_;
  if (gliding())
    now = gain_from_ + (gain_ - gain_from_) * static_cast<float>(glide_done_) /
                           static_cast<float>(rebalance_gain_glide_frames);
  if (target.gain != gain_ || gliding()) {
    gain_from_ = now;
    gain_ = target.gain;
    glide_done_ = 0;
  }
  pending_ = true;
}

void RoleStage::process(std::span<engine::StereoFrame> stereo,
                        engine::BlockContext context) noexcept {
  adopt();
  const auto& target = buffers_[front_];
  if (pending_) {
    // A bypassed stage starts from the same bands at 0 dB, which pass its
    // block as it is, so the glide starts without a step.
    if (!eq_ && !target.flat_eq)
      eq_.emplace(target.flat);
    if (!eq_ || eq_->offer_prepared(target.eq, offered_ + 1)) {
      if (eq_)
        ++offered_;
      pending_ = false;
    }
  }
  if (eq_)
    eq_->process(stereo, context);
  if (gliding()) {
    for (auto& frame : stereo) {
      float gain = gain_;
      if (gliding()) {
        ++glide_done_;
        gain = gain_from_ + (gain_ - gain_from_) * static_cast<float>(glide_done_) /
                                static_cast<float>(rebalance_gain_glide_frames);
      }
      frame = {frame.left * gain, frame.right * gain};
    }
  } else if (gain_ != 1.0F) {
    for (auto& frame : stereo)
      frame = {frame.left * gain_, frame.right * gain_};
  }
  // At a flat EQ, once what the earlier bands left in the cascade has rung
  // out: stop running it, so a stage at 0 dB is exact.
  if (eq_ && !pending_ && target.flat_eq && eq_->applied_prepared_revision() == offered_) {
    settled_ += stereo.size();
    if (settled_ >= target.settle_frames)
      eq_.reset();
  } else {
    settled_ = 0;
  }
}

} // namespace oscillate::native_dsp

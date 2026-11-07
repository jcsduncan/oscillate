#include "effects/dust/vinyl.hpp"

#include "effects/dust/parts.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace oscillate::native_dsp::effects::dust::detail {
namespace {

// Salt that keeps the record's scratches their own sequence.
constexpr std::uint64_t record_salt = 0x7EC0D5ULL;

[[nodiscard]] PopShape pop_of_size(double amplitude, double tone, double rate, Rng& rng) noexcept {
  PopShape pop;
  pop.amplitude = amplitude;
  pop.size = std::log(std::max(amplitude, 1.0)) / std::log(vinyl_pop_range);
  pop.pulse_frames = static_cast<std::uint32_t>(
      std::max(1L, std::lround((vinyl_pulse_min_s + vinyl_pulse_span_s * pop.size) * rate)));
  pop.ring_hz = vinyl_ring_low_hz * std::pow(vinyl_ring_ratio, rng.unit()) *
                (vinyl_ring_tone_low + vinyl_ring_tone_span * tone) *
                (1.0 - vinyl_ring_size_drop * pop.size);
  pop.ring_seconds = vinyl_ring_min_s + vinyl_ring_span_s * pop.size;
  pop.polarity = rng.sign();
  pop.pan = rng.bipolar();
  return pop;
}

} // namespace

PopShape draw_pop(Rng& rng, double alpha, double tone, double rate) noexcept {
  const double amplitude = std::min(std::pow(rng.open_unit(), -1.0 / alpha), vinyl_pop_range);
  return pop_of_size(amplitude, tone, rate, rng);
}

void Vinyl::prepare(LayerFormat format) {
  const double rate = format.rate;
  const double reference = format.reference;
  rate_ = rate;
  reference_ = reference;
  for (auto& f : floor_hp_) {
    f.set(vinyl_floor_hp_hz, butterworth_q, rate);
  }
  pop_unit_ = gain_of(vinyl_pop_smallest_dbfs);
  rumble_arm_.set(vinyl_arm_hz, vinyl_arm_q, rate);
  rumble_sub_.set(vinyl_rumble_sub_hz, rate);
}

void Vinyl::press(std::uint64_t seed_key) noexcept {
  Rng rng;
  rng.seed(seed_key ^ record_salt);
  for (auto& scratch : scratches_) {
    scratch.angle = rng.unit();
    scratch.strength = vinyl_scratch_strength_min + (1.0 - vinyl_scratch_strength_min) * rng.unit();
    scratch.hz = vinyl_scratch_low_hz * std::pow(vinyl_scratch_ratio, rng.unit());
    scratch.polarity = rng.sign();
    scratch.pan = rng.bipolar();
  }
}

void Vinyl::reset() noexcept {
  for (auto& f : surface_high_) {
    f.reset();
  }
  for (auto& f : surface_low_) {
    f.reset();
  }
  for (auto& f : floor_hp_) {
    f.reset();
  }
  for (auto& f : floor_lp_) {
    f.reset();
  }
  for (auto& voice : voices_) {
    voice = PopVoice{};
  }
  rumble_low_.reset();
  rumble_arm_.reset();
  rumble_sub_.reset();
  last_rotation_ = -1.0;
}

void Vinyl::set(std::span<const double> v, LayerShape shape) noexcept {
  const double tone = shape.tone;
  const double width = shape.width;
  const double crackle = v[p::vinyl_crackle];
  const double wear = v[p::vinyl_wear];
  const double rumble = v[p::vinyl_rumble];
  pop_chance_ =
      pop_rate_max * (std::pow(pop_rate_span, crackle) - 1.0) / (pop_rate_span - 1.0) / rate_;
  alpha_ = vinyl_alpha_fine - vinyl_alpha_span * v[p::vinyl_dust];
  tone_ = tone;
  floor_chance_ = (vinyl_floor_rate_min + vinyl_floor_rate_span * crackle) / rate_;
  if (tone != surface_tone_) {
    surface_tone_ = tone;
    const double low = vinyl_surface_low_hz * std::pow(vinyl_surface_ratio, tone);
    for (auto& f : surface_high_) {
      f.set(vinyl_surface_hp_hz, rate_);
    }
    for (auto& f : surface_low_) {
      f.set(low, vinyl_surface_q, rate_);
    }
    const std::array chain{high1(vinyl_surface_hp_hz), low2(low, vinyl_surface_q)};
    surface_power_ = noise_power(chain, rate_);
    const double floor_low =
        std::min(vinyl_floor_low_hz * std::pow(vinyl_floor_ratio, tone), nyquist_guard * rate_);
    for (auto& f : floor_lp_) {
      f.set(floor_low, butterworth_q, rate_);
    }
    const std::array floor_chain{high2(vinyl_floor_hp_hz, butterworth_q),
                                 low2(floor_low, butterworth_q)};
    floor_power_ = noise_power(floor_chain, rate_);
    const double rumble_hz = vinyl_rumble_low_hz + vinyl_rumble_span_hz * tone;
    rumble_low_.set(rumble_hz, vinyl_rumble_q, rate_);
    const std::array rumble_chain{high1(vinyl_rumble_sub_hz), low2(rumble_hz, vinyl_rumble_q)};
    rumble_power_ = noise_power(rumble_chain, rate_);
  }
  const double floor_target =
      reference_ *
      gain_of(vinyl_floor_db + vinyl_floor_slope_db * (crackle - def(p::vinyl_crackle)));
  floor_amp_ = floor_target / std::sqrt(floor_chance_ * vinyl_floor_square_mean * floor_power_);
  surface_gain_ = reference_ *
                  gain_of(vinyl_surface_db + vinyl_surface_wear_db * (wear - def(p::vinyl_wear))) /
                  std::sqrt(surface_power_);
  rumble_gain_ = rumble <= 0.0 ? 0.0
                               : reference_ * gain_of(vinyl_rumble_db) *
                                     std::pow(rumble / def(p::vinyl_rumble), vinyl_rumble_law) /
                                     std::sqrt(rumble_power_);
  audible_scratches_ = 0;
  for (std::size_t i = 0; i < vinyl_scratch_count; ++i) {
    if (wear >= vinyl_scratch_first + vinyl_scratch_step * static_cast<double>(i)) {
      audible_scratches_ = i + 1;
    }
  }
  scratch_scale_ = pop_unit_ * vinyl_scratch_size * (vinyl_scratch_base + wear);
  drive_ = 1.0 + vinyl_wear_drive * wear * wear;
  width_.set(width);
  place_ = vinyl_pop_place * width;
}

void Vinyl::start(const PopShape& shape, double scale) noexcept {
  // A free voice, else the one nearest its end.
  PopVoice* chosen = &voices_[0];
  for (auto& voice : voices_) {
    if (voice.life < chosen->life) {
      chosen = &voice;
    }
  }
  chosen->start(shape, scale, place_, rate_);
  ++pops_;
}

void Vinyl::scratch(const Scratch& s) noexcept {
  const double jitter = 1.0 - 0.5 * vinyl_scratch_jitter + vinyl_scratch_jitter * rng_.unit();
  PopShape shape = pop_of_size(s.strength * jitter * vinyl_scratch_size, tone_, rate_, rng_);
  shape.ring_hz = s.hz;
  shape.polarity = s.polarity;
  shape.pan = s.pan;
  shape.amplitude = s.strength * jitter;
  start(shape, scratch_scale_);
}

} // namespace oscillate::native_dsp::effects::dust::detail

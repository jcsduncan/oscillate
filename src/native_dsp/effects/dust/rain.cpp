#include "effects/dust/rain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace oscillate::native_dsp::effects::dust::detail {

DropShape draw_drop(Rng& rng, const SurfaceShape& surface, double intensity, double tone) noexcept {
  DropShape drop;
  const double mean = rain_radius_min_mm + rain_radius_span_mm * intensity;
  const double radius =
      std::clamp(-mean * std::log(rng.open_unit()), rain_radius_low_mm, rain_radius_high_mm);
  drop.amplitude = surface.level * std::pow(radius, rain_size_law);
  drop.tick_seconds = surface.length_scale * (rain_tick_min_s + rain_tick_per_mm_s * radius);
  const double colour = rain_tone_low + rain_tone_span * tone;
  drop.hz = surface.low_hz * std::pow(surface.ratio, rng.unit()) * colour;
  drop.q = surface.q_min + surface.q_span * rng.unit();
  if (rng.unit() < surface.bubbles) {
    drop.bubble = rain_bubble_level * drop.amplitude;
    drop.bubble_hz =
        std::min(rain_minnaert_hz_mm /
                     (radius * (rain_bubble_radius_low + rain_bubble_radius_span * rng.unit())),
                 rain_bubble_max_hz);
    drop.bubble_rise = rain_bubble_rise_min + rain_bubble_rise_span * rng.unit();
    drop.bubble_seconds = rain_bubble_min_s + rain_bubble_per_mm_s * radius;
  }
  drop.pan = rng.bipolar();
  return drop;
}

double drop_energy(const DropShape& drop, const SurfaceShape& surface, double rate) noexcept {
  const double tick_power = band_power(std::min(drop.hz, nyquist_guard * rate), drop.q, rate) +
                            surface.direct * surface.direct * pi * surface.tick_hz / rate;
  return drop.amplitude * drop.amplitude * drop.tick_seconds / 2.0 * tick_power +
         drop.bubble * drop.bubble * drop.bubble_seconds / 4.0;
}

void DropVoice::start(const DropShape& drop, const SurfaceShape& surface, double scale,
                      double place, double rate) noexcept {
  envelope = drop.amplitude * scale;
  decay = std::exp(-1.0 / (drop.tick_seconds * rate));
  direct = surface.direct;
  band.reset();
  band.set(drop.hz, drop.q, rate);
  tick.reset();
  tick.set(surface.tick_hz, rate);
  double longest = std::max(drop.tick_seconds, drop.q / (pi * drop.hz));
  bubble = drop.bubble * scale;
  if (bubble > 0.0) {
    const double w = two_pi * std::min(drop.bubble_hz, nyquist_guard * rate) / rate;
    re = 1.0;
    im = 0.0;
    cr = std::cos(w);
    ci = std::sin(w);
    const double dw = w * drop.bubble_rise / (rain_bubble_rise_taus * drop.bubble_seconds * rate);
    dr = std::cos(dw);
    di = std::sin(dw);
    bubble_decay = std::exp(-1.0 / (drop.bubble_seconds * rate));
    longest = std::max(longest, drop.bubble_seconds);
  }
  pan = pan_gains(drop.pan * place);
  life = static_cast<std::uint32_t>(std::ceil(voice_life_tau * longest * rate));
}

void Rain::prepare(double rate, double reference) {
  rate_ = rate;
  reference_ = reference;
  // Each surface's drops: their expected power at the default Intensity
  // and Tone.
  const double intensity = def(p::rain_intensity);
  for (std::size_t s = 0; s < rain_surfaces.size(); ++s) {
    const auto& surface = rain_surfaces[s];
    Rng rng;
    rng.seed(calibration_seed + s);
    double energy = 0.0;
    for (std::size_t i = 0; i < calibration_draws; ++i) {
      energy += drop_energy(draw_drop(rng, surface, intensity, def(p::rain_tone)), surface, rate);
    }
    energy /= static_cast<double>(calibration_draws);
    const double drops_a_second =
        surface.rate_scale * rain_rate_min * std::pow(rain_rate_span, intensity);
    drop_scale_[s] = reference * gain_of(rain_drops_db) / std::sqrt(drops_a_second * energy);
  }
}

void Rain::reset() noexcept {
  for (auto& voice : voices_) {
    voice = DropVoice{};
  }
  for (std::size_t c = 0; c < channels; ++c) {
    wash_high_[c].reset();
    wash_band_[c].reset();
    wash_low_[c].reset();
    distance_[c].reset();
  }
  swell_.reset();
}

void Rain::set(std::span<const double> v, double tone, double width) noexcept {
  const auto surface = choice_of(v[p::rain_surface], rain_surfaces.size());
  intensity_ = v[p::rain_intensity];
  tone_ = tone;
  const double distance = v[p::rain_distance];
  if (surface != surface_ || tone != wash_tone_) {
    surface_ = surface;
    wash_tone_ = tone;
    const double centre = rain_surfaces[surface].wash_hz * (rain_tone_low + rain_tone_span * tone);
    const double low = std::min(centre * rain_wash_lp_ratio, nyquist_guard * rate_);
    for (std::size_t c = 0; c < channels; ++c) {
      wash_high_[c].set(rain_wash_hp_hz, rate_);
      wash_band_[c].set(centre, rain_wash_q, rate_);
      wash_low_[c].set(low, butterworth_q, rate_);
    }
    const std::array chain{high1(rain_wash_hp_hz), band2(centre, rain_wash_q),
                           low2(low, butterworth_q)};
    wash_power_ = noise_power(chain, rate_);
  }
  if (distance != distance_set_) {
    distance_set_ = distance;
    for (auto& f : distance_) {
      f.set(rain_near_hz * std::pow(rain_far_ratio, distance), butterworth_q, rate_);
    }
  }
  const auto& shape = rain_surfaces[surface_];
  drop_chance_ = shape.rate_scale * rain_rate_min * std::pow(rain_rate_span, intensity_) / rate_;
  wash_gain_ = reference_ *
               gain_of(rain_wash_db + rain_wash_slope_db * (intensity_ - def(p::rain_intensity))) /
               std::sqrt(wash_power_);
  direct_ = (1.0 - rain_room_share * distance) / (1.0 - rain_room_share * def(p::rain_distance));
  send_ = rain_room_send * distance;
  width_.set(width);
  place_ = width;
}

void Rain::start(const DropShape& drop, const SurfaceShape& surface) noexcept {
  DropVoice* chosen = &voices_[0];
  for (auto& voice : voices_) {
    if (voice.life < chosen->life) {
      chosen = &voice;
    }
  }
  chosen->start(drop, surface, drop_scale_[surface_], place_, rate_);
}

} // namespace oscillate::native_dsp::effects::dust::detail

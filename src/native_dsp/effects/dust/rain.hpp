#pragma once

#include "effects/dust/parts.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

// Dust's rain layer (ADR-0069): drops on a surface (ticks through its
// resonance, Minnaert bubbles), the wash of far drops, and distance. The
// kernel sends some of it to a room (dust.cpp).
namespace oscillate::native_dsp::effects::dust::detail {

// Drops: Poisson, rain_rate_min .. rain_rate_min * rain_rate_span a second
// with Intensity (doubled on leaves), radii exponential about a mean that
// grows with Intensity. Each drop is a noise tick through its surface's
// resonance plus a little of it direct, and sometimes a Minnaert bubble: a
// decaying sine at rain_minnaert_hz_mm / radius whose pitch rises as it
// rings. Under them, the wash of countless far drops.
inline constexpr double rain_rate_min = 15.0;
inline constexpr double rain_rate_span = 60.0;
inline constexpr double rain_radius_min_mm = 0.5;
inline constexpr double rain_radius_span_mm = 1.0;
inline constexpr double rain_radius_low_mm = 0.2;
inline constexpr double rain_radius_high_mm = 3.0;
inline constexpr double rain_size_law = 1.5;
inline constexpr double rain_tick_min_s = 0.4e-3;
inline constexpr double rain_tick_per_mm_s = 0.5e-3;
inline constexpr double rain_minnaert_hz_mm = 3'260.0;
inline constexpr double rain_bubble_rise_min = 0.1;
inline constexpr double rain_bubble_rise_span = 0.25;
inline constexpr double rain_bubble_min_s = 3e-3;
inline constexpr double rain_bubble_per_mm_s = 6e-3;
inline constexpr double rain_bubble_level = 0.5;
inline constexpr double rain_bubble_max_hz = 9'000.0;
inline constexpr double rain_tone_low = 0.8;
inline constexpr double rain_tone_span = 0.4;
inline constexpr std::size_t rain_voices = 32;
struct SurfaceShape {
  double low_hz; // The resonance's range, low ..
  double ratio;  // .. low * ratio
  double q_min;
  double q_span;
  double direct; // The tick heard directly, through tick_hz.
  double tick_hz;
  double bubbles;      // The share of drops that bubble.
  double rate_scale;   // Drops a second against the others.
  double level;        // Each drop against the others.
  double length_scale; // Tick length against the others.
  double wash_hz;      // The wash's centre.
};
// Glass: bright ringing ticks; Roof: dull thuds with a resonant body;
// Leaves: soft, dense and filtered.
inline constexpr std::array<SurfaceShape, 3> rain_surfaces{{
    {2'500.0, 2.8, 5.0, 6.0, 0.25, 9'000.0, 0.3, 1.0, 1.0, 1.0, 3'500.0},
    {220.0, 3.0, 6.0, 4.0, 0.08, 2'500.0, 0.12, 1.0, 1.0, 1.0, 1'500.0},
    {900.0, 3.0, 0.9, 0.4, 0.5, 3'500.0, 0.0, 2.0, 0.6, 1.5, 2'200.0},
}};
// The wash: a high-pass, the surface's band and a low-pass.
inline constexpr double rain_wash_hp_hz = 400.0;
inline constexpr double rain_wash_q = 0.5;
inline constexpr double rain_wash_lp_ratio = 4.0;
inline constexpr double rain_wash_db = -5.0;
inline constexpr double rain_wash_slope_db = 10.0;
inline constexpr double rain_wash_swell = 0.15;
inline constexpr double rain_wash_swell_seconds = 1.5;
inline constexpr double rain_drops_db = -2.5;
// Distance: a low-pass from rain_near_hz down by rain_far_ratio, and more
// heard through a room (rain_room_share of the direct moves to the room).
inline constexpr double rain_near_hz = 18'000.0;
inline constexpr double rain_far_ratio = 0.12;
inline constexpr double rain_room_share = 0.5;
inline constexpr double rain_room_send = 0.8;
// A bubbling drop's radius is scaled by rain_bubble_radius_low ..
// rain_bubble_radius_low + rain_bubble_radius_span.
inline constexpr double rain_bubble_radius_low = 0.5;
inline constexpr double rain_bubble_radius_span = 0.5;
// The pitch rises by bubble_rise over about this many time constants.
inline constexpr double rain_bubble_rise_taus = 3.0;

struct DropShape {
  double amplitude = 1.0;
  double tick_seconds = rain_tick_min_s;
  double hz = 1'000.0;
  double q = 1.0;
  double bubble = 0.0; // amplitude, 0 for none
  double bubble_hz = rain_minnaert_hz_mm;
  double bubble_rise = 0.0;
  double bubble_seconds = rain_bubble_min_s;
  double pan = 0.0;
};

[[nodiscard]] DropShape draw_drop(Rng& rng, const SurfaceShape& surface, double intensity,
                                  double tone) noexcept;

// A drop's energy in seconds (power x time), the tick as a noise burst
// through its band and the direct path, the bubble as a decaying sine.
[[nodiscard]] double drop_energy(const DropShape& drop, const SurfaceShape& surface,
                                 double rate) noexcept;

struct DropVoice {
  std::uint32_t life = 0;
  double envelope = 0.0;
  double decay = 0.0;
  double direct = 0.0;
  Svf band;
  OnePole tick;
  double re = 0.0;
  double im = 0.0;
  double cr = 1.0;
  double ci = 0.0;
  double dr = 1.0;
  double di = 0.0;
  double bubble = 0.0;
  double bubble_decay = 0.0;
  Pair pan{1.0, 1.0};

  void start(const DropShape& drop, const SurfaceShape& surface, double scale, double place,
             double rate) noexcept;
  [[nodiscard]] double next(Rng& rng) noexcept {
    const double x = rng.white() * envelope;
    envelope *= decay;
    double y = band.band(x) + direct * tick.low(x);
    if (bubble > 0.0) {
      const double r = re * cr - im * ci;
      im = re * ci + im * cr;
      re = r;
      const double c = cr * dr - ci * di;
      ci = cr * di + ci * dr;
      cr = c;
      y += bubble * im;
      bubble *= bubble_decay;
    }
    --life;
    return y;
  }
};

class Rain {
public:
  void prepare(double rate, double reference);
  void reseed(std::uint64_t key) noexcept { rng_.seed(key); }
  void reset() noexcept;
  void set(std::span<const double> v, double tone, double width) noexcept;
  [[nodiscard]] Pair next() noexcept {
    const auto& shape = rain_surfaces[surface_];
    if (rng_.unit() < drop_chance_) {
      start(draw_drop(rng_, shape, intensity_, tone_), shape);
    }
    const double swell = 1.0 + rain_wash_swell * swell_.next(rng_, rain_wash_swell_seconds, rate_);
    const double a = wash_low_[0].low(wash_band_[0].band(wash_high_[0].high(rng_.white())));
    const double b = wash_low_[1].low(wash_band_[1].band(wash_high_[1].high(rng_.white())));
    Pair out = width_.place(a * wash_gain_ * swell, b * wash_gain_ * swell);
    for (auto& voice : voices_) {
      if (voice.life > 0) {
        const double y = voice.next(rng_);
        out.left += y * voice.pan.left;
        out.right += y * voice.pan.right;
      }
    }
    return {distance_[0].low(out.left) * direct_, distance_[1].low(out.right) * direct_};
  }
  // The room's share: how much of the rain the room hears.
  [[nodiscard]] double send() const noexcept { return send_; }

private:
  void start(const DropShape& drop, const SurfaceShape& surface) noexcept;

  double rate_ = nominal_rate;
  double reference_ = 0.0;
  Rng rng_;
  std::array<DropVoice, rain_voices> voices_{};
  std::array<double, rain_surfaces.size()> drop_scale_{};
  std::array<OnePole, 2> wash_high_{};
  std::array<Svf, 2> wash_band_{};
  std::array<Svf, 2> wash_low_{};
  std::array<Svf, 2> distance_{};
  Drift swell_;
  Width width_;
  std::size_t surface_ = 1;
  double intensity_ = 0.5;
  double tone_ = 0.5;
  double wash_tone_ = -1.0;
  double wash_power_ = 1.0;
  double wash_gain_ = 0.0;
  double distance_set_ = -1.0;
  double drop_chance_ = 0.0;
  double direct_ = 1.0;
  double send_ = 0.0;
  double place_ = 0.0;
};

} // namespace oscillate::native_dsp::effects::dust::detail

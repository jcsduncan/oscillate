#pragma once

#include "effects/dust/parts.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

// Dust's tape layer (ADR-0069): hiss and dropouts. A dropout also takes the
// input's highs down (see age.hpp).
namespace oscillate::native_dsp::effects::dust::detail {

// Hiss: rising (a one-pole high-pass a quarter of the peak), a broad peak
// between tape_peak_low_hz and tape_peak_low_hz * tape_peak_ratio (Tone),
// rolling off above. Its power is linear in Hiss, the default at the
// reference.
inline constexpr double tape_peak_low_hz = 3'000.0;
inline constexpr double tape_peak_ratio = 3.0;
inline constexpr double tape_peak_q = 1.0;
inline constexpr double tape_peak_gain = 1.6;
inline constexpr double tape_hp_ratio = 0.25;
inline constexpr double tape_lp_ratio = 2.6;
// Dropouts: up to tape_dropout_rate a second (Dropouts squared), each a
// smooth dip of tape_drop_min_s .. tape_drop_max_s, tape_drop_depth_min_db
// deep and up to tape_drop_depth_span_db deeper with Dropouts. The input
// also loses its highs in a dip, as tape does.
inline constexpr double tape_dropout_rate = 4.0;
inline constexpr double tape_drop_min_s = 0.015;
inline constexpr double tape_drop_max_s = 0.12;
inline constexpr double tape_drop_depth_min_db = 3.0;
inline constexpr double tape_drop_depth_span_db = 21.0;

class Tape {
public:
  void prepare(LayerFormat format) {
    rate_ = format.rate;
    reference_ = format.reference;
  }
  void reseed(std::uint64_t key) noexcept { rng_.seed(key); }
  void reset() noexcept;
  void set(std::span<const double> v, LayerShape shape) noexcept;
  [[nodiscard]] Pair next() noexcept {
    if (drop_left_ == 0) {
      envelope_ = 1.0;
      if (drop_chance_ > 0.0 && rng_.unit() < drop_chance_) {
        const double seconds =
            tape_drop_min_s * std::pow(tape_drop_max_s / tape_drop_min_s, rng_.unit());
        drop_length_ = static_cast<std::uint32_t>(std::max(1L, std::lround(seconds * rate_)));
        drop_left_ = drop_length_;
        drop_floor_ = gain_of(-(tape_drop_depth_min_db + drop_depth_db_ * rng_.unit()));
      }
    }
    if (drop_left_ > 0) {
      const double x =
          static_cast<double>(drop_length_ - drop_left_) / static_cast<double>(drop_length_);
      const double dip = std::sin(pi * x);
      envelope_ = 1.0 - (1.0 - drop_floor_) * dip * dip;
      --drop_left_;
    }
    const double g = hiss_gain_ * envelope_;
    const double a = low_[0].low(peak_[0].peak(high_[0].high(rng_.white()), tape_peak_gain));
    const double b = low_[1].low(peak_[1].peak(high_[1].high(rng_.white()), tape_peak_gain));
    return width_.place(a * g, b * g);
  }
  // The dropout's level now (1 outside a dip).
  [[nodiscard]] double envelope() const noexcept { return envelope_; }

private:
  double rate_ = nominal_rate;
  double reference_ = 0.0;
  Rng rng_;
  std::array<OnePole, 2> high_{};
  std::array<Svf, 2> peak_{};
  std::array<Svf, 2> low_{};
  Width width_;
  double tone_ = -1.0;
  double power_ = 1.0;
  double hiss_gain_ = 0.0;
  double drop_chance_ = 0.0;
  double drop_depth_db_ = 0.0;
  double drop_floor_ = 1.0;
  double envelope_ = 1.0;
  std::uint32_t drop_length_ = 1;
  std::uint32_t drop_left_ = 0;
};

} // namespace oscillate::native_dsp::effects::dust::detail

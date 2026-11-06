#pragma once

#include "effects/dust/parts.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

// Dust's vinyl layer (ADR-0069): pops and the micro-crackle floor, surface
// noise and rumble coming round with the record, and the scratches pressed
// into it at fixed places.
namespace oscillate::native_dsp::effects::dust::detail {

// Pops: Poisson at the rate Crackle sets (pop_rate_max, pop_rate_span).
// Sizes are Pareto, the tail heavier as Dust rises (fine dust .. big pops),
// from the smallest, vinyl_pop_below_reference under the layer's reference,
// up to vinyl_pop_range times it: the biggest pop pokes about 10 dB out of
// the bed, no more. Pops are sparse, so they are set by their peaks: the
// layer's level comes from the surface and the crackle floor under them.
inline constexpr double vinyl_alpha_fine = 3.2;
inline constexpr double vinyl_alpha_span = 1.9;
inline constexpr double vinyl_pop_range = 20.0;
inline constexpr double vinyl_pop_below_reference = 16.0;
inline constexpr double vinyl_pop_smallest_dbfs = layer_reference_dbfs - vinyl_pop_below_reference;
// Each pop: a half-sine pulse (longer for bigger pops) heard directly, and
// the ring it excites in a band of 1-8 kHz (lower and longer for bigger
// pops, brighter with Tone).
inline constexpr double vinyl_pulse_min_s = 0.08e-3;
inline constexpr double vinyl_pulse_span_s = 0.5e-3;
inline constexpr double vinyl_ring_min_s = 0.25e-3;
inline constexpr double vinyl_ring_span_s = 1.0e-3;
inline constexpr double vinyl_ring_low_hz = 1'000.0;
inline constexpr double vinyl_ring_ratio = 8.0;
inline constexpr double vinyl_ring_tone_low = 0.7;
inline constexpr double vinyl_ring_tone_span = 0.6;
inline constexpr double vinyl_ring_size_drop = 0.35;
inline constexpr double vinyl_pop_direct = 0.7;
inline constexpr double vinyl_pop_ring = 1.0;
inline constexpr double vinyl_pop_place = 0.7;
inline constexpr std::size_t vinyl_pop_voices = 12;
// The micro-crackle floor: clicks a second per channel, sizes uniform in
// [floor_min, floor_min + floor_span], through a band for the fry (Tone
// moves its top).
inline constexpr double vinyl_floor_rate_min = 300.0;
inline constexpr double vinyl_floor_rate_span = 2'500.0;
inline constexpr double vinyl_floor_min = 0.3;
inline constexpr double vinyl_floor_span = 0.7;
inline constexpr double vinyl_floor_square_mean = vinyl_floor_min * vinyl_floor_min +
                                                  vinyl_floor_min * vinyl_floor_span +
                                                  vinyl_floor_span * vinyl_floor_span / 3.0;
inline constexpr double vinyl_floor_hp_hz = 1'200.0;
inline constexpr double vinyl_floor_low_hz = 4'000.0;
inline constexpr double vinyl_floor_ratio = 3.0;
// Surface noise: a high-pass and a low-pass whose corner Tone moves,
// modulated as the record turns.
inline constexpr double vinyl_surface_hp_hz = 250.0;
inline constexpr double vinyl_surface_low_hz = 2'500.0;
inline constexpr double vinyl_surface_ratio = 4.0;
inline constexpr double vinyl_surface_q = 0.6;
inline constexpr double vinyl_surface_turn = 0.25;
inline constexpr double vinyl_surface_turn_phase = 1.3;
// Rumble: noise below vinyl_rumble_low_hz (Tone raises it), the tonearm's
// resonance, and the level coming round with the platter.
inline constexpr double vinyl_rumble_low_hz = 18.0;
inline constexpr double vinyl_rumble_span_hz = 25.0;
inline constexpr double vinyl_rumble_q = 0.8;
inline constexpr double vinyl_rumble_sub_hz = 12.0;
inline constexpr double vinyl_arm_hz = 9.0;
inline constexpr double vinyl_arm_q = 2.5;
inline constexpr double vinyl_arm_level = 0.5;
inline constexpr double vinyl_rumble_turn = 0.6;
inline constexpr double vinyl_rumble_law = 0.75;
// Scratches: fixed places on the record (from the seed), heard once a
// revolution; Wear brings in more of them.
inline constexpr std::size_t vinyl_scratch_count = 6;
inline constexpr double vinyl_scratch_first = 0.08;
inline constexpr double vinyl_scratch_step = 0.15;
inline constexpr double vinyl_scratch_size = 20.0; // in smallest-pop units
inline constexpr double vinyl_scratch_low_hz = 600.0;
inline constexpr double vinyl_scratch_ratio = 4.0;
inline constexpr double vinyl_scratch_jitter = 0.3;
inline constexpr double vinyl_scratch_strength_min = 0.4;
// Wear's drive: tanh(d x) / d with d = 1 + vinyl_wear_drive * wear^2.
inline constexpr double vinyl_wear_drive = 1.5;
// The bed at the defaults, against the layer reference (dB): the crackle
// floor, the surface and the rumble.
inline constexpr double vinyl_floor_db = -3.5;
inline constexpr double vinyl_floor_slope_db = 8.0;
inline constexpr double vinyl_surface_db = -5.0;
inline constexpr double vinyl_surface_wear_db = 8.0;
inline constexpr double vinyl_rumble_db = -12.0;

// A scratch is louder with Wear: vinyl_scratch_base + Wear, in scratch sizes.
inline constexpr double vinyl_scratch_base = 0.5;
inline constexpr double seconds_per_minute = 60.0;

struct PopShape {
  double amplitude = 1.0; // In smallest-pop units.
  double size = 0.0;      // 0 .. 1
  std::uint32_t pulse_frames = 1;
  double ring_hz = vinyl_ring_low_hz;
  double ring_seconds = vinyl_ring_min_s;
  double polarity = 1.0;
  double pan = 0.0;
};

struct PopVoice {
  std::uint32_t life = 0;
  std::uint32_t pulse_at = 0;
  std::uint32_t pulse_frames = 0;
  double pulse = 0.0;
  double re = 0.0;
  double im = 0.0;
  double cr = 0.0;
  double ci = 0.0;
  Pair pan{1.0, 1.0};

  void start(const PopShape& shape, double scale, double place, double rate) noexcept {
    const double a = shape.polarity * shape.amplitude * scale;
    pulse = a * vinyl_pop_direct;
    pulse_at = 0;
    pulse_frames = shape.pulse_frames;
    re = a * vinyl_pop_ring;
    im = 0.0;
    const double decay = std::exp(-1.0 / (shape.ring_seconds * rate));
    const double w = two_pi * std::min(shape.ring_hz, nyquist_guard * rate) / rate;
    cr = decay * std::cos(w);
    ci = decay * std::sin(w);
    pan = pan_gains(shape.pan * place);
    life = static_cast<std::uint32_t>(std::max(
        static_cast<double>(pulse_frames), std::ceil(voice_life_tau * shape.ring_seconds * rate)));
  }
  [[nodiscard]] double next() noexcept {
    double y = 0.0;
    if (pulse_at < pulse_frames) {
      y = pulse *
          std::sin(pi * (static_cast<double>(pulse_at) + 0.5) / static_cast<double>(pulse_frames));
      ++pulse_at;
    }
    const double r = re * cr - im * ci;
    im = re * ci + im * cr;
    re = r;
    --life;
    return y + im;
  }
};

// A pop drawn from the Pareto sizes at tail `alpha`.
[[nodiscard]] PopShape draw_pop(Rng& rng, double alpha, double tone, double rate) noexcept;

class Vinyl {
public:
  void prepare(double rate, double reference);

  // The record itself: where its scratches are, from the seed alone.
  void press(std::uint64_t seed_key) noexcept;

  void reseed(std::uint64_t key) noexcept { rng_.seed(key); }

  void reset() noexcept;

  void set(std::span<const double> v, double tone, double width) noexcept;

  [[nodiscard]] Pair next(double rotation) noexcept {
    const double turn = two_pi * rotation;
    // Surface noise, coming round with the record.
    const double around = 1.0 + vinyl_surface_turn * std::sin(turn + vinyl_surface_turn_phase);
    const double a = surface_low_[0].low(surface_high_[0].high(rng_.white()));
    const double b = surface_low_[1].low(surface_high_[1].high(rng_.white()));
    Pair out = width_.place(a * surface_gain_ * around, b * surface_gain_ * around);
    // The micro-crackle floor.
    double fl = 0.0;
    double fr = 0.0;
    if (rng_.unit() < floor_chance_) {
      fl = floor_amp_ * (vinyl_floor_min + vinyl_floor_span * rng_.unit()) * rng_.sign();
    }
    if (rng_.unit() < floor_chance_) {
      fr = floor_amp_ * (vinyl_floor_min + vinyl_floor_span * rng_.unit()) * rng_.sign();
    }
    const Pair fry = width_.place(floor_lp_[0].low(floor_hp_[0].high(fl)),
                                  floor_lp_[1].low(floor_hp_[1].high(fr)));
    out.left += fry.left;
    out.right += fry.right;
    // Pops, and the scratches as the record brings them round.
    if (rng_.unit() < pop_chance_) {
      start(draw_pop(rng_, alpha_, tone_, rate_), pop_unit_);
    }
    if (audible_scratches_ > 0 && last_rotation_ >= 0.0) {
      for (std::size_t i = 0; i < audible_scratches_; ++i) {
        const double angle = scratches_[i].angle;
        const bool crossed = rotation >= last_rotation_
                                 ? angle > last_rotation_ && angle <= rotation
                                 : angle > last_rotation_ || angle <= rotation;
        if (crossed) {
          scratch(scratches_[i]);
        }
      }
    }
    last_rotation_ = rotation;
    for (auto& voice : voices_) {
      if (voice.life > 0) {
        const double y = voice.next();
        out.left += y * voice.pan.left;
        out.right += y * voice.pan.right;
      }
    }
    // Rumble, its level coming round with the platter: one source, centred.
    if (rumble_gain_ > 0.0) {
      const double motor = rumble_gain_ * (1.0 + vinyl_rumble_turn * std::sin(turn));
      const double low = rumble_low_.low(rumble_sub_.high(rng_.white()));
      const double rumble = (low + vinyl_arm_level * rumble_arm_.band(low)) * motor;
      out.left += rumble;
      out.right += rumble;
    }
    // Wear: a little distortion, mostly on the pops.
    out.left = std::tanh(drive_ * out.left) / drive_;
    out.right = std::tanh(drive_ * out.right) / drive_;
    return out;
  }

  // Pops started since the last call.
  [[nodiscard]] std::uint32_t take_pops() noexcept {
    const std::uint32_t pops = pops_;
    pops_ = 0;
    return pops;
  }
  [[nodiscard]] double frames_per_revolution(std::span<const double> v) const noexcept {
    const auto speed = choice_of(v[p::vinyl_speed], rpm.size());
    return seconds_per_minute * rate_ / rpm[speed];
  }

private:
  struct Scratch {
    double angle = 0.0;
    double strength = 1.0;
    double hz = vinyl_scratch_low_hz;
    double polarity = 1.0;
    double pan = 0.0;
  };

  void start(const PopShape& shape, double scale) noexcept;
  void scratch(const Scratch& s) noexcept;

  double rate_ = nominal_rate;
  double reference_ = 0.0;
  Rng rng_;
  std::array<OnePole, 2> surface_high_{};
  std::array<Svf, 2> surface_low_{};
  std::array<Svf, 2> floor_hp_{};
  std::array<Svf, 2> floor_lp_{};
  std::array<PopVoice, vinyl_pop_voices> voices_{};
  std::array<Scratch, vinyl_scratch_count> scratches_{};
  Svf rumble_low_;
  Svf rumble_arm_;
  OnePole rumble_sub_;
  Width width_;
  double surface_tone_ = -1.0;
  double surface_power_ = 1.0;
  double rumble_power_ = 1.0;
  double floor_power_ = 1.0;
  double surface_gain_ = 0.0;
  double rumble_gain_ = 0.0;
  double floor_chance_ = 0.0;
  double floor_amp_ = 0.0;
  double pop_chance_ = 0.0;
  double pop_unit_ = 0.0;
  double alpha_ = vinyl_alpha_fine;
  double tone_ = 0.5;
  double place_ = 0.0;
  double scratch_scale_ = 0.0;
  double drive_ = 1.0;
  double last_rotation_ = -1.0;
  std::size_t audible_scratches_ = 0;
  std::uint32_t pops_ = 0;
};

} // namespace oscillate::native_dsp::effects::dust::detail

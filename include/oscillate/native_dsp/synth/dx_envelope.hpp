#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace oscillate::native_dsp::synth {

// A six-operator FM envelope with four rates and four levels, on the classic
// chip's own 0-99 scales, so published patches carry over:
// - Note on runs L4 -> L1 at R1, L1 -> L2 at R2, then L2 -> L3 at R3, and
//   holds L3 while the key is down; note off runs to L4 at R4, from wherever
//   the envelope is.
// - Rates are speeds, not times: a short move at one rate takes less time
//   than a long one. Falling segments are linear in decibels. Rising
//   segments jump to about -50 dB, then climb faster the quieter they are,
//   which gives the hardware's punchy attack.
// - Rate scaling speeds every segment up toward the top of the keyboard.
// Levels are kept in the chip's log units (256 per octave, 0.75 dB per
// level step) and timed against its 44.1 kHz reference, so a segment takes
// the same time at any sample rate. Whole-number rates and levels are the
// chip's exact values; fractions between them interpolate (speeds
// geometrically, levels in units), so a knob or automation glides.
struct DxEnvelopeSettings {
  std::array<double, 4> rates{99, 99, 99, 99};
  std::array<double, 4> levels{99, 99, 99, 0};
  int rate_scaling = 0; // 0-7.
  bool operator==(const DxEnvelopeSettings&) const = default;
};

class DxEnvelope {
public:
  static constexpr double units_per_octave = 256.0;
  static constexpr double full_units = 4032.0;   // Level 99.
  static constexpr double floor_units = 208.0;   // The chip's lowest level, about -90 dB.
  static constexpr double attack_units = 1908.0; // Where a rising segment starts, about -50 dB.
  static constexpr double reference_rate = 44'100.0;

  // Envelope units for a level of 0-99.
  [[nodiscard]] static double level_units(int level) noexcept {
    constexpr std::array<int, 20> low{0,  5,  9,  13, 17, 20, 23, 25, 27, 29,
                                      31, 33, 35, 37, 39, 41, 42, 43, 45, 46};
    const int clamped = std::clamp(level, 0, 99);
    const int scaled = clamped < 20 ? low[static_cast<std::size_t>(clamped)] : 28 + clamped;
    return std::max(static_cast<double>((scaled >> 1) << 6), floor_units);
  }
  [[nodiscard]] static double level_units(double level) noexcept {
    const double clamped = std::clamp(level, 0.0, 99.0);
    const int whole = static_cast<int>(clamped);
    const double fraction = clamped - whole;
    const double low = level_units(whole);
    return fraction > 0.0 ? low + fraction * (level_units(whole + 1) - low) : low;
  }
  // Linear amplitude for envelope units; the floor is silence.
  [[nodiscard]] static double amplitude(double units) noexcept {
    return units <= floor_units ? 0.0 : std::exp2((units - full_units) / units_per_octave);
  }
  // Units per reference-rate frame for a rate of 0-99 on a key.
  [[nodiscard]] static double increment(int rate, int rate_scaling, int key) noexcept {
    const int x = std::clamp(key / 3 - 7, 0, 31);
    const int q = std::min(((std::clamp(rate, 0, 99) * 41) >> 6) + ((rate_scaling * x) >> 3), 63);
    return static_cast<double>((4 + (q & 3)) << (2 + (q >> 2))) / 65'536.0;
  }
  [[nodiscard]] static double increment(double rate, int rate_scaling, int key) noexcept {
    const double clamped = std::clamp(rate, 0.0, 99.0);
    const int whole = static_cast<int>(clamped);
    const double fraction = clamped - whole;
    const double low = increment(whole, rate_scaling, key);
    return fraction > 0.0 ? low * std::pow(increment(whole + 1, rate_scaling, key) / low, fraction)
                          : low;
  }
  // A rising segment's speed-up: faster the quieter it is.
  [[nodiscard]] static double attack_multiplier(double units) noexcept {
    // The chip's absolute level is 192 units below the envelope's at full
    // output, in bands of 256.
    return 17.0 - std::floor(std::max(units - 192.0, 0.0) / 256.0);
  }
  // Seconds a segment takes between two levels in units, for tail bounds
  // and drawings.
  [[nodiscard]] static double segment_seconds(double from, double to, double rate, int rate_scaling,
                                              int key) noexcept {
    const double step = increment(rate, rate_scaling, key);
    if (to <= from) {
      return (from - to) / step / reference_rate;
    }
    double frames = 0.0;
    double units = std::max(from, attack_units);
    while (units < to) {
      const double band_end =
          std::min(to, (std::floor((units - 192.0) / 256.0) + 1.0) * 256.0 + 192.0);
      const double speed = attack_multiplier(units) * step;
      frames += std::ceil((band_end - units) / speed);
      units = band_end;
    }
    return frames / reference_rate;
  }

  void prepare(double sample_rate) noexcept { scale_ = reference_rate / sample_rate; }

  void reset() noexcept {
    stage_ = 4;
    down_ = false;
    units_ = floor_units;
    amp_ = 0.0;
  }

  // Control rate. A changed setting re-aims the running segment from where
  // the envelope is, so live edits are smooth; a changed sustain level is
  // followed at R3 while the key is held.
  void set(const DxEnvelopeSettings& settings, int key) noexcept {
    if (settings == settings_ && key == key_) {
      return;
    }
    const bool sustain_moved = settings.levels[2] != settings_.levels[2];
    settings_ = settings;
    key_ = key;
    if (stage_ == 3 && down_ && sustain_moved) {
      begin(2);
    } else if (stage_ < 4) {
      begin(stage_);
    }
  }

  // A new note starts from L4; a retrigger from wherever the envelope is.
  void gate_on() noexcept {
    if (stage_ == 4 && amp_ == 0.0) {
      units_ = level_units(settings_.levels[3]);
    }
    down_ = true;
    begin(0);
  }
  void gate_off() noexcept {
    down_ = false;
    if (stage_ < 4) {
      begin(3);
    }
  }

  [[nodiscard]] double next() noexcept {
    if (stage_ >= 4 || (stage_ == 3 && down_)) {
      return amp_;
    }
    if (rising_) {
      if (units_ < attack_units) {
        units_ = attack_units;
        retune();
      }
      units_ += multiplier_ * step_;
      amp_ *= ratio_;
      if (units_ >= target_) {
        units_ = target_;
        begin(stage_ + 1);
      } else if (attack_multiplier(units_) != multiplier_) {
        retune();
      }
    } else {
      units_ -= step_;
      amp_ *= ratio_;
      if (units_ <= target_) {
        units_ = target_;
        begin(stage_ + 1);
      }
    }
    return amp_;
  }

  [[nodiscard]] double value() const noexcept { return amp_; }
  [[nodiscard]] double units() const noexcept { return units_; }
  // 0-2 attack and decays, 3 sustain or release, 4 done.
  [[nodiscard]] int stage() const noexcept { return stage_; }
  // Done once the release has reached a silent L4; a sounding L4 holds, as
  // on the hardware, until the voice is stolen or reset.
  [[nodiscard]] bool active() const noexcept { return stage_ < 4 || amp_ > 0.0; }

private:
  void begin(int stage) noexcept {
    stage_ = stage;
    if (stage_ >= 4) {
      amp_ = amplitude(units_);
      return;
    }
    const auto index = static_cast<std::size_t>(stage_);
    target_ = level_units(settings_.levels[index]);
    rising_ = target_ > units_;
    step_ = increment(settings_.rates[index], settings_.rate_scaling, key_) * scale_;
    retune();
  }

  // Exact amplitude now, and the per-frame ratio for the current band.
  void retune() noexcept {
    amp_ = amplitude(units_);
    if (rising_) {
      multiplier_ = attack_multiplier(units_);
      ratio_ = std::exp2(multiplier_ * step_ / units_per_octave);
    } else {
      ratio_ = std::exp2(-step_ / units_per_octave);
    }
    if (amp_ == 0.0 && rising_) {
      // Silent until the jump on the next frame sets a real level.
      ratio_ = 1.0;
    }
  }

  DxEnvelopeSettings settings_{};
  int key_ = 60;
  int stage_ = 4;
  bool down_ = false;
  bool rising_ = false;
  double units_ = floor_units;
  double target_ = floor_units;
  double step_ = 0.0;
  double multiplier_ = 1.0;
  double ratio_ = 1.0;
  double amp_ = 0.0;
  double scale_ = 1.0;
};

} // namespace oscillate::native_dsp::synth

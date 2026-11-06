#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace oscillate::native_dsp::synth {

enum class FmWave : std::uint8_t { sine, half_sine, abs_sine, quarter_sine };
inline constexpr std::size_t fm_operators = 6;
inline constexpr std::size_t fm_algorithms = 32;
inline constexpr std::size_t fm_table_size = 4'096;

// One cycle of each operator waveform with a guard point, for linear reads.
// Phases are 32-bit fixed point (2^32 is one cycle): the top bits index the
// table and the rest interpolate, so reads need no floor and wrap for free.
class FmWaveTables {
public:
  static constexpr unsigned index_bits = 12;
  static_assert((std::size_t{1} << index_bits) == fm_table_size);
  static constexpr unsigned fraction_bits = 32 - index_bits;

  // Built once; call from control before the first callback.
  [[nodiscard]] static const FmWaveTables& instance();
  [[nodiscard]] double read(FmWave wave, std::uint32_t phase) const noexcept {
    const std::uint32_t index = phase >> fraction_bits;
    const double t = static_cast<double>(phase & ((1U << fraction_bits) - 1U)) *
                     (1.0 / static_cast<double>(1U << fraction_bits));
    const float* table = &tables_[static_cast<std::size_t>(wave) * (fm_table_size + 1) + index];
    return table[0] + t * (table[1] - table[0]);
  }
  // Phase in cycles, any sign or size.
  [[nodiscard]] double read(FmWave wave, double phase) const noexcept {
    return read(wave, to_fixed(phase));
  }
  static constexpr double fixed_scale = 4294967296.0; // Fixed units per cycle.
  [[nodiscard]] static std::uint32_t to_fixed(double cycles) noexcept {
    return wrap(cycles * fixed_scale);
  }
  // Magnitude below which a double converts to std::int64_t without overflow.
  static constexpr double int64_safe_units = 4611686018427387904.0; // 2^62
  // Already in fixed units (2^-32 cycles), any sign or size. Larger finite
  // values are reduced modulo one cycle first (exact, so the phase matches the
  // direct conversion); NaN and infinities read as phase zero.
  [[nodiscard]] static std::uint32_t wrap(double units) noexcept {
    if (!(std::abs(units) < int64_safe_units)) {
      if (!std::isfinite(units)) {
        return 0;
      }
      units = std::fmod(units, fixed_scale);
    }
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(units));
  }

private:
  FmWaveTables();
  std::array<float, 4 * (fm_table_size + 1)> tables_{};
};

// One FM operator: a fixed-point phase accumulator read through a waveform
// table with a phase offset (modulation, in cycles) and optional
// two-sample-averaged self feedback, as classic FM chips do. Envelopes and
// levels live in the voice.
class FmOperator {
public:
  // A waveform change crossfades old into new at the same phase over this
  // many frames, so switching (from the panel or a stepped automation lane)
  // never steps the output (ADR-0034).
  static constexpr double wave_fade_frames = 128.0;

  FmOperator() : tables_(&FmWaveTables::instance()) {}
  void reset(double phase = 0.0) noexcept {
    phase_ = FmWaveTables::to_fixed(phase);
    previous_ = 0.0;
    last_ = 0.0;
    fade_ = 0.0;
    primed_ = false;
  }
  // increment in cycles per frame; modulation in cycles; feedback depth in
  // cycles applied to the average of the last two outputs.
  [[nodiscard]] double next(double increment, double modulation, double feedback,
                            FmWave wave) noexcept {
    set_wave(wave);
    return next(FmWaveTables::to_fixed(increment), modulation * FmWaveTables::fixed_scale,
                feedback * FmWaveTables::fixed_scale);
  }
  // Control rate: the waveform for the following frames.
  void set_wave(FmWave wave) noexcept {
    if (wave != wave_) {
      // The first wave after a reset is adopted at once; later changes fade.
      fade_ = primed_ ? 1.0 : 0.0;
      faded_ = wave_;
      wave_ = wave;
    }
    primed_ = true;
  }
  // The engine's form: a fixed-point increment, and modulation and feedback
  // depth already in fixed units (2^-32 cycles), so no scaling sits on the
  // serial operator chain.
  [[nodiscard]] double next(std::uint32_t increment, double modulation, double feedback) noexcept {
    const double offset = modulation + feedback * 0.5 * (last_ + previous_);
    return advance(phase_ + FmWaveTables::wrap(offset), increment);
  }
  // Without feedback (modulation in fixed units): reads no previous output,
  // so successive calls depend only on the phase and can overlap.
  [[nodiscard]] double next(std::uint32_t increment, double modulation) noexcept {
    return advance(phase_ + FmWaveTables::wrap(modulation), increment);
  }
  [[nodiscard]] double last() const noexcept { return last_; }
  // The two-sample average that feedback reads, for a loop that feeds this
  // operator's output into another operator.
  [[nodiscard]] double feedback_signal() const noexcept { return 0.5 * (last_ + previous_); }

private:
  [[nodiscard]] double advance(std::uint32_t read, std::uint32_t increment) noexcept {
    double value = tables_->read(wave_, read);
    if (fade_ > 0.0) {
      value += fade_ * (tables_->read(faded_, read) - value);
      fade_ = fade_ > 1.0 / wave_fade_frames ? fade_ - 1.0 / wave_fade_frames : 0.0;
    }
    phase_ += increment;
    previous_ = last_;
    last_ = value;
    return value;
  }

  const FmWaveTables* tables_;
  std::uint32_t phase_ = 0;
  double previous_ = 0.0;
  double last_ = 0.0;
  FmWave wave_ = FmWave::sine;
  FmWave faded_ = FmWave::sine;
  double fade_ = 0.0;
  bool primed_ = false;
};

// A routing: modulates[m] is a bit mask of operators (bit 0 = operator 1)
// that operator m+1 modulates; carriers is the mask of operators heard.
// Feedback runs from operator feedback_from's output into operator
// feedback's input (0-based): the same operator for self-feedback, or a
// loop (algorithms 4 and 6, where the chain's last operator feeds its
// first). Modulators always have higher numbers than their targets, so
// evaluating 6 down to 1 works; a loop reads its source one frame late, as
// the hardware does.
struct FmAlgorithm {
  std::array<std::uint8_t, fm_operators> modulates{};
  std::uint8_t carriers = 0;
  std::uint8_t feedback = 5;
  std::uint8_t feedback_from = 5;
  [[nodiscard]] constexpr bool loop() const noexcept { return feedback_from != feedback; }
};

// The 32 classic six-operator routings from the published algorithm charts,
// as compile-time data so the engine can specialise its kernel per routing.
// Comments give the edges (modulator>target) and feedback, 1-based.
inline constexpr std::array<FmAlgorithm, fm_algorithms> fm_algorithm_data{{
    {{0x00, 0x01, 0x00, 0x04, 0x08, 0x10}, 0x05, 5, 5}, // 1: 2>1, 6>5, 5>4, 4>3; fb 6
    {{0x00, 0x01, 0x00, 0x04, 0x08, 0x10}, 0x05, 1, 1}, // 2: 2>1, 6>5, 5>4, 4>3; fb 2
    {{0x00, 0x01, 0x02, 0x00, 0x08, 0x10}, 0x09, 5, 5}, // 3: 3>2, 2>1, 6>5, 5>4; fb 6
    {{0x00, 0x01, 0x02, 0x00, 0x08, 0x10}, 0x09, 5, 3}, // 4: 3>2, 2>1, 6>5, 5>4; fb 4>6
    {{0x00, 0x01, 0x00, 0x04, 0x00, 0x10}, 0x15, 5, 5}, // 5: 2>1, 4>3, 6>5; fb 6
    {{0x00, 0x01, 0x00, 0x04, 0x00, 0x10}, 0x15, 5, 4}, // 6: 2>1, 4>3, 6>5; fb 5>6
    {{0x00, 0x01, 0x00, 0x04, 0x04, 0x10}, 0x05, 5, 5}, // 7: 2>1, 4>3, 5>3, 6>5; fb 6
    {{0x00, 0x01, 0x00, 0x04, 0x04, 0x10}, 0x05, 3, 3}, // 8: 2>1, 4>3, 5>3, 6>5; fb 4
    {{0x00, 0x01, 0x00, 0x04, 0x04, 0x10}, 0x05, 1, 1}, // 9: 2>1, 4>3, 5>3, 6>5; fb 2
    {{0x00, 0x01, 0x02, 0x00, 0x08, 0x08}, 0x09, 2, 2}, // 10: 3>2, 2>1, 5>4, 6>4; fb 3
    {{0x00, 0x01, 0x02, 0x00, 0x08, 0x08}, 0x09, 5, 5}, // 11: 3>2, 2>1, 5>4, 6>4; fb 6
    {{0x00, 0x01, 0x00, 0x04, 0x04, 0x04}, 0x05, 1, 1}, // 12: 2>1, 4>3, 5>3, 6>3; fb 2
    {{0x00, 0x01, 0x00, 0x04, 0x04, 0x04}, 0x05, 5, 5}, // 13: 2>1, 4>3, 5>3, 6>3; fb 6
    {{0x00, 0x01, 0x00, 0x04, 0x08, 0x08}, 0x05, 5, 5}, // 14: 2>1, 4>3, 5>4, 6>4; fb 6
    {{0x00, 0x01, 0x00, 0x04, 0x08, 0x08}, 0x05, 1, 1}, // 15: 2>1, 4>3, 5>4, 6>4; fb 2
    {{0x00, 0x01, 0x01, 0x04, 0x01, 0x10}, 0x01, 5, 5}, // 16: 2>1, 3>1, 4>3, 5>1, 6>5; fb 6
    {{0x00, 0x01, 0x01, 0x04, 0x01, 0x10}, 0x01, 1, 1}, // 17: 2>1, 3>1, 4>3, 5>1, 6>5; fb 2
    {{0x00, 0x01, 0x01, 0x01, 0x08, 0x10}, 0x01, 2, 2}, // 18: 2>1, 3>1, 4>1, 5>4, 6>5; fb 3
    {{0x00, 0x01, 0x02, 0x00, 0x00, 0x18}, 0x19, 5, 5}, // 19: 3>2, 2>1, 6>4, 6>5; fb 6
    {{0x00, 0x00, 0x03, 0x00, 0x08, 0x08}, 0x0B, 2, 2}, // 20: 3>1, 3>2, 5>4, 6>4; fb 3
    {{0x00, 0x00, 0x03, 0x00, 0x00, 0x18}, 0x1B, 2, 2}, // 21: 3>1, 3>2, 6>4, 6>5; fb 3
    {{0x00, 0x01, 0x00, 0x00, 0x00, 0x1C}, 0x1D, 5, 5}, // 22: 2>1, 6>3, 6>4, 6>5; fb 6
    {{0x00, 0x00, 0x02, 0x00, 0x00, 0x18}, 0x1B, 5, 5}, // 23: 3>2, 6>4, 6>5; fb 6
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x1C}, 0x1F, 5, 5}, // 24: 6>3, 6>4, 6>5; fb 6
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x18}, 0x1F, 5, 5}, // 25: 6>4, 6>5; fb 6
    {{0x00, 0x00, 0x02, 0x00, 0x08, 0x08}, 0x0B, 5, 5}, // 26: 3>2, 5>4, 6>4; fb 6
    {{0x00, 0x00, 0x02, 0x00, 0x08, 0x08}, 0x0B, 2, 2}, // 27: 3>2, 5>4, 6>4; fb 3
    {{0x00, 0x01, 0x00, 0x04, 0x08, 0x00}, 0x25, 4, 4}, // 28: 2>1, 5>4, 4>3; fb 5
    {{0x00, 0x00, 0x00, 0x04, 0x00, 0x10}, 0x17, 5, 5}, // 29: 4>3, 6>5; fb 6
    {{0x00, 0x00, 0x00, 0x04, 0x08, 0x00}, 0x27, 4, 4}, // 30: 5>4, 4>3; fb 5
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x10}, 0x1F, 5, 5}, // 31: 6>5; fb 6
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 0x3F, 5, 5}, // 32: none; fb 6
}};

[[nodiscard]] inline const std::array<FmAlgorithm, fm_algorithms>& fm_algorithm_table() noexcept {
  return fm_algorithm_data;
}

} // namespace oscillate::native_dsp::synth

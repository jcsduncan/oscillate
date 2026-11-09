#pragma once

#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"
#include "oscillate/native_dsp/synth/nova/fx.hpp"
#include "oscillate/native_dsp/synth/oversampler.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Private building blocks of Nova's effect rack (spec 08 §7, contract 08a
// §5): the shared glides, meters, delay line, crossover and resampler, the
// driver that runs a native effect kernel (ADR-0040) inside a Nova module,
// and one factory per module family.
namespace oscillate::native_dsp::synth::nova::fx_detail {

using Frames = std::span<engine::StereoFrame>;
inline constexpr double pi = std::numbers::pi;
inline constexpr double two_pi = 2.0 * std::numbers::pi;
// Every module is created for up to this many times the engine's rate.
inline constexpr std::uint32_t max_factor = 4;

[[nodiscard]] inline double clamp01(double p) noexcept {
  return std::isfinite(p) ? std::clamp(p, 0.0, 1.0) : 0.0;
}
[[nodiscard]] inline double lerp(double p, double lo, double hi) noexcept {
  return lo + (hi - lo) * clamp01(p);
}
[[nodiscard]] inline double log_map(double p, double lo, double hi) noexcept {
  return lo * std::pow(hi / lo, clamp01(p));
}
// A mode carried on a p value (08a §5): index = round(p * (n - 1)).
[[nodiscard]] inline std::size_t choice(double p, std::size_t n) noexcept {
  return static_cast<std::size_t>(std::lround(clamp01(p) * static_cast<double>(n - 1)));
}
using native_dsp::dsp::db_to_gain;
// Nova's meters and dynamics read -120 dB below one part in a million.
constexpr double meter_floor_db = -120.0;
constexpr double meter_threshold = 1e-6;
[[nodiscard]] inline double gain_to_db(double gain) noexcept {
  return native_dsp::dsp::gain_to_db(gain, meter_floor_db, meter_threshold);
}
// The chain factor (1, 2 or 4) a module rate stands for.
[[nodiscard]] inline std::uint32_t factor_for(double rate, double base) noexcept {
  const double r = rate / base;
  return r > 3.0 ? 4U : (r > 1.5 ? 2U : 1U);
}

// A linear glide that lands exactly on its target and then holds, however
// long the caller runs past the frames it was given.
struct Glide {
  double value = 0.0;
  double target = 0.0;
  double step = 0.0;
  std::uint32_t left = 0;

  void jump(double v) noexcept {
    value = target = v;
    step = 0.0;
    left = 0;
  }
  void to(double v, std::uint32_t frames) noexcept {
    if (frames == 0) {
      jump(v);
      return;
    }
    target = v;
    step = (v - value) / static_cast<double>(frames);
    left = frames;
  }
  double next() noexcept {
    if (left > 0) {
      --left;
      value = left == 0 ? target : value + step;
    }
    return value;
  }
  [[nodiscard]] bool moving() const noexcept { return left > 0; }
};

// Output peak for the telemetry frame, falling 24 dB per second.
class PeakMeter {
public:
  void prepare(double rate) noexcept { fall_ = std::pow(10.0, -24.0 / 20.0 / rate); }
  void feed(std::span<const engine::StereoFrame> io) noexcept {
    for (const auto& f : io) {
      const double a =
          std::max(std::fabs(static_cast<double>(f.left)), std::fabs(static_cast<double>(f.right)));
      peak_ = std::isfinite(a) ? std::max(peak_ * fall_, a) : peak_ * fall_;
    }
  }
  void reset() noexcept { peak_ = 0.0; }
  [[nodiscard]] float db() const noexcept { return static_cast<float>(gain_to_db(peak_)); }

private:
  double fall_ = 0.9999;
  double peak_ = 0.0;
};

// A power-of-two ring read at fractional delays through four-point
// Lagrange interpolation. Sized on control, then allocation-free.
class DelayLine {
public:
  void prepare(std::size_t frames) {
    std::size_t size = 8;
    while (size < frames + 8) {
      size *= 2;
    }
    buffer_.assign(size, 0.0);
    mask_ = size - 1;
    write_ = 0;
  }
  void reset() noexcept {
    std::ranges::fill(buffer_, 0.0);
    write_ = 0;
  }
  void write(double x) noexcept { buffer_[write_++ & mask_] = x; }
  // The input `frames` samples before the newest write (at least 1).
  [[nodiscard]] double read(double frames) const noexcept {
    const double limit = static_cast<double>(mask_ - 4);
    frames = std::clamp(frames, 1.0, limit);
    const auto whole = static_cast<std::size_t>(frames);
    const double t = frames - static_cast<double>(whole);
    const std::size_t newest = write_ - 1;
    const double ym1 = buffer_[(newest - whole + 1) & mask_];
    const double y0 = buffer_[(newest - whole) & mask_];
    const double y1 = buffer_[(newest - whole - 1) & mask_];
    const double y2 = buffer_[(newest - whole - 2) & mask_];
    const double cm1 = -t * (t - 1.0) * (t - 2.0) / 6.0;
    const double c0 = (t + 1.0) * (t - 1.0) * (t - 2.0) / 2.0;
    const double c1 = -(t + 1.0) * t * (t - 2.0) / 2.0;
    const double c2 = (t + 1.0) * t * (t - 1.0) / 6.0;
    return cm1 * ym1 + c0 * y0 + c1 * y1 + c2 * y2;
  }
  [[nodiscard]] std::size_t capacity() const noexcept { return mask_ - 4; }

private:
  std::vector<double> buffer_;
  std::size_t mask_ = 0;
  std::size_t write_ = 0;
};

// Fourth-order Linkwitz-Riley split from two Butterworth SVF pairs: the
// bands sum to a second-order allpass, flat in magnitude.
struct Lr4 {
  BasicZdfSvf<StereoSample> lp1;
  BasicZdfSvf<StereoSample> lp2;
  BasicZdfSvf<StereoSample> hp1;
  BasicZdfSvf<StereoSample> hp2;

  [[nodiscard]] static SvfCoefficients coefficients(double hz, double rate) noexcept {
    return SvfCoefficients::make(prewarp(hz / rate), std::numbers::sqrt2);
  }
  void process(StereoSample x, const SvfCoefficients& c, StereoSample& low,
               StereoSample& high) noexcept {
    low = lp2.process(lp1.process(x, c).low, c).low;
    high = hp2.process(hp1.process(x, c).high, c).high;
  }
  // The same allpass the split leaves on the sum, for phase-matching a band.
  [[nodiscard]] StereoSample allpass(StereoSample x, const SvfCoefficients& c) noexcept {
    StereoSample low;
    StereoSample high;
    process(x, c, low, high);
    return low + high;
  }
  void reset() noexcept {
    lp1.reset();
    lp2.reset();
    hp1.reset();
    hp2.reset();
  }
};

// Halfband polyphase IIR stages between a base rate and 1x, 2x or 4x it.
// One instance can carry one stream up and another down at the same time.
class Resampler {
public:
  void set_factor(std::uint32_t factor) noexcept {
    factor_ = factor == 4U ? 4U : (factor == 2U ? 2U : 1U);
    reset();
  }
  [[nodiscard]] std::uint32_t factor() const noexcept { return factor_; }
  void reset() noexcept {
    for (auto& u : up_) {
      u.reset();
    }
    for (auto& d : down_) {
      d.reset();
    }
  }
  // n base frames in, n * factor out.
  void up(const engine::StereoFrame* in, std::size_t n, engine::StereoFrame* out) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
      const StereoSample x{in[i].left, in[i].right};
      if (factor_ == 1U) {
        out[i] = in[i];
        continue;
      }
      StereoSample a;
      StereoSample b;
      up_[0].process(x, a, b);
      if (factor_ == 2U) {
        out[2 * i] = to_frame(a);
        out[2 * i + 1] = to_frame(b);
        continue;
      }
      StereoSample a1;
      StereoSample a2;
      StereoSample b1;
      StereoSample b2;
      up_[1].process(a, a1, a2);
      up_[1].process(b, b1, b2);
      out[4 * i] = to_frame(a1);
      out[4 * i + 1] = to_frame(a2);
      out[4 * i + 2] = to_frame(b1);
      out[4 * i + 3] = to_frame(b2);
    }
  }
  // n * factor frames in, n base frames out. `in` may equal `out`.
  void down(const engine::StereoFrame* in, std::size_t n, engine::StereoFrame* out) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
      if (factor_ == 1U) {
        out[i] = in[i];
        continue;
      }
      if (factor_ == 2U) {
        out[i] = to_frame(down_[0].process(from_frame(in[2 * i]), from_frame(in[2 * i + 1])));
        continue;
      }
      const StereoSample a = down_[1].process(from_frame(in[4 * i]), from_frame(in[4 * i + 1]));
      const StereoSample b = down_[1].process(from_frame(in[4 * i + 2]), from_frame(in[4 * i + 3]));
      out[i] = to_frame(down_[0].process(a, b));
    }
  }
  // Up and straight back down, for phase-matching a path that skips the
  // oversampled processing.
  [[nodiscard]] engine::StereoFrame pass(engine::StereoFrame x) noexcept {
    if (factor_ == 1U) {
      return x;
    }
    std::array<engine::StereoFrame, 4> high{};
    up(&x, 1, high.data());
    engine::StereoFrame y{};
    down(high.data(), 1, &y);
    return y;
  }

  [[nodiscard]] static engine::StereoFrame to_frame(StereoSample s) noexcept {
    return {static_cast<float>(s.left), static_cast<float>(s.right)};
  }
  [[nodiscard]] static StereoSample from_frame(engine::StereoFrame f) noexcept {
    return {f.left, f.right};
  }

private:
  std::uint32_t factor_ = 1;
  std::array<StereoUpsampler2x, 2> up_{};
  std::array<StereoDownsampler2x, 2> down_{};
};

// Runs one native effect kernel (ADR-0040) at the engine's base rate inside
// a chain that may be oversampled: the chain's audio is decimated to the
// base rate, processed and interpolated back. Time-based effects gain
// nothing from oversampling, and the kernel keeps its state (tails, LFO
// phases) when the chain's factor changes. The kernel's own ramps are
// re-aimed at their targets once the frames of a tick run out, so a caller
// that processes past a tick never overshoots.
class KernelDriver {
public:
  // Control only. False when the engine has no kernel or misses an id.
  [[nodiscard]] bool init(const effects::EffectEngine& engine, double base_rate,
                          std::size_t max_frames);
  // Looks a parameter up by id; a missing id makes ok() false.
  [[nodiscard]] std::size_t index(std::string_view id);
  [[nodiscard]] bool ok() const noexcept { return ok_ && kernel_ != nullptr; }
  void value(std::size_t index, double v) noexcept {
    if (index < values_.size()) {
      values_[index] = v;
    }
  }
  // At a tick: values reach their targets over `base_frames` base-rate
  // frames (0 jumps).
  void control(std::uint32_t base_frames, double beats_per_second) noexcept;
  void set_factor(std::uint32_t factor) noexcept;
  [[nodiscard]] std::uint32_t factor() const noexcept { return resampler_.factor(); }
  // In place at the chain's rate.
  void process(Frames io) noexcept;
  void reset() noexcept;
  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept {
    return kernel_->telemetry(out);
  }
  [[nodiscard]] std::uint64_t tail_frames() const noexcept {
    return engine_->tail_frames(values_, base_rate_);
  }
  [[nodiscard]] double base_rate() const noexcept { return base_rate_; }

private:
  void issue(std::uint32_t frames) noexcept;
  void run(Frames base) noexcept;

  static constexpr std::uint32_t hold_frames = 64;
  const effects::EffectEngine* engine_ = nullptr;
  std::unique_ptr<effects::EffectKernel> kernel_;
  std::vector<double> values_;
  std::vector<engine::StereoFrame> scratch_;
  Resampler resampler_;
  Transport transport_{};
  double base_rate_ = 48'000.0;
  std::uint32_t remaining_ = 0;
  std::uint32_t issued_ = 0;
  bool primed_ = false;
  bool ok_ = true;
};

// ---- EQ and Shifter mappings (contract constants in fx.hpp) --------------------------

[[nodiscard]] inline EqType eq_type(double p) noexcept {
  return static_cast<EqType>(choice(p, eq_type_count));
}
// The contract's EQ defaults: low shelf 100 Hz, bells at 400 Hz and 2.5 kHz,
// high shelf 8 kHz, all flat.
[[nodiscard]] FxParams eq_defaults() noexcept;
// p0 -> Hz: 5000 (2p - 1)^3, fine near zero.
[[nodiscard]] inline double shifter_hz(double p) noexcept {
  const double d = 2.0 * clamp01(p) - 1.0;
  return shifter_max_hz * d * d * d;
}

// ---- Factories (null when a kernel cannot be made) ----------------------------------

// fx_wrapped.cpp: modules around the native effect kernels.
[[nodiscard]] std::unique_ptr<FxModule> make_chorus(double base_rate, std::size_t max_frames);
[[nodiscard]] std::unique_ptr<FxModule> make_phaser(double base_rate, std::size_t max_frames);
[[nodiscard]] std::unique_ptr<FxModule> make_delay(double base_rate, std::size_t max_frames);
[[nodiscard]] std::unique_ptr<FxModule> make_reverb(double base_rate, std::size_t max_frames);
[[nodiscard]] std::unique_ptr<FxModule> make_flyby(double base_rate, std::size_t max_frames);
// fx_tone.cpp: distortion (Forge plus Nova's own curves) and the filter.
[[nodiscard]] std::unique_ptr<FxModule> make_distortion(double base_rate, std::size_t max_frames);
[[nodiscard]] std::unique_ptr<FxModule> make_filter(double base_rate, std::size_t max_frames);
// fx_dynamics.cpp: compressor, multiband (OTT), EQ, utility.
[[nodiscard]] std::unique_ptr<FxModule> make_compressor(double base_rate, std::size_t max_frames);
[[nodiscard]] std::unique_ptr<FxModule> make_multiband(double base_rate, std::size_t max_frames);
[[nodiscard]] std::unique_ptr<FxModule> make_eq(double base_rate, std::size_t max_frames);
[[nodiscard]] std::unique_ptr<FxModule> make_utility(double base_rate, std::size_t max_frames);
// fx_motion.cpp: flanger, dimension, hyper.
[[nodiscard]] std::unique_ptr<FxModule> make_flanger(double base_rate, std::size_t max_frames);
[[nodiscard]] std::unique_ptr<FxModule> make_dimension(double base_rate, std::size_t max_frames);
[[nodiscard]] std::unique_ptr<FxModule> make_hyper(double base_rate, std::size_t max_frames);
// fx_convolve.cpp: the convolver. `ir` may be null (the module is silent);
// `sha256` identifies the IR for instance sharing.
[[nodiscard]] std::unique_ptr<FxModule> make_convolve(double base_rate, std::size_t max_frames,
                                                      std::shared_ptr<const SynthMediaAsset> ir,
                                                      std::string sha256);
// fx_shift.cpp: the frequency shifter.
[[nodiscard]] std::unique_ptr<FxModule> make_shifter(double base_rate, std::size_t max_frames);
// The IR identity a convolve module was made with; empty for other modules.
[[nodiscard]] std::string_view convolve_identity(const FxModule& module) noexcept;

} // namespace oscillate::native_dsp::synth::nova::fx_detail

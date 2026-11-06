#include "fx_modules.hpp"

#include "oscillate/native_dsp/compressor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>

// Nova's Compressor, Multiband (OTT), EQ and Utility modules (08a §5). All
// run at the chain's rate and recompute their coefficients when it changes,
// keeping their filter and detector state.
namespace oscillate::native_dsp::synth::nova::fx_detail {
namespace {

// Smoothed coefficients are rebuilt this often (frames), from per-frame
// glides, so modulation never zippers and tan() stays off the per-frame path.
constexpr std::uint32_t coefficient_interval = 16;

[[nodiscard]] double one_pole(double seconds, double rate) noexcept {
  return 1.0 - std::exp(-1.0 / (std::max(seconds, 1e-6) * rate));
}

[[nodiscard]] StereoSample sample(const engine::StereoFrame& f) noexcept {
  return {f.left, f.right};
}
void store(engine::StereoFrame& f, StereoSample s) noexcept {
  f.left = std::isfinite(s.left) ? static_cast<float>(s.left) : 0.0F;
  f.right = std::isfinite(s.right) ? static_cast<float>(s.right) : 0.0F;
}

// ---- Compressor ---------------------------------------------------------------------

// A feed-forward, stereo-linked compressor. The static curve is the classic
// native compressor's gain computer (ADR-0047, 6 dB soft knee); the gain
// reduction is smoothed in decibels with separate attack and release, so
// release does not slow down as reduction deepens. The "Voice sum"
// sidechain is the module's own input in this API.
class CompressorModule final : public FxModule {
public:
  CompressorModule(double base_rate, std::size_t) : base_(base_rate) {
    rate(base_rate);
    set(fx_defaults(FxKind::compressor).p, {2.0, base_rate}, 0);
  }
  [[nodiscard]] FxKind kind() const noexcept override { return FxKind::compressor; }
  void reset() noexcept override { reduction_ = 0.0; }
  void set(const FxParams& p, const FxContext& context, std::uint32_t frames) noexcept override {
    const std::uint32_t factor = factor_for(context.sample_rate, base_);
    if (base_ * factor != rate_) {
      rate(base_ * factor);
    }
    attack_ms_ = log_map(p[2], 0.1, 100.0);
    release_ms_ = log_map(p[3], 10.0, 1'000.0);
    attack_ = std::exp(-1.0 / (attack_ms_ * 1e-3 * rate_));
    release_ = std::exp(-1.0 / (release_ms_ * 1e-3 * rate_));
    threshold_.to(lerp(p[0], -60.0, 0.0), frames);
    ratio_.to(log_map(p[1], 1.0, 20.0), frames);
    makeup_.to(24.0 * clamp01(p[4]), frames);
  }
  void process(Frames io) noexcept override {
    CompressorSettings curve;
    curve.knee_db = 6.0;
    for (auto& frame : io) {
      curve.threshold_db = threshold_.next();
      curve.ratio = ratio_.next();
      const double makeup = makeup_.next();
      const double level = std::max(std::fabs(static_cast<double>(frame.left)),
                                    std::fabs(static_cast<double>(frame.right)));
      const double target =
          std::isfinite(level) ? compressor_reduction_db(gain_to_db(level), curve) : reduction_;
      const double coefficient = target > reduction_ ? attack_ : release_;
      reduction_ = target + coefficient * (reduction_ - target);
      const double gain = db_to_gain(makeup - reduction_);
      store(frame, sample(frame) * gain);
    }
  }
  [[nodiscard]] float telemetry() const noexcept override {
    return static_cast<float>(std::max(0.0, reduction_));
  }

private:
  void rate(double rate) noexcept {
    rate_ = rate;
    attack_ = std::exp(-1.0 / (attack_ms_ * 1e-3 * rate_));
    release_ = std::exp(-1.0 / (release_ms_ * 1e-3 * rate_));
  }

  double base_;
  double rate_ = 0.0;
  double attack_ms_ = 10.0;
  double release_ms_ = 100.0;
  double attack_ = 0.0;
  double release_ = 0.0;
  Glide threshold_;
  Glide ratio_;
  Glide makeup_;
  double reduction_ = 0.0;
};

// ---- Multiband (OTT) ----------------------------------------------------------------

// Three Linkwitz-Riley bands (88 Hz and 2.5 kHz, the low band phase-matched
// through the upper split's allpass so the bands sum flat), each squashed
// down above -16 dB and lifted up below -34 dB, then made up: dense and
// loud, as OTT is. Depth scales the whole gain change in decibels.
constexpr double low_crossover = 88.0;
constexpr double high_crossover = 2'500.0;
constexpr std::array<double, 3> band_time{2.0, 1.0, 0.5};

class MultibandModule final : public FxModule {
public:
  MultibandModule(double base_rate, std::size_t) : base_(base_rate) {
    rate(base_rate);
    set(fx_defaults(FxKind::multiband).p, {2.0, base_rate}, 0);
  }
  [[nodiscard]] FxKind kind() const noexcept override { return FxKind::multiband; }
  void reset() noexcept override {
    low_split_.reset();
    high_split_.reset();
    phase_match_.reset();
    envelope_ = {};
    gain_db_ = {};
  }
  void set(const FxParams& p, const FxContext& context, std::uint32_t frames) noexcept override {
    const std::uint32_t factor = factor_for(context.sample_rate, base_);
    if (base_ * factor != rate_) {
      rate(base_ * factor);
    }
    time_scale_ = std::pow(4.0, 2.0 * clamp01(p[1]) - 1.0);
    coefficients();
    depth_.to(clamp01(p[0]), frames);
    const double balance = 2.0 * clamp01(p[2]) - 1.0; // -1 down only .. +1 up only.
    down_.to(std::clamp(1.0 - balance, 0.0, 1.0), frames);
    up_.to(std::clamp(1.0 + balance, 0.0, 1.0), frames);
    for (std::size_t k = 0; k < 3; ++k) {
      output_[k].to(db_to_gain(lerp(p[3 + k], -12.0, 12.0)), frames);
    }
  }
  void process(Frames io) noexcept override {
    for (auto& frame : io) {
      const double depth = depth_.next();
      const double down = down_.next();
      const double up = up_.next();
      StereoSample low_raw;
      StereoSample rest;
      low_split_.process(sample(frame), low_c_, low_raw, rest);
      StereoSample mid;
      StereoSample high;
      high_split_.process(rest, high_c_, mid, high);
      const StereoSample low = phase_match_.allpass(low_raw, high_c_);
      const std::array<StereoSample, 3> band{low, mid, high};
      StereoSample sum{};
      for (std::size_t k = 0; k < 3; ++k) {
        const double level = std::max(std::fabs(band[k].left), std::fabs(band[k].right));
        double& envelope = envelope_[k];
        envelope += (level - envelope) * (level > envelope ? attack_[k] : release_[k]);
        const double db = gain_to_db(envelope);
        double change = 0.0;
        if (db > -16.0) {
          change = -(db + 16.0) * (1.0 - 1.0 / 6.0) * down;
        } else if (db < -34.0) {
          // Never lift near-silence into hiss: the lift fades out from -64
          // to -84 dB.
          const double gate = std::clamp((db + 84.0) / 20.0, 0.0, 1.0);
          change = std::min(24.0, (-34.0 - db) * (1.0 - 1.0 / 3.0)) * gate * up;
        }
        const double target = depth * (change + 4.0 * gate_level(db));
        gain_db_[k] += (target - gain_db_[k]) * smooth_;
        sum = sum + band[k] * (db_to_gain(gain_db_[k]) * output_[k].next());
      }
      store(frame, sum);
    }
  }
  [[nodiscard]] float telemetry() const noexcept override {
    double reduction = 0.0;
    for (const double g : gain_db_) {
      reduction = std::max(reduction, -g);
    }
    return static_cast<float>(reduction);
  }
  void detail(std::span<float, fx_detail_values> out) const noexcept override {
    for (std::size_t k = 0; k < out.size(); ++k) {
      out[k] = static_cast<float>(gain_db_[k]);
    }
  }
  [[nodiscard]] std::uint64_t tail_frames() const noexcept override {
    return static_cast<std::uint64_t>(std::ceil(0.05 * rate_));
  }

private:
  // The make-up follows the signal in, so silence stays silent.
  [[nodiscard]] static double gate_level(double db) noexcept {
    return std::clamp((db + 84.0) / 20.0, 0.0, 1.0);
  }
  void rate(double rate) noexcept {
    rate_ = rate;
    low_c_ = Lr4::coefficients(low_crossover, rate);
    high_c_ = Lr4::coefficients(high_crossover, rate);
    smooth_ = one_pole(0.001, rate);
    coefficients();
  }
  void coefficients() noexcept {
    for (std::size_t k = 0; k < 3; ++k) {
      attack_[k] = one_pole(0.003 * time_scale_ * band_time[k], rate_);
      release_[k] = one_pole(0.15 * time_scale_ * band_time[k], rate_);
    }
  }

  double base_;
  double rate_ = 0.0;
  double time_scale_ = 1.0;
  double smooth_ = 0.0;
  SvfCoefficients low_c_{};
  SvfCoefficients high_c_{};
  Lr4 low_split_;
  Lr4 high_split_;
  Lr4 phase_match_;
  std::array<double, 3> attack_{};
  std::array<double, 3> release_{};
  std::array<double, 3> envelope_{};
  std::array<double, 3> gain_db_{};
  Glide depth_;
  Glide down_;
  Glide up_;
  std::array<Glide, 3> output_{};
};

// ---- EQ -----------------------------------------------------------------------------

// Four parametric bands in series (08a §5), each a trapezoidal SVF (after
// Simper) mixed into the response its type asks for, so gains, frequencies
// and Qs can glide without zipper or instability. The 24 dB cuts add a
// second section: a Butterworth pair whose upper Q follows the band's, so
// Q 0.707 is a fourth-order Butterworth. A type change crossfades from the
// old response to the new one.
constexpr double butterworth4_k_low = 1.0 / 0.5412; // The flat section of a 4th-order Butterworth.
constexpr double butterworth4_q_high = 1.3066;      // Its peaked section's Q...
constexpr double butterworth2_q = 1.0 / std::numbers::sqrt2; // ...for a user Q this flat.
constexpr double eq_type_fade_seconds = 0.01;

// y = m0 x + m1 band + m2 low for one section.
struct Mix3 {
  double m0 = 1.0;
  double m1 = 0.0;
  double m2 = 0.0;
};

struct EqStage {
  EqType type = EqType::off;
  BasicZdfSvf<StereoSample> first;
  BasicZdfSvf<StereoSample> second;
  SvfCoefficients first_c{};
  SvfCoefficients second_c{};
  Mix3 first_m{};
  Mix3 second_m{};
  bool two = false;

  void reset() noexcept {
    first.reset();
    second.reset();
  }
  void design(double g, double db, double q, EqType t) noexcept {
    type = t;
    const double k = 1.0 / q;
    const double a = std::pow(10.0, db / 40.0);
    two = t == EqType::low_cut_24 || t == EqType::high_cut_24;
    switch (t) {
    case EqType::off:
      first_c = SvfCoefficients::make(g, k);
      first_m = {1.0, 0.0, 0.0};
      break;
    case EqType::bell:
      first_c = SvfCoefficients::make(g, 1.0 / (q * a));
      first_m = {1.0, first_c.k * (a * a - 1.0), 0.0};
      break;
    case EqType::low_shelf:
      first_c = SvfCoefficients::make(g / std::sqrt(a), k);
      first_m = {1.0, k * (a - 1.0), a * a - 1.0};
      break;
    case EqType::high_shelf:
      first_c = SvfCoefficients::make(g * std::sqrt(a), k);
      first_m = {a * a, k * (1.0 - a) * a, 1.0 - a * a};
      break;
    case EqType::low_cut_12:
      first_c = SvfCoefficients::make(g, k);
      first_m = {1.0, -k, -1.0};
      break;
    case EqType::high_cut_12:
      first_c = SvfCoefficients::make(g, k);
      first_m = {0.0, 0.0, 1.0};
      break;
    case EqType::low_cut_24:
    case EqType::high_cut_24: {
      const double k2 = 1.0 / (q * butterworth4_q_high / butterworth2_q);
      first_c = SvfCoefficients::make(g, butterworth4_k_low);
      second_c = SvfCoefficients::make(g, k2);
      if (t == EqType::low_cut_24) {
        first_m = {1.0, -first_c.k, -1.0};
        second_m = {1.0, -k2, -1.0};
      } else {
        first_m = {0.0, 0.0, 1.0};
        second_m = {0.0, 0.0, 1.0};
      }
      break;
    }
    case EqType::notch:
      first_c = SvfCoefficients::make(g, k);
      first_m = {1.0, -k, 0.0};
      break;
    case EqType::band_pass:
      first_c = SvfCoefficients::make(g, k);
      first_m = {0.0, k, 0.0};
      break;
    }
  }
  [[nodiscard]] StereoSample process(StereoSample x) noexcept {
    const auto a = first.process(x, first_c);
    StereoSample y = first_m.m0 * x + first_m.m1 * a.band + first_m.m2 * a.low;
    if (two) {
      const auto b = second.process(y, second_c);
      y = second_m.m0 * y + second_m.m1 * b.band + second_m.m2 * b.low;
    }
    return y;
  }
};

class EqModule final : public FxModule {
public:
  EqModule(double base_rate, std::size_t) : base_(base_rate) {
    rate(base_rate);
    set(fx_defaults(FxKind::eq).p, {2.0, base_rate}, 0);
  }
  [[nodiscard]] FxKind kind() const noexcept override { return FxKind::eq; }
  void reset() noexcept override {
    for (auto& band : bands_) {
      band.now.reset();
      band.was.reset();
      band.fade.jump(1.0);
    }
    meter_.reset();
  }
  void set(const FxParams& p, const FxContext& context, std::uint32_t frames) noexcept override {
    const std::uint32_t factor = factor_for(context.sample_rate, base_);
    if (base_ * factor != rate_) {
      rate(base_ * factor);
    }
    const bool first = !primed_;
    for (std::size_t b = 0; b < eq_bands; ++b) {
      auto& band = bands_[b];
      const EqType type = eq_type(p[b * eq_band_params]);
      band.octave.to(std::log2(log_map(p[b * eq_band_params + 1], eq_min_hz, eq_max_hz)), frames);
      band.db.to(lerp(p[b * eq_band_params + 2], -eq_max_db, eq_max_db), frames);
      band.log_q.to(std::log2(log_map(p[b * eq_band_params + 3], eq_min_q, eq_max_q)), frames);
      if (first) {
        band.type = type;
        band.now.type = type;
        band.fade.jump(1.0);
      } else if (type != band.type) {
        // The old response keeps its filter state and fades out under the new.
        band.was = band.now;
        band.type = type;
        band.fade.jump(0.0);
        band.fade.to(1.0, static_cast<std::uint32_t>(std::lround(eq_type_fade_seconds * rate_)));
      }
    }
    primed_ = true;
    tick_ = 0;
  }
  void process(Frames io) noexcept override {
    for (auto& frame : io) {
      const bool update = (tick_++ % coefficient_interval) == 0;
      StereoSample y = sample(frame);
      for (auto& band : bands_) {
        const double octave = band.octave.next();
        const double db = band.db.next();
        const double log_q = band.log_q.next();
        if (update) {
          const double g = prewarp(std::exp2(octave) / rate_);
          const double q = std::exp2(log_q);
          band.now.design(g, db, q, band.type);
          if (band.fade.moving()) {
            band.was.design(g, db, q, band.was.type);
          }
        }
        if (band.type == EqType::off && !band.fade.moving()) {
          continue;
        }
        const StereoSample now = band.now.process(y);
        if (band.fade.moving()) {
          const double w = band.fade.next();
          const StereoSample was = band.was.process(y);
          y = was + w * (now - was);
        } else {
          y = now;
        }
      }
      store(frame, y);
    }
    meter_.feed(io);
  }
  [[nodiscard]] float telemetry() const noexcept override { return meter_.db(); }
  [[nodiscard]] std::uint64_t tail_frames() const noexcept override {
    return static_cast<std::uint64_t>(std::ceil(0.05 * rate_));
  }

private:
  struct Band {
    EqType type = EqType::off;
    Glide octave;
    Glide db;
    Glide log_q;
    Glide fade;
    EqStage now;
    EqStage was;
  };
  void rate(double rate) noexcept {
    rate_ = rate;
    meter_.prepare(rate);
    tick_ = 0;
  }

  double base_;
  double rate_ = 0.0;
  std::uint32_t tick_ = 0;
  bool primed_ = false;
  std::array<Band, eq_bands> bands_{};
  PeakMeter meter_;
};

// ---- Utility ------------------------------------------------------------------------

// Gain, mid/side width, bass mono (the side high-passed by a fourth-order
// Linkwitz-Riley slope, so the mid is never touched) and a constant-power
// balance.
class UtilityModule final : public FxModule {
public:
  UtilityModule(double base_rate, std::size_t) : base_(base_rate) {
    rate(base_rate);
    set(fx_defaults(FxKind::utility).p, {2.0, base_rate}, 0);
  }
  [[nodiscard]] FxKind kind() const noexcept override { return FxKind::utility; }
  void reset() noexcept override {
    side_hp1_.reset();
    side_hp2_.reset();
    meter_.reset();
  }
  void set(const FxParams& p, const FxContext& context, std::uint32_t frames) noexcept override {
    const std::uint32_t factor = factor_for(context.sample_rate, base_);
    if (base_ * factor != rate_) {
      rate(base_ * factor);
    }
    gain_.to(db_to_gain(lerp(p[0], -24.0, 24.0)), frames);
    width_.to(2.0 * clamp01(p[1]), frames);
    const bool mono = clamp01(p[2]) > 0.0;
    if (mono) {
      mono_octave_.to(std::log2(log_map(p[2], 20.0, 500.0)), mono_on_ ? frames : 0U);
    }
    if (mono != mono_on_) {
      mono_amount_.to(mono ? 1.0 : 0.0, static_cast<std::uint32_t>(0.02 * rate_));
      mono_on_ = mono;
    }
    const double angle = (lerp(p[3], -1.0, 1.0) + 1.0) * 0.25 * std::numbers::pi;
    left_.to(std::numbers::sqrt2 * std::cos(angle), frames);
    right_.to(std::numbers::sqrt2 * std::sin(angle), frames);
  }
  void process(Frames io) noexcept override {
    for (auto& frame : io) {
      const double gain = gain_.next();
      const double width = width_.next();
      const double mono = mono_amount_.next();
      const double octave = mono_octave_.next();
      if ((tick_++ % coefficient_interval) == 0) {
        side_c_ = SvfCoefficients::make(prewarp(std::exp2(octave) / rate_), std::numbers::sqrt2);
      }
      const double left_gain = left_.next();
      const double right_gain = right_.next();
      const double mid = 0.5 * (static_cast<double>(frame.left) + frame.right);
      double side = 0.5 * (static_cast<double>(frame.left) - frame.right);
      const double high = side_hp2_.process(side_hp1_.process(side, side_c_).high, side_c_).high;
      side = (side + mono * (high - side)) * width;
      store(frame, {(mid + side) * left_gain * gain, (mid - side) * right_gain * gain});
    }
    meter_.feed(io);
  }
  [[nodiscard]] float telemetry() const noexcept override { return meter_.db(); }

private:
  void rate(double rate) noexcept {
    rate_ = rate;
    meter_.prepare(rate);
    tick_ = 0;
  }

  double base_;
  double rate_ = 0.0;
  std::uint32_t tick_ = 0;
  bool mono_on_ = false;
  Glide gain_;
  Glide width_;
  Glide mono_amount_;
  Glide mono_octave_;
  Glide left_;
  Glide right_;
  SvfCoefficients side_c_{};
  ZdfSvf side_hp1_;
  ZdfSvf side_hp2_;
  PeakMeter meter_;
};

} // namespace

std::unique_ptr<FxModule> make_compressor(double base_rate, std::size_t max_frames) {
  return std::make_unique<CompressorModule>(base_rate, max_frames);
}
std::unique_ptr<FxModule> make_multiband(double base_rate, std::size_t max_frames) {
  return std::make_unique<MultibandModule>(base_rate, max_frames);
}
FxParams eq_defaults() noexcept {
  constexpr std::array<EqType, eq_bands> types{EqType::low_shelf, EqType::bell, EqType::bell,
                                               EqType::high_shelf};
  constexpr std::array<double, eq_bands> hz{100.0, 400.0, 2'500.0, 8'000.0};
  constexpr std::array<double, eq_bands> q{butterworth2_q, 1.0, 1.0, butterworth2_q};
  FxParams p{};
  for (std::size_t b = 0; b < eq_bands; ++b) {
    p[b * eq_band_params] = eq_type_value(types[b]);
    p[b * eq_band_params + 1] = std::log(hz[b] / eq_min_hz) / std::log(eq_max_hz / eq_min_hz);
    p[b * eq_band_params + 2] = 0.5;
    p[b * eq_band_params + 3] = std::log(q[b] / eq_min_q) / std::log(eq_max_q / eq_min_q);
  }
  return p;
}

std::unique_ptr<FxModule> make_eq(double base_rate, std::size_t max_frames) {
  return std::make_unique<EqModule>(base_rate, max_frames);
}
std::unique_ptr<FxModule> make_utility(double base_rate, std::size_t max_frames) {
  return std::make_unique<UtilityModule>(base_rate, max_frames);
}

} // namespace oscillate::native_dsp::synth::nova::fx_detail

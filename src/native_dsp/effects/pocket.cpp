#include "oscillate/native_dsp/effects/pocket.hpp"

#include "dsp/crossover.hpp"
#include "effect_dsp.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"
#include "pocket_presets.hpp"
#include "pocket_table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

namespace oscillate::native_dsp::effects::pocket {
namespace {

using pocket_table::index_of;
using pocket_table::Mode;

namespace p {
constexpr std::size_t mode = index_of("pocket.mode");
constexpr std::size_t depth = index_of("pocket.depth");
constexpr std::size_t margin = index_of("pocket.margin");
constexpr std::size_t focus_low = index_of("pocket.focus_low");
constexpr std::size_t focus_high = index_of("pocket.focus_high");
constexpr std::size_t attack = index_of("pocket.attack");
constexpr std::size_t release = index_of("pocket.release");
constexpr std::size_t follow = index_of("pocket.follow");
constexpr std::size_t lookahead = index_of("pocket.lookahead");
constexpr std::size_t mono_sub = index_of("pocket.mono_sub");
constexpr std::size_t mono_freq = index_of("pocket.mono_freq");
constexpr std::size_t mix = index_of("pocket.mix");
constexpr std::size_t output = index_of("pocket.output");
} // namespace p

constexpr double lowest_hz = 30.0;
constexpr double highest_hz = 8'000.0;
// Third-octave bands: Q for a -3 dB overlap with the neighbours. Analysis
// cascades two band-passes (fourth order, 24 dB/octave skirts) whose joint
// -3 dB bandwidth is the third octave; each stage is wider by 1/0.644.
constexpr double band_q = 4.32;
constexpr double analysis_stage_q = band_q * 0.6436;
// The ducking bells are wider than the analysis bands (Q 3 against 4.32):
// a broad cut across many bands ripples under 0.6 dB between centres, and
// a lone cut reaches a third of its depth at the next centre.
constexpr double bell_q = 3.0;
// Their interaction matrix is measured at this cut. Every solve_frames the
// bells that are cutting are solved as one system (an active set: a bell
// that would have to boost is dropped and the rest solved again), then
// refined against the cascade's exact response from each bell's shape at
// its own gain (after Liski and Valimaki), so every targeted centre lands
// on its target. Gains are interpolated per sample in between.
constexpr double prototype_cut_db = 9.0;
constexpr std::size_t solve_frames = 32;
constexpr int solve_passes = 4;
// Band envelopes come from the filter's band and low outputs. For a tone at
// w the band output is (w / w0) times the low output's derivative, so
// low^2 + band^2 / r^2 is constant with r^2 = (w / w0)^2; r^2 is the ratio
// of the two outputs' powers, which drifts only as fast as the kick's pitch
// glides. That quadrature holds for any tone in the band, not just at the
// centre: a rippling envelope would modulate the bass. The power is then
// averaged over one period of the centre against whatever ripple is left
// (two tones in one band beat, as they should). Powers for the ratio pass
// two one-poles of two periods each.
constexpr double ratio_periods = 2.0;
// Unmask: cut by how far the bass sits above the kick plus the margin, but
// only while the depth can reveal the kick; past that the cut tapers off
// over 12 dB, so neither the start of a kick's rise nor its far tail draws
// a pointless cut. Below -60 dBFS the kick is absent.
constexpr double reveal_taper_db = 12.0;
constexpr double kick_floor_db = -60.0;
// Pump: one cut across the whole focus, following the kick's loudest band
// over the top 24 dB of its reference, the classic sidechain duck.
// Duck: the cut in each band follows the kick's level there over the top
// 24 dB of the kick's loudest band (its reference: rising over 10 ms so a
// filter's start-up cannot set it, released over a second). Bands where the
// kick has little energy duck little; nothing ducks below -50 dBFS.
constexpr double reference_attack_seconds = 0.01;
constexpr double reference_release_seconds = 1.0;
constexpr double presence_range_db = 24.0;
constexpr double duck_floor_db = -50.0;
// Follow Kick releases the cut as fast as the kick's own band envelope.
constexpr double follow_release_seconds = 0.015;
// Focus edges fall to nothing over half an octave.
constexpr double focus_edge_octaves = 0.5;
constexpr double meter_seconds = 0.3;
constexpr double toggle_seconds = native_dsp::dsp::switch_fade_seconds;
constexpr double max_lookahead_ms = 40.0;

[[nodiscard]] int as_int(double value) noexcept { return static_cast<int>(value); }
[[nodiscard]] double to_db(double amplitude) noexcept {
  return 20.0 * std::log10(amplitude + 1e-12);
}

// A zero-delay-feedback state-variable filter (Simper): its band output
// drives both the analysis band-pass and the ducking bell.
using Svf = synth::BasicZdfSvf<double>;
using synth::SvfCoefficients;

struct Band {
  double hz = 0.0;
  double g = 0.0; // tan(pi f / fs)
  SvfCoefficients analysis{};
  double ratio_coefficient = 0.0;
  // Per signal: smoothed band and low powers for the frequency ratio.
  std::array<double, 4> kick_powers{}; // band, low, then their second poles.
  std::array<double, 4> bass_powers{};
  // One period of the centre, for the power averages.
  std::vector<double> kick_power;
  std::vector<double> bass_power;
  std::size_t power_index = 0;
  double kick_sum = 0.0;
  double bass_sum = 0.0;
  std::array<Svf, 2> kick_filter;
  std::array<Svf, 2> bass_filter;
  std::array<Svf, 2> bells;
  double kick_level = 0.0; // Amplitudes.
  double bass_level = 0.0;
  double kick_reference = 0.0;
  double reduction_db = 0.0; // The target cut at this band's centre.
  double gain_db = 0.0;      // This bell's own cut, from the solve.
  double gain_step = 0.0;    // Per-sample move toward the next solve.
  double weight = 0.0;       // Focus.
  // The analysis's delay at the centre: each stage's band-pass delays its
  // envelope by 2Q / w0, and the smoothing by its time constant.
  double analysis_delay = 0.0; // Frames.
  // With lookahead, the target is held back by (lookahead - analysis delay)
  // so the cut lands with the audio it was measured on.
  std::vector<double> held;
  std::size_t held_write = 0;
  std::size_t hold = 0;

  void reset() noexcept {
    for (auto& stage : kick_filter) {
      stage.reset();
    }
    for (auto& stage : bass_filter) {
      stage.reset();
    }
    std::ranges::fill(kick_power, 0.0);
    std::ranges::fill(bass_power, 0.0);
    power_index = 0;
    kick_sum = 0.0;
    bass_sum = 0.0;
    kick_powers = {};
    bass_powers = {};
    for (auto& bell : bells) {
      bell.reset();
    }
    kick_level = 0.0;
    bass_level = 0.0;
    kick_reference = 0.0;
    reduction_db = 0.0;
    gain_db = 0.0;
    gain_step = 0.0;
    std::ranges::fill(held, 0.0);
    held_write = 0;
  }

  // One band's quadrature power through the two analysis stages, each
  // normalised to unity at the centre.
  [[nodiscard]] double power(std::array<Svf, 2>& stages, std::array<double, 4>& powers,
                             double x) const noexcept {
    const double first = stages[0].process(x, analysis).band / analysis_stage_q;
    const auto second = stages[1].process(first, analysis);
    const double band = second.band;
    const double low = second.low;
    powers[0] += ratio_coefficient * (band * band - powers[0]);
    powers[1] += ratio_coefficient * (low * low - powers[1]);
    powers[2] += ratio_coefficient * (powers[0] - powers[2]);
    powers[3] += ratio_coefficient * (powers[1] - powers[3]);
    // r^2 = (w / w0)^2, held to what the band can see (a third of an
    // octave each side): a young estimate cannot spike the envelope.
    const double ratio = powers[3] > 1e-30 ? std::clamp(powers[2] / powers[3], 0.25, 4.0) : 1.0;
    return (band * band / ratio + low * low) / (analysis_stage_q * analysis_stage_q);
  }

  // Both signals' levels: one-period averages of their quadrature power.
  // The running sums are rebuilt once a period so rounding never builds up.
  void measure(double kick, double bass) noexcept {
    const double k = power(kick_filter, kick_powers, kick);
    const double b = power(bass_filter, bass_powers, bass);
    kick_sum += k - kick_power[power_index];
    bass_sum += b - bass_power[power_index];
    kick_power[power_index] = k;
    bass_power[power_index] = b;
    if (++power_index == kick_power.size()) {
      power_index = 0;
      kick_sum = 0.0;
      bass_sum = 0.0;
      for (std::size_t i = 0; i < kick_power.size(); ++i) {
        kick_sum += kick_power[i];
        bass_sum += bass_power[i];
      }
    }
    const double n = static_cast<double>(kick_power.size());
    kick_level = std::sqrt(std::max(0.0, kick_sum / n));
    bass_level = std::sqrt(std::max(0.0, bass_sum / n));
  }
};

// When a band's envelope crosses half of a step: the two band-pass stages'
// group delays (2Q / w0 each) and half the averaging period, scaled by
// analysis_delay_scale as measured on tone bursts.
constexpr double analysis_delay_scale = 0.89;
[[nodiscard]] double analysis_delay_seconds(double hz) noexcept {
  return analysis_delay_scale *
         (2.0 * 2.0 * analysis_stage_q / (2.0 * std::numbers::pi * hz) + 0.5 / hz);
}

// |H| in dB of the Simper bell with gain a^2 at prewarped frequency ratio
// w = tan(pi f / fs) / tan(pi f0 / fs): (s^2 + (a/Q) s + 1) / (s^2 + s/(Q a) + 1).
[[nodiscard]] double bell_db_at(double w, double a) noexcept {
  const double real = 1.0 - w * w;
  const double numerator = std::hypot(real, w * a / bell_q);
  const double denominator = std::hypot(real, w / (bell_q * a));
  return 20.0 * std::log10(numerator / denominator);
}

// The Linkwitz-Riley mono-sub split for the side.
struct Split {
  synth::MultimodeFilter low_a;
  synth::MultimodeFilter low_b;
  synth::MultimodeFilter high_a;
  synth::MultimodeFilter high_b;
  void reset() noexcept {
    low_a.reset();
    low_b.reset();
    high_a.reset();
    high_b.reset();
  }
  [[nodiscard]] native_dsp::dsp::SplitSample process(double x, const synth::FilterCoefficients& lp,
                                                     const synth::FilterCoefficients& hp) noexcept {
    const double low = low_b.process(low_a.process(x, lp), lp);
    const double high = high_b.process(high_a.process(x, hp), hp);
    return {.low = low, .high = high};
  }
};

class PocketKernel final : public EffectKernel {
public:
  explicit PocketKernel(double sample_rate)
      : rate_(sample_rate),
        reference_attack_(1.0 - std::exp(-1.0 / (reference_attack_seconds * sample_rate))),
        reference_release_(1.0 - std::exp(-1.0 / (reference_release_seconds * sample_rate))),
        meter_(1.0 - std::exp(-1.0 / (meter_seconds * sample_rate))),
        toggle_step_(1.0 / (toggle_seconds * sample_rate)) {
    for (std::size_t b = 0; b < bands; ++b) {
      auto& band = bands_[b];
      band.hz = std::min(band_hz(b), native_dsp::dsp::nyquist_guard * sample_rate);
      band.g = std::tan(std::numbers::pi * band.hz / sample_rate);
      band.analysis = SvfCoefficients::make(band.g, 1.0 / analysis_stage_q);
      const auto period =
          std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(sample_rate / band.hz)));
      band.ratio_coefficient = 1.0 - std::exp(-1.0 / (ratio_periods / band.hz * sample_rate));
      band.kick_power.assign(period, 0.0);
      band.bass_power.assign(period, 0.0);
      band.analysis_delay = analysis_delay_seconds(band.hz) * sample_rate;
      band.held.assign(
          static_cast<std::size_t>(std::ceil(max_lookahead_ms * 1e-3 * sample_rate)) + 2, 0.0);
    }
    for (auto& line : lookahead_) {
      line.prepare(static_cast<std::size_t>(std::ceil(max_lookahead_ms * 1e-3 * sample_rate)) + 8);
    }
    kick_delay_.prepare(static_cast<std::size_t>(std::ceil(max_lookahead_ms * 1e-3 * sample_rate)) +
                        8);
    prepare_interaction();
    reset();
  }

  void control(std::span<const double> v, std::uint32_t frames,
               const Transport&) noexcept override {
    if (!primed_) {
      // Lookahead is the instance's fixed latency (EffectEngine::latency_frames).
      lookahead_frames_ = static_cast<std::size_t>(std::lround(
          pocket_table::lookahead_ms[static_cast<std::size_t>(v[p::lookahead])] * 1e-3 * rate_));
      for (auto& band : bands_) {
        const double spare = static_cast<double>(lookahead_frames_) - band.analysis_delay;
        band.hold = spare > 0.0 ? static_cast<std::size_t>(std::lround(spare)) : 0;
      }
    }
    mode_ = as_int(v[p::mode]);
    depth_db_ = v[p::depth];
    margin_db_ = v[p::margin];
    attack_ = 1.0 - std::exp(-1.0 / (v[p::attack] * 1e-3 * rate_));
    const double release_seconds =
        v[p::follow] != 0.0 ? follow_release_seconds : v[p::release] * 1e-3;
    release_ = 1.0 - std::exp(-1.0 / (release_seconds * rate_));
    const double low = v[p::focus_low];
    const double high = std::max(v[p::focus_high], low);
    for (auto& band : bands_) {
      double outside = 0.0;
      if (band.hz < low) {
        outside = std::log2(low / band.hz);
      } else if (band.hz > high) {
        outside = std::log2(band.hz / high);
      }
      band.weight = std::max(0.0, 1.0 - outside / focus_edge_octaves);
    }
    mono_target_ = v[p::mono_sub] != 0.0 ? 1.0 : 0.0;
    if (!primed_) {
      mono_ = mono_target_;
    }
    mono_lp_ = synth::make_filter_coefficients(synth::FilterMode::lowpass_12,
                                               v[p::mono_freq] / rate_, 0.0, 1.0);
    mono_hp_ = synth::make_filter_coefficients(synth::FilterMode::highpass_12,
                                               v[p::mono_freq] / rate_, 0.0, 1.0);
    const double steps = std::max<double>(1.0, frames);
    mix_.to(v[p::mix], steps, !primed_);
    gain_.to(dsp::db_to_gain(v[p::output]), steps, !primed_);
    primed_ = true;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame> sidechain) noexcept override {
    for (std::size_t i = 0; i < audio.size(); ++i) {
      const std::array<double, 2> x{audio[i].left, audio[i].right};
      const double kick = sidechain.empty() ? 0.0 : 0.5 * (sidechain[i].left + sidechain[i].right);
      const double bass = 0.5 * (x[0] + x[1]);

      // Delay the bass by the lookahead; analysis runs ahead of it. The
      // kick is delayed alike for the panel's waveforms only.
      std::array<double, 2> dry{};
      for (std::size_t c = 0; c < 2; ++c) {
        lookahead_[c].write(x[c]);
        dry[c] = lookahead_frames_ == 0 ? x[c] : lookahead_[c].sample(lookahead_frames_);
      }
      kick_delay_.write(kick);
      const double kick_shown =
          lookahead_frames_ == 0 ? kick : kick_delay_.sample(lookahead_frames_);

      // Analysis and each band's target cut.
      double loudest_reference = 0.0;
      double loudest_kick = 0.0;
      for (auto& band : bands_) {
        band.measure(kick, bass);
        band.kick_reference +=
            (band.kick_level > band.kick_reference ? reference_attack_ : reference_release_) *
            (band.kick_level - band.kick_reference);
        if (band.weight > 0.0) {
          loudest_reference = std::max(loudest_reference, band.kick_reference);
          loudest_kick = std::max(loudest_kick, band.kick_level);
        }
      }
      const double loudest_db = to_db(loudest_reference);
      const double pump_db = to_db(loudest_kick);
      const double pump =
          pump_db > duck_floor_db
              ? depth_db_ *
                    std::clamp((pump_db - (loudest_db - presence_range_db)) / presence_range_db,
                               0.0, 1.0)
              : 0.0;
      for (auto& band : bands_) {
        double need = 0.0;
        const double kick_db = to_db(band.kick_level);
        if (band.weight > 0.0) {
          if (mode_ == Mode::pump) {
            need = pump;
          } else if (mode_ == Mode::duck) {
            if (kick_db > duck_floor_db) {
              need = depth_db_ *
                     std::clamp((kick_db - (loudest_db - presence_range_db)) / presence_range_db,
                                0.0, 1.0);
            }
          } else if (kick_db > kick_floor_db) {
            const double deficit = to_db(band.bass_level) + margin_db_ - kick_db;
            if (deficit > depth_db_) {
              need =
                  depth_db_ * std::clamp(1.0 - (deficit - depth_db_) / reveal_taper_db, 0.0, 1.0);
            } else {
              need = std::max(0.0, deficit);
            }
          }
          need *= band.weight;
        }
        band.reduction_db +=
            (need > band.reduction_db ? attack_ : release_) * (need - band.reduction_db);
      }

      // Hold each target back to meet its audio.
      std::array<double, bands> target{};
      for (std::size_t b = 0; b < bands; ++b) {
        auto& band = bands_[b];
        const std::size_t size = band.held.size();
        band.held[band.held_write] = band.reduction_db;
        target[b] = band.held[(band.held_write + size - band.hold) % size];
        band.held_write = (band.held_write + 1) % size;
      }

      // Solve for the bell cuts whose combined response meets every band's
      // target at its centre (cuts only), every solve_frames.
      if (solve_count_ == 0) {
        solve(target);
      }
      solve_count_ = (solve_count_ + 1) % solve_frames;
      for (auto& band : bands_) {
        band.gain_db += band.gain_step;
      }

      // The bells: exactly the identity while a cut is zero.
      std::array<double, 2> y = dry;
      double overlap_before = 0.0;
      double overlap_after = 0.0;
      for (auto& band : bands_) {
        const double a = std::exp(-band.gain_db * std::numbers::ln10 / 40.0);
        const double bell_k = 1.0 / (bell_q * a);
        const auto bell = SvfCoefficients::make(band.g, bell_k);
        const double m1 = bell_k * (a * a - 1.0);
        for (std::size_t c = 0; c < 2; ++c) {
          y[c] += m1 * band.bells[c].process(y[c], bell).band;
        }
        if (band.weight > 0.0) {
          const double kick_power = band.kick_level * band.kick_level;
          const double bass_power = band.bass_level * band.bass_level;
          const double ducked = bass_power * std::pow(10.0, -band.reduction_db / 10.0);
          overlap_before += std::min(kick_power, bass_power);
          overlap_after += std::min(kick_power, ducked);
        }
      }
      overlap_before_ += meter_ * (overlap_before - overlap_before_);
      overlap_after_ += meter_ * (overlap_after - overlap_after_);

      // Mono sub on the side alone: the mid, which sums with the kick, is
      // never filtered, so the sub cannot phase against it. Crossfaded in
      // and out, so off is exactly the dry path.
      mono_ += std::clamp(mono_target_ - mono_, -toggle_step_, toggle_step_);
      const double mid = 0.5 * (y[0] + y[1]);
      const double side = 0.5 * (y[0] - y[1]);
      const auto side_split = side_split_.process(side, mono_lp_, mono_hp_);
      const double side_high = side_split.high;
      if (mono_ > 0.0) {
        const double s = side + (side_high - side) * mono_;
        y = {mid + s, mid - s};
      }

      const double mix = mix_.next();
      const double gain = gain_.next();
      audio[i].left = static_cast<float>((dry[0] + (y[0] - dry[0]) * mix) * gain);
      audio[i].right = static_cast<float>((dry[1] + (y[1] - dry[1]) * mix) * gain);
      record_wave(kick_shown, 0.5 * (dry[0] + dry[1]), 0.5 * (audio[i].left + audio[i].right));
    }
  }

  void reset() noexcept override {
    for (auto& band : bands_) {
      band.reset();
    }
    for (auto& line : lookahead_) {
      line.reset();
    }
    side_split_.reset();
    kick_delay_.reset();
    solve_count_ = 0;
    overlap_before_ = 0.0;
    overlap_after_ = 0.0;
    for (auto& wave : waves_) {
      wave.columns.fill({});
      wave.low = 0.0;
      wave.high = 0.0;
    }
    wave_write_ = 0;
    wave_count_ = 0;
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    for (std::size_t b = 0; b < bands; ++b) {
      const auto& band = bands_[b];
      out[telemetry::kick_db + b] = static_cast<float>(to_db(band.kick_level));
      out[telemetry::bass_db + b] = static_cast<float>(to_db(band.bass_level));
      out[telemetry::reduction_db + b] = static_cast<float>(-band.reduction_db);
    }
    out[telemetry::overlap_before_db] =
        static_cast<float>(10.0 * std::log10(overlap_before_ + 1e-24));
    out[telemetry::overlap_after_db] =
        static_cast<float>(10.0 * std::log10(overlap_after_ + 1e-24));
    const std::array<std::size_t, 3> offsets{telemetry::wave_kick, telemetry::wave_bass_in,
                                             telemetry::wave_bass_out};
    for (std::size_t w = 0; w < waves_.size(); ++w) {
      for (std::size_t i = 0; i < wave_points; ++i) {
        const auto& column = waves_[w].columns[(wave_write_ + i) % wave_points];
        out[offsets[w] + 2 * i] = column[0];
        out[offsets[w] + 2 * i + 1] = column[1];
      }
    }
    return telemetry::size;
  }

private:
  // The bell cuts for these targets: the active bells solved as one
  // system against the cascade's exact response, ramped to over
  // solve_frames.
  void solve(const std::array<double, bands>& target) noexcept {
    std::array<double, bands> cut{};
    std::array<std::size_t, bands> active{};
    std::size_t count = 0;
    for (std::size_t j = 0; j < bands; ++j) {
      if (target[j] > 1e-3) {
        active[count++] = j;
      }
    }
    for (int pass = 0; pass < solve_passes && count > 0; ++pass) {
      // The error at each active centre against the cascade as it stands.
      std::array<double, bands> error{};
      std::array<double, bands> a{};
      for (std::size_t j = 0; j < bands; ++j) {
        a[j] = std::exp(-cut[j] * std::numbers::ln10 / 40.0);
      }
      for (std::size_t r = 0; r < count; ++r) {
        const std::size_t i = active[r];
        double response = 0.0;
        for (std::size_t j = 0; j < bands; ++j) {
          if (cut[j] > 0.0) {
            response += bell_db_at(ratio_[i][j], a[j]);
          }
        }
        error[r] = target[i] + response; // The response is negative in dB.
      }
      // Solve the active system M_aa x = error (Gauss-Jordan with partial
      // pivoting; the matrix is diagonally dominant).
      std::array<std::array<double, bands + 1>, bands> m{};
      for (std::size_t r = 0; r < count; ++r) {
        for (std::size_t c = 0; c < count; ++c) {
          m[r][c] = interaction_[active[r]][active[c]];
        }
        m[r][count] = error[r];
      }
      for (std::size_t col = 0; col < count; ++col) {
        std::size_t pivot = col;
        for (std::size_t r = col + 1; r < count; ++r) {
          if (std::fabs(m[r][col]) > std::fabs(m[pivot][col])) {
            pivot = r;
          }
        }
        std::swap(m[col], m[pivot]);
        const double d = m[col][col];
        for (std::size_t c = col; c <= count; ++c) {
          m[col][c] /= d;
        }
        for (std::size_t r = 0; r < count; ++r) {
          if (r != col && m[r][col] != 0.0) {
            const double factor = m[r][col];
            for (std::size_t c = col; c <= count; ++c) {
              m[r][c] -= factor * m[col][c];
            }
          }
        }
      }
      // Step, and drop any bell that would have to boost.
      std::size_t kept = 0;
      for (std::size_t r = 0; r < count; ++r) {
        const std::size_t j = active[r];
        cut[j] += m[r][count];
        if (cut[j] > 0.0) {
          active[kept++] = j;
        } else {
          cut[j] = 0.0;
        }
      }
      count = kept;
    }
    for (std::size_t j = 0; j < bands; ++j) {
      bands_[j].gain_step = (cut[j] - bands_[j].gain_db) / static_cast<double>(solve_frames);
    }
  }

  struct Wave {
    std::array<std::array<float, 2>, wave_points> columns{};
    double low = 0.0;
    double high = 0.0;
  };

  // One sample into each waveform history's open column.
  void record_wave(double kick, double bass_in, double bass_out) noexcept {
    const std::array<double, 3> samples{kick, bass_in, bass_out};
    for (std::size_t w = 0; w < waves_.size(); ++w) {
      auto& wave = waves_[w];
      wave.low = wave_count_ == 0 ? samples[w] : std::min(wave.low, samples[w]);
      wave.high = wave_count_ == 0 ? samples[w] : std::max(wave.high, samples[w]);
    }
    if (++wave_count_ == wave_frames) {
      for (auto& wave : waves_) {
        wave.columns[wave_write_] = {static_cast<float>(wave.low), static_cast<float>(wave.high)};
      }
      wave_write_ = (wave_write_ + 1) % wave_points;
      wave_count_ = 0;
    }
  }

  // The dB cut each bell makes at every band centre per dB of its own cut,
  // at the prototype cut, and the prewarped frequency ratios for the
  // cascade's exact response.
  void prepare_interaction() noexcept {
    const double a = std::exp(-prototype_cut_db * std::numbers::ln10 / 40.0);
    for (std::size_t i = 0; i < bands; ++i) {
      for (std::size_t j = 0; j < bands; ++j) {
        ratio_[i][j] = std::tan(std::numbers::pi * std::min(bands_[i].hz, 0.49 * rate_) / rate_) /
                       std::tan(std::numbers::pi * bands_[j].hz / rate_);
        interaction_[i][j] = -bell_db_at(ratio_[i][j], a) / prototype_cut_db;
      }
    }
  }

  double rate_;
  std::array<std::array<double, bands>, bands> interaction_{};
  // Prewarped frequency ratio of centre i to bell j.
  std::array<std::array<double, bands>, bands> ratio_{};
  std::size_t solve_count_ = 0;
  double reference_attack_;
  double reference_release_;

  double meter_;
  double toggle_step_;
  std::array<Band, bands> bands_{};
  std::array<dsp::Delay, 2> lookahead_;
  std::size_t lookahead_frames_ = 0;
  int mode_ = Mode::unmask;
  double depth_db_ = 0.0;
  double margin_db_ = 0.0;
  double attack_ = 1.0;
  double release_ = 1.0;
  Split side_split_;
  synth::FilterCoefficients mono_lp_{};
  synth::FilterCoefficients mono_hp_{};
  double mono_ = 0.0;
  double mono_target_ = 0.0;
  dsp::Ramp mix_;
  dsp::Ramp gain_;
  double overlap_before_ = 0.0;
  double overlap_after_ = 0.0;
  dsp::Delay kick_delay_;
  std::array<Wave, 3> waves_{};
  std::size_t wave_write_ = 0;
  std::size_t wave_count_ = 0;
  bool primed_ = false;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double>, double sample_rate) {
  return std::make_unique<PocketKernel>(sample_rate);
}

// The lowest bell rings longest; the mono split and the release add a
// little.
std::uint64_t tail_frames(std::span<const double>, double sample_rate) {
  return static_cast<std::uint64_t>(std::ceil(0.3 * sample_rate));
}

std::uint32_t latency_frames(std::span<const double> v, double sample_rate) {
  return static_cast<std::uint32_t>(std::lround(
      pocket_table::lookahead_ms[static_cast<std::size_t>(v[p::lookahead])] * 1e-3 * sample_rate));
}

const EngineDescriptor descriptor{"pocket", "Pocket", 1, pocket_table::parameters,
                                  pocket_presets::presets};
const EffectEngine pocket_engine{&descriptor, &create_kernel, &tail_frames, &latency_frames, true};

} // namespace

double band_hz(std::size_t band) noexcept {
  return lowest_hz * std::pow(highest_hz / lowest_hz,
                              static_cast<double>(band) / static_cast<double>(bands - 1));
}

double band_delay_seconds(std::size_t band) noexcept {
  return analysis_delay_seconds(band_hz(band));
}

const EffectEngine& engine() { return pocket_engine; }

} // namespace oscillate::native_dsp::effects::pocket

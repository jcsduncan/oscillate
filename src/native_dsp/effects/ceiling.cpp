#include "oscillate/native_dsp/effects/ceiling.hpp"

#include "ceiling_dsp.hpp"
#include "ceiling_presets.hpp"
#include "ceiling_table.hpp"
#include "dsp/bs1770.hpp"
#include "edge_dsp.hpp"
#include "effect_dsp.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "reduction_analysis.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>

namespace oscillate::native_dsp::effects::ceiling {
namespace {

using ceiling_table::index_of;
namespace bs1770 = native_dsp::dsp::bs1770;

namespace p {
constexpr std::size_t gain = index_of("ceiling.gain");
constexpr std::size_t ceiling = index_of("ceiling.ceiling");
constexpr std::size_t lookahead = index_of("ceiling.lookahead");
constexpr std::size_t release = index_of("ceiling.release");
constexpr std::size_t character = index_of("ceiling.character");
constexpr std::size_t transients = index_of("ceiling.transients");
constexpr std::size_t clip = index_of("ceiling.clip");
constexpr std::size_t shape = index_of("ceiling.shape");
constexpr std::size_t link = index_of("ceiling.link");
constexpr std::size_t transient_link = index_of("ceiling.transient_link");
} // namespace p

// The meters hold for a second.
constexpr double hold_seconds = 1.0;
constexpr double hold_fall = 1e-3;
// The gain stage holds a peak's gain this long before releasing, so the
// release does not ripple at a low note's period (20 ms is a 25 Hz half
// cycle).
constexpr double hold_after_peak_seconds = 0.02;
// The slow (loudness) envelope's attack and release, as multiples of
// Release: short passages of reduction barely move it.
constexpr double slow_attack_ratio = 4.0;
constexpr double slow_release_ratio = 8.0;
constexpr double milliseconds_per_second = 1'000.0;
// The gain stage aims this far under the ceiling, so the guard (0.01 dB
// under it) is left only the clipper's and the filters' overshoot.
constexpr double stage_margin_db = 0.02;
// Shape's knee reaches its full width once the rooms add up to this.
constexpr double knee_full_room_db = 1.0;
// With any room, the knee starts at least 0.1 dB under the ceiling, so the
// clipper never flattens sustained material whose 8x peaks read a little
// over the gain stage's 4x estimate.
constexpr double minimum_knee = 0.0114;
// The loudness readings: 100 ms steps, gated over up to an hour.
constexpr double loudness_step_seconds = 0.1;
constexpr std::size_t loudness_blocks = 36'000;
constexpr double reading_floor_db = -200.0;

using native_dsp::dsp::gain_to_db;

[[nodiscard]] double finite_or_zero(float x) noexcept {
  return std::isfinite(x) ? static_cast<double>(x) : 0.0;
}

[[nodiscard]] double keep_for(double seconds, double sample_rate) noexcept {
  return std::exp(-1.0 / (seconds * sample_rate));
}

[[nodiscard]] std::size_t frames_for_ms(double ms, double sample_rate) noexcept {
  return std::max<std::size_t>(
      1, static_cast<std::size_t>(std::llround(ms * sample_rate / milliseconds_per_second)));
}

[[nodiscard]] double log_gain(double db) noexcept { return db * std::numbers::ln10 / 20.0; }

class CeilingKernel final : public EffectKernel {
public:
  // Control only.
  CeilingKernel(std::span<const double> values, double sample_rate)
      : rate_(sample_rate), hold_(std::exp(std::log(hold_fall) / (hold_seconds * sample_rate))),
        gate_(loudness_blocks) {
    stage_.prepare(frames_for_ms(values[p::lookahead], sample_rate),
                   frames_for_ms(hold_after_peak_seconds * milliseconds_per_second, sample_rate),
                   sample_rate);
    clipper_.prepare(sample_rate);
    guard_.prepare(sample_rate);
    audio_delay_.prepare(stage_.delay());
    latency_ = static_cast<std::uint32_t>(stage_.delay()) + clipper_.latency() + guard_.delay();
    ceiling_delay_.prepare(latency_);
    for (auto& k : weighting_) {
      k.prepare(sample_rate);
    }
    windows_.prepare(static_cast<std::size_t>(std::llround(loudness_step_seconds * sample_rate)));
    bands_.prepare(sample_rate);
    reset();
  }

  void control(std::span<const double> v, std::uint32_t frames,
               const Transport&) noexcept override {
    const bool jump = !primed_;
    const double steps = std::max<double>(1.0, frames);
    gain_.settle(dsp::db_to_gain(v[p::gain]), steps, jump);
    // The integrated loudness reads the output since the last Gain change,
    // so a second Learn sees only what the new gain gives.
    if (!jump && v[p::gain] != gain_db_) {
      gate_.clear();
      integrated_ = no_loudness;
    }
    gain_db_ = v[p::gain];
    ceiling_.settle(dsp::db_to_gain(v[p::ceiling]), steps, jump);
    const double release = v[p::release] / milliseconds_per_second;
    const double clip_room = log_gain(v[p::clip]);
    const double onset_room = log_gain(onset_room_db);
    // The clipper's knee: Shape's, faded in over the first dB of room and
    // bending no further under the ceiling than the room reaches over it.
    // Sustained material sits under the knee; with no room there is no
    // clipper, only the gain stage and the guard.
    const double room_db = v[p::clip] + v[p::transients] * onset_room_db;
    const double knee =
        room_db > 0.0
            ? std::max(minimum_knee, std::min(edge_dsp::max_knee * v[p::shape] *
                                                  std::min(1.0, room_db / knee_full_room_db),
                                              1.0 - std::exp(-log_gain(room_db))))
            : 0.0;
    clipper_.set(knee, room_db > 0.0, jump);
    stage_.set({.sustain = std::log1p(-knee),
                .onset_room = onset_room,
                .clip_room = clip_room - log_gain(stage_margin_db),
                .transients = v[p::transients],
                .fast_release = keep_for(release, rate_),
                .slow_attack = keep_for(slow_attack_ratio * release, rate_),
                .slow_release = keep_for(slow_release_ratio * release, rate_),
                .character = v[p::character],
                .link = v[p::link],
                .transient_link = v[p::transient_link]});
    if (jump) {
      ceiling_delay_.reset();
      fill_ceiling();
    }
    primed_ = true;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame>) noexcept override {
    double block_gain = 0.0;
    double block_clip = 1.0;
    double block_guard = 1.0;
    for (auto& frame : audio) {
      const double drive = gain_.next();
      const double ceiling = ceiling_.next();
      const std::array<double, 2> raw{finite_or_zero(frame.left), finite_or_zero(frame.right)};
      meter_input(raw);
      const std::array<double, 2> driven{raw[0] * drive, raw[1] * drive};
      detector_[detector_count_++] = {static_cast<float>(driven[0]), static_cast<float>(driven[1])};
      if (detector_count_ == detector_.size()) {
        bands_.analyse(detector_);
        detector_count_ = 0;
      }
      // Under a ceiling of 1 from here to the guard.
      const std::array<double, 2> unit{driven[0] / ceiling, driven[1] / ceiling};
      const auto gains = stage_.process(unit[0], unit[1]);
      const auto delayed = audio_delay_.push(unit);
      block_gain = std::min({block_gain, gains[0], gains[1]});
      double left = delayed[0] * std::exp(gains[0]);
      double right = delayed[1] * std::exp(gains[1]);
      block_clip = std::max(block_clip, clipper_.process(left, right));
      block_guard = std::min(block_guard, guard_.process(left, right));
      // Back to the ceiling this frame went in under.
      const double out_ceiling = ceiling_delay_.push({ceiling, 0.0})[0];
      left *= out_ceiling;
      right *= out_ceiling;
      meter_output(left, right);
      frame.left = static_cast<float>(left);
      frame.right = static_cast<float>(right);
    }
    if (detector_count_ > 0) {
      bands_.analyse(std::span(detector_).first(detector_count_));
      detector_count_ = 0;
    }
    const double frames = static_cast<double>(audio.size());
    const double decay = std::pow(hold_, frames);
    const double reduction_db = -gain_to_db(std::exp(block_gain));
    reduction_ = std::max(reduction_ * decay, reduction_db);
    clipped_ = std::max(clipped_ * decay, gain_to_db(block_clip));
    guarded_ = std::max(guarded_ * decay, -gain_to_db(block_guard));
    bands_.settle(reduction_db, audio.size());
  }

  void reset() noexcept override {
    stage_.reset();
    clipper_.reset();
    guard_.reset();
    audio_delay_.reset();
    fill_ceiling();
    for (auto& peak : out_peaks_) {
      peak.reset();
    }
    for (auto& k : weighting_) {
      k.reset();
    }
    windows_.reset();
    gate_.clear();
    integrated_ = no_loudness;
    input_peak_ = {};
    output_peak_ = {};
    reduction_ = 0.0;
    clipped_ = 0.0;
    guarded_ = 0.0;
    bands_.reset();
    detector_count_ = 0;
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    for (std::size_t c = 0; c < 2; ++c) {
      out[telemetry::input_peak + c] =
          static_cast<float>(gain_to_db(input_peak_[c], reading_floor_db));
      out[telemetry::output_peak + c] =
          static_cast<float>(gain_to_db(output_peak_[c], reading_floor_db));
    }
    out[telemetry::reduction] = static_cast<float>(reduction_);
    bands_.write(out.subspan(telemetry::reduction_bands, effects::reduction_bands::count));
    out[telemetry::clipped] = static_cast<float>(clipped_);
    out[telemetry::guarded] = static_cast<float>(guarded_);
    out[telemetry::momentary] = loudness(windows_.momentary());
    out[telemetry::short_term] = loudness(windows_.short_term());
    out[telemetry::integrated] = integrated_;
    out[telemetry::latency_ms] =
        static_cast<float>(static_cast<double>(latency_) / rate_ * milliseconds_per_second);
    return telemetry::size;
  }

  [[nodiscard]] std::uint32_t latency() const noexcept { return latency_; }

private:
  [[nodiscard]] static float loudness(double power) noexcept {
    return power > 0.0
               ? static_cast<float>(std::max(bs1770::lkfs(power), static_cast<double>(no_loudness)))
               : no_loudness;
  }

  void fill_ceiling() noexcept {
    for (std::uint32_t i = 0; i <= latency_; ++i) {
      (void)ceiling_delay_.push({ceiling_.value, 0.0});
    }
  }

  void meter_input(const std::array<double, 2>& x) noexcept {
    for (std::size_t c = 0; c < 2; ++c) {
      input_peak_[c] = std::max(input_peak_[c] * hold_, std::fabs(x[c]));
    }
  }

  void meter_output(double left, double right) noexcept {
    const std::array<double, 2> y{left, right};
    double power = 0.0;
    for (std::size_t c = 0; c < 2; ++c) {
      const double peak = std::max(out_peaks_[c].push(y[c]), std::fabs(y[c]));
      output_peak_[c] = std::max(output_peak_[c] * hold_, peak);
      const double k = weighting_[c].process(y[c]);
      power += k * k;
    }
    if (windows_.add(power) && windows_.steps() >= bs1770::momentary_steps) {
      if (gate_.add(windows_.momentary())) {
        const auto gated = gate_.integrated();
        integrated_ = gated.integrated_lkfs
                          ? static_cast<float>(
                                std::max(*gated.integrated_lkfs, static_cast<double>(no_loudness)))
                          : no_loudness;
      }
    }
  }

  double rate_;
  double hold_;
  dsp::Ramp gain_;
  dsp::Ramp ceiling_;
  ceiling_dsp::GainStage stage_;
  ceiling_dsp::FrameDelay audio_delay_;
  ceiling_dsp::FrameDelay ceiling_delay_;
  ceiling_dsp::Clipper clipper_;
  edge_dsp::TruePeakGuard guard_;
  std::uint32_t latency_ = 0;
  std::array<bs1770::TruePeak, 2> out_peaks_{};
  std::array<bs1770::KWeighting, 2> weighting_{};
  bs1770::StepWindows windows_;
  bs1770::Gate gate_;
  float integrated_ = no_loudness;
  double gain_db_ = 0.0;
  std::array<double, 2> input_peak_{};
  std::array<double, 2> output_peak_{};
  double reduction_ = 0.0;
  double clipped_ = 0.0;
  double guarded_ = 0.0;
  dsp::ReductionBands bands_;
  // The driven input in chunks for the reduction bands.
  static constexpr std::size_t detector_chunk = 64;
  std::array<engine::StereoFrame, detector_chunk> detector_{};
  std::size_t detector_count_ = 0;
  bool primed_ = false;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double> values, double sample_rate) {
  if (!(sample_rate > 0.0)) {
    return nullptr;
  }
  return std::make_unique<CeilingKernel>(values, sample_rate);
}

// With room to clip, the clipper's filters ring a frame past their round
// trip, as Edge's do; with none it is a plain delay and stops with its
// input.
std::uint64_t tail_frames(std::span<const double> values, double) {
  const bool room = values[p::clip] > 0.0 || values[p::transients] > 0.0;
  return room ? edge_dsp::design(ceiling_dsp::clip_oversampling, 0).tail : 0;
}

std::uint32_t engine_latency(std::span<const double> values, double sample_rate) {
  return latency_frames(values[p::lookahead], sample_rate);
}

const EngineDescriptor descriptor{"ceiling", "Ceiling", 1, ceiling_table::parameters,
                                  ceiling_presets::presets};
const EffectEngine ceiling_engine{&descriptor, &create_kernel, &tail_frames, &engine_latency,
                                  false};

} // namespace

std::uint32_t latency_frames(double lookahead_ms, double sample_rate) {
  const std::size_t attack = frames_for_ms(lookahead_ms, sample_rate);
  return static_cast<std::uint32_t>(ceiling_dsp::GainStage::peak_radius + attack - 1) +
         edge_dsp::design(ceiling_dsp::clip_oversampling, 0).latency +
         edge_dsp::TruePeakGuard::delay_at(sample_rate);
}

const EffectEngine& engine() { return ceiling_engine; }

} // namespace oscillate::native_dsp::effects::ceiling

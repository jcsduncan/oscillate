#include "oscillate/native_dsp/effects/loudness.hpp"

#include "capture_queue.hpp"
#include "dsp/bs1770.hpp"
#include "loudness_presets.hpp"
#include "loudness_table.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

namespace oscillate::native_dsp::effects::loudness {
namespace {

using loudness_table::index_of;

namespace p {
constexpr std::size_t integrate = index_of("loudness.integrate");
} // namespace p

// BS.1770 (the project's one core, shared with export and the master
// meters): loudness is -0.691 + 10 log10 of the summed channel mean squares
// (left and right weighted 1).
namespace bs1770 = native_dsp::dsp::bs1770;
constexpr double offset_lufs = bs1770::offset_lkfs;
constexpr double absolute_gate = bs1770::absolute_gate_lkfs;
constexpr double relative_gate = bs1770::relative_gate_lu; // Integrated.
constexpr double range_relative_gate = -20.0;              // Loudness range.
constexpr std::size_t short_term_blocks = bs1770::short_term_steps;
// Integrated loudness is gated exactly over the first two hours of blocks;
// past that the 0.01 LU histogram takes over.
constexpr std::size_t exact_gate_blocks = 72'000;
// Histograms of 400 ms block (and 3 s short-term) loudness from the
// absolute gate up, in 0.01 LU bins with each bin's exact energy sum.
constexpr double bin_lu = 0.01;
constexpr std::size_t bin_count = 8'000; // -70 to +10 LUFS.
// The meter analyses a copy of its input off the callback (ADR-0129); the
// copy waits this long at most for the analysis.
constexpr double capture_seconds = 2.0;

// One frame as the callback captures it: the audio, whether Integrate is
// on, and whether the statistics start afresh from it (Integrate switched).
struct Captured {
  float left = 0.0F;
  float right = 0.0F;
  bool integrating = true;
  bool fresh_statistics = false;
};

[[nodiscard]] double to_lufs(double energy) noexcept {
  return energy > 0.0 ? offset_lufs + 10.0 * std::log10(energy) : silent;
}
[[nodiscard]] double to_db(double amplitude) noexcept {
  return native_dsp::dsp::gain_to_db(amplitude, silent);
}

struct Histogram {
  std::vector<double> counts = std::vector<double>(bin_count, 0.0);
  std::vector<double> energies = std::vector<double>(bin_count, 0.0);
  double count = 0.0;
  double energy = 0.0;

  void clear() noexcept {
    std::ranges::fill(counts, 0.0);
    std::ranges::fill(energies, 0.0);
    count = 0.0;
    energy = 0.0;
  }
  void add(double block_energy) noexcept {
    const double lufs = to_lufs(block_energy);
    if (lufs <= absolute_gate) {
      return;
    }
    const auto bin =
        std::min(bin_count - 1, static_cast<std::size_t>((lufs - absolute_gate) / bin_lu));
    counts[bin] += 1.0;
    energies[bin] += block_energy;
    count += 1.0;
    energy += block_energy;
  }
  // The first bin at or above `lufs`.
  [[nodiscard]] static std::size_t bin_of(double lufs) noexcept {
    if (lufs <= absolute_gate) {
      return 0;
    }
    return std::min(bin_count,
                    static_cast<std::size_t>(std::ceil((lufs - absolute_gate) / bin_lu)));
  }
};

// The callback passes the audio through and captures a copy; the meter
// analyses it off the callback, on the reader or the analysis worker.
class LoudnessKernel final : public EffectKernel {
public:
  explicit LoudnessKernel(double sample_rate)
      : rate_(sample_rate), gate_(exact_gate_blocks),
        queue_(static_cast<std::size_t>(std::ceil(capture_seconds * sample_rate))) {
    for (auto& filter : weighting_) {
      filter.prepare(sample_rate);
    }
    windows_.prepare(static_cast<std::size_t>(std::lround(sample_rate / 10.0)));
    restart();
    clear_statistics();
  }

  void control(std::span<const double> v, std::uint32_t, const Transport&) noexcept override {
    // Switching Integrate either way starts the statistics afresh.
    const bool integrate = v[p::integrate] != 0.0;
    if (primed_ && integrate != integrating_) {
      fresh_statistics_ = true;
    }
    integrating_ = integrate;
    primed_ = true;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame>) noexcept override {
    if (queue_.write(audio.size(), [&](std::size_t i) {
          return Captured{audio[i].left, audio[i].right, integrating_, fresh_statistics_ && i == 0};
        })) {
      fresh_statistics_ = false;
    }
  }

  // A seek starts the windows and filters afresh; the integrated
  // statistics carry on until Integrate is switched off.
  void reset() noexcept override { queue_.restart(); }

  // The integrated statistics need every frame, so the analysis worker
  // drains the capture when no panel reads.
  [[nodiscard]] TelemetryAnalysis analysis() const noexcept override {
    return TelemetryAnalysis::continuous;
  }

  [[nodiscard]] bool analysis_backlogged() const noexcept override { return queue_.backlogged(); }

  void analyse() noexcept override {
    queue_.read([this] { restart(); }, [this](const Captured& frame) { measure(frame); });
  }

  // The reader: the meter publishes nothing from the callback.
  [[nodiscard]] std::size_t analysed_telemetry(std::span<float> out,
                                               std::size_t) noexcept override {
    if (out.size() < telemetry::size || !captured_) {
      return 0;
    }
    const auto put = [&](std::size_t at, double value) { out[at] = static_cast<float>(value); };
    put(telemetry::momentary, momentary_);
    put(telemetry::short_term, short_term_);
    put(telemetry::integrated, integrated_);
    put(telemetry::range, range_);
    put(telemetry::momentary_max, momentary_max_);
    put(telemetry::short_term_max, short_term_max_);
    put(telemetry::true_peak, to_db(std::max(true_peak_[0], true_peak_[1])));
    put(telemetry::true_peak_left, to_db(true_peak_[0]));
    put(telemetry::true_peak_right, to_db(true_peak_[1]));
    double recent = 0.0;
    for (const double peak : recent_peaks_) {
      recent = std::max(recent, peak);
    }
    put(telemetry::true_peak_recent, to_db(recent));
    put(telemetry::sample_peak, to_db(sample_peak_));
    put(telemetry::psr, short_term_ > silent && recent > 0.0 ? to_db(recent) - short_term_ : 0.0);
    put(telemetry::plr, integrated_ > silent && std::max(true_peak_[0], true_peak_[1]) > 0.0
                            ? to_db(std::max(true_peak_[0], true_peak_[1])) - integrated_
                            : 0.0);
    put(telemetry::duration, static_cast<double>(integrated_blocks_) * 0.1);
    for (std::size_t i = 0; i < history; ++i) {
      const std::size_t at = (history_write_ + i) % history;
      put(telemetry::momentary_history + i, momentary_history_[at]);
      put(telemetry::short_term_history + i, short_term_history_[at]);
    }
    return telemetry::size;
  }

private:
  // One captured frame through the meter.
  void measure(const Captured& frame) noexcept {
    captured_ = true;
    if (frame.fresh_statistics) {
      clear_statistics();
    }
    integrating_now_ = frame.integrating;
    const std::array<double, 2> x{frame.left, frame.right};
    double power = 0.0;
    for (std::size_t c = 0; c < 2; ++c) {
      const double k = weighting_[c].process(x[c]);
      power += 1.0 * k * k;
      // True peak: the interpolated points around the sample `radius` back.
      const double top = std::max(std::fabs(x[c]), peak_[c].push(x[c]));
      block_true_peak_[c] = std::max(block_true_peak_[c], top);
      sample_peak_ = std::max(sample_peak_, std::fabs(x[c]));
    }
    if (windows_.add(power)) {
      finish_sub_block();
    }
  }

  // A new run (a seek, or a capture the analysis fell behind on).
  void restart() noexcept {
    for (std::size_t c = 0; c < 2; ++c) {
      weighting_[c].reset();
      peak_[c].reset();
      block_true_peak_[c] = 0.0;
    }
    windows_.reset();
    recent_peaks_.fill(0.0);
    momentary_ = silent;
    short_term_ = silent;
  }

  void clear_statistics() noexcept {
    gate_.clear();
    blocks_histogram_.clear();
    short_term_histogram_.clear();
    integrated_ = silent;
    range_ = 0.0;
    momentary_max_ = silent;
    short_term_max_ = silent;
    true_peak_ = {};
    sample_peak_ = 0.0;
    integrated_blocks_ = 0;
    momentary_history_.fill(static_cast<float>(silent));
    short_term_history_.fill(static_cast<float>(silent));
  }

  void finish_sub_block() noexcept {
    const std::uint64_t blocks = windows_.steps();
    const double block_peak = std::max(block_true_peak_[0], block_true_peak_[1]);
    recent_peaks_[(blocks - 1) % short_term_blocks] = block_peak;
    for (std::size_t c = 0; c < 2; ++c) {
      true_peak_[c] = std::max(true_peak_[c], block_true_peak_[c]);
      block_true_peak_[c] = 0.0;
    }

    if (blocks >= bs1770::momentary_steps) {
      const double block = windows_.momentary();
      momentary_ = to_lufs(block);
      if (integrating_now_) {
        momentary_max_ = std::max(momentary_max_, momentary_);
        blocks_histogram_.add(block);
        ++integrated_blocks_;
        if (gate_.add(block)) {
          const auto gated_exactly = gate_.integrated().integrated_lkfs;
          integrated_ = gated_exactly ? *gated_exactly : silent;
        } else {
          integrated_ = gated(blocks_histogram_, relative_gate);
        }
      }
    }
    if (blocks >= short_term_blocks) {
      const double block = windows_.short_term();
      short_term_ = to_lufs(block);
      if (integrating_now_) {
        short_term_max_ = std::max(short_term_max_, short_term_);
        short_term_histogram_.add(block);
        range_ = loudness_range();
      }
    }
    momentary_history_[history_write_] = static_cast<float>(momentary_);
    short_term_history_[history_write_] = static_cast<float>(short_term_);
    history_write_ = (history_write_ + 1) % history;
  }

  // Integrated loudness: the mean energy of the blocks above the relative
  // gate, itself 10 LU under the mean of the blocks above the absolute gate.
  [[nodiscard]] static double gated(const Histogram& h, double relative) noexcept {
    if (h.count <= 0.0) {
      return silent;
    }
    const double threshold = to_lufs(h.energy / h.count) + relative;
    double count = 0.0;
    double energy = 0.0;
    for (std::size_t b = Histogram::bin_of(threshold); b < bin_count; ++b) {
      count += h.counts[b];
      energy += h.energies[b];
    }
    return count > 0.0 ? to_lufs(energy / count) : silent;
  }

  // EBU Tech 3342: short-term values above the absolute gate and 20 LU
  // under their mean; the spread between their 10th and 95th percentiles.
  [[nodiscard]] double loudness_range() const noexcept {
    const auto& h = short_term_histogram_;
    if (h.count <= 0.0) {
      return 0.0;
    }
    const double threshold = to_lufs(h.energy / h.count) + range_relative_gate;
    const std::size_t from = Histogram::bin_of(threshold);
    double count = 0.0;
    for (std::size_t b = from; b < bin_count; ++b) {
      count += h.counts[b];
    }
    if (count <= 0.0) {
      return 0.0;
    }
    const auto percentile = [&](double fraction) {
      const double target = fraction * count;
      double seen = 0.0;
      for (std::size_t b = from; b < bin_count; ++b) {
        seen += h.counts[b];
        if (seen >= target) {
          return absolute_gate + (static_cast<double>(b) + 0.5) * bin_lu;
        }
      }
      return absolute_gate + static_cast<double>(bin_count) * bin_lu;
    };
    return percentile(0.95) - percentile(0.10);
  }

  double rate_;
  std::array<bs1770::KWeighting, 2> weighting_{};
  bs1770::StepWindows windows_;
  bs1770::Gate gate_;
  std::array<bs1770::TruePeak, 2> peak_{};
  std::array<double, 2> block_true_peak_{};
  std::array<double, 2> true_peak_{};
  double sample_peak_ = 0.0;
  std::array<double, short_term_blocks> recent_peaks_{};
  Histogram blocks_histogram_;
  Histogram short_term_histogram_;
  double momentary_ = silent;
  double short_term_ = silent;
  double integrated_ = silent;
  double range_ = 0.0;
  double momentary_max_ = silent;
  double short_term_max_ = silent;
  std::uint64_t integrated_blocks_ = 0;
  std::array<float, history> momentary_history_{};
  std::array<float, history> short_term_history_{};
  std::size_t history_write_ = 0;
  bool integrating_now_ = true;
  bool captured_ = false;
  // The callback's.
  CaptureQueue<Captured> queue_;
  bool integrating_ = true;
  bool fresh_statistics_ = false;
  bool primed_ = false;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double>, double sample_rate) {
  return std::make_unique<LoudnessKernel>(sample_rate);
}

// The meter passes audio through unchanged.
std::uint64_t tail_frames(std::span<const double>, double) { return 1; }
std::uint32_t latency_frames(std::span<const double>, double) { return 0; }

const EngineDescriptor descriptor{"loudness", "Loudness", 1, loudness_table::parameters,
                                  loudness_presets::presets};
const EffectEngine loudness_engine{&descriptor, &create_kernel, &tail_frames, &latency_frames,
                                   false};

} // namespace

const EffectEngine& engine() { return loudness_engine; }

} // namespace oscillate::native_dsp::effects::loudness

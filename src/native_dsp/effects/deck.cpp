#include "oscillate/native_dsp/effects/deck.hpp"

#include "deck_presets.hpp"
#include "deck_table.hpp"
#include "dsp/crossover.hpp"
#include "effect_dsp.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace oscillate::native_dsp::effects::deck {
namespace {

using deck_table::index_of;

namespace p {
constexpr std::size_t low = index_of("deck.low");
constexpr std::size_t mid = index_of("deck.mid");
constexpr std::size_t high = index_of("deck.high");
constexpr std::size_t low_kill = index_of("deck.low_kill");
constexpr std::size_t mid_kill = index_of("deck.mid_kill");
constexpr std::size_t high_kill = index_of("deck.high_kill");
constexpr std::size_t low_x = index_of("deck.low_x");
constexpr std::size_t high_x = index_of("deck.high_x");
constexpr std::size_t gain = index_of("deck.gain");
} // namespace p

constexpr std::size_t bands = 3;
// A kill switch fades its band over this long: fast as a mixer's button,
// slow enough never to click.
constexpr double kill_seconds = 0.01;
// The crossovers stay at least an octave apart and below 0.45 of the rate.
constexpr double crossover_spacing = 2.0;
constexpr double peak_hold_seconds = 1.0;
constexpr double rms_seconds = 0.3;

using native_dsp::dsp::gain_to_db;

// A fourth-order Linkwitz-Riley split: two Butterworth sections each way.
// Low plus high is a second-order allpass, the same for every input.
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

struct Crossover {
  synth::FilterCoefficients lp{};
  synth::FilterCoefficients hp{};
  double hz = 0.0;
  void set(double next, double rate) noexcept {
    if (next == hz)
      return;
    hz = next;
    lp = synth::make_filter_coefficients(synth::FilterMode::lowpass_12, hz / rate, 0.0, 1.0);
    hp = synth::make_filter_coefficients(synth::FilterMode::highpass_12, hz / rate, 0.0, 1.0);
  }
};

// One channel's band tree: low at the low crossover, behind the high
// crossover's allpass; mid and high at the high crossover, behind the low
// one's highpass. Every band carries both allpasses, so they sum flat.
struct Tree {
  Split low;
  Split low_comp;
  Split high;
  void reset() noexcept {
    low.reset();
    low_comp.reset();
    high.reset();
  }
};

// A weight gliding to its target at a fixed rate; exactly at rest when there.
struct Glide {
  double value = 1.0;
  double target = 1.0;
  double step = 1.0;
  void to(double goal, bool jump) noexcept {
    target = goal;
    if (jump)
      value = goal;
  }
  double next() noexcept { return value += std::clamp(target - value, -step, step); }
};

class DeckKernel final : public EffectKernel {
public:
  explicit DeckKernel(double sample_rate)
      : rate_(sample_rate), hold_(std::exp(std::log(1e-3) / (peak_hold_seconds * sample_rate))),
        rms_(1.0 - std::exp(-1.0 / (rms_seconds * sample_rate))) {
    for (auto& kill : kills_)
      kill.step = 1.0 / (kill_seconds * sample_rate);
  }

  void control(std::span<const double> v, std::uint32_t frames,
               const Transport&) noexcept override {
    const bool jump = !primed_;
    const double steps = std::max<double>(1.0, frames);
    const double top = native_dsp::dsp::nyquist_guard * rate_;
    const double high_x = std::min(v[p::high_x], top);
    const double low_x = std::min(v[p::low_x], high_x / crossover_spacing);
    low_.set(low_x, rate_);
    high_.set(high_x, rate_);
    const std::array levels{v[p::low], v[p::mid], v[p::high]};
    const std::array killed{v[p::low_kill], v[p::mid_kill], v[p::high_kill]};
    for (std::size_t band = 0; band < bands; ++band) {
      // The bottom stop is a full kill.
      const double level =
          levels[band] <= deck_table::kill_db ? 0.0 : dsp::db_to_gain(levels[band]);
      levels_[band].settle(level, steps, jump);
      kills_[band].to(killed[band] != 0.0 ? 0.0 : 1.0, jump);
    }
    gain_.settle(dsp::db_to_gain(v[p::gain]), steps, jump);
    primed_ = true;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame>) noexcept override {
    for (auto& frame : audio) {
      std::array<double, bands> weights{};
      for (std::size_t band = 0; band < bands; ++band)
        weights[band] = levels_[band].next() * kills_[band].next();
      const double gain = gain_.next();
      std::array<double, 2> x{frame.left, frame.right};
      std::array<double, bands> power{};
      for (std::size_t c = 0; c < 2; ++c) {
        auto& tree = trees_[c];
        const auto split = tree.low.process(x[c], low_.lp, low_.hp);
        const auto low = tree.low_comp.process(split.low, high_.lp, high_.hp);
        const auto high = tree.high.process(split.high, high_.lp, high_.hp);
        std::array<double, bands> band{low.low + low.high, high.low, high.high};
        double sum = 0.0;
        for (std::size_t k = 0; k < bands; ++k) {
          const double shaped = band[k] * weights[k];
          power[k] += 0.5 * shaped * shaped;
          sum += shaped;
        }
        x[c] = sum * gain;
        peak_[c] = std::max(std::fabs(x[c]), peak_[c] * hold_);
      }
      for (std::size_t k = 0; k < bands; ++k)
        band_power_[k] += rms_ * (power[k] - band_power_[k]);
      frame.left = static_cast<float>(x[0]);
      frame.right = static_cast<float>(x[1]);
    }
  }

  void reset() noexcept override {
    for (auto& tree : trees_)
      tree.reset();
    band_power_ = {};
    peak_ = {};
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size)
      return 0;
    for (std::size_t k = 0; k < bands; ++k)
      out[telemetry::band_rms + k] = static_cast<float>(gain_to_db(std::sqrt(band_power_[k])));
    for (std::size_t c = 0; c < 2; ++c)
      out[telemetry::output_peak + c] = static_cast<float>(gain_to_db(peak_[c]));
    return telemetry::size;
  }

private:
  double rate_;
  double hold_;
  double rms_;
  Crossover low_{};
  Crossover high_{};
  std::array<Tree, 2> trees_{};
  std::array<dsp::Ramp, bands> levels_{};
  std::array<Glide, bands> kills_{};
  dsp::Ramp gain_;
  std::array<double, bands> band_power_{};
  std::array<double, 2> peak_{};
  bool primed_ = false;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double>, double sample_rate) {
  return std::make_unique<DeckKernel>(sample_rate);
}

// The lowest crossover's ring dominates: 60 dB well inside 50 ms at 60 Hz.
std::uint64_t tail_frames(std::span<const double>, double sample_rate) {
  return static_cast<std::uint64_t>(std::ceil(0.05 * sample_rate));
}
std::uint32_t latency_frames(std::span<const double>, double) { return 0; }

const EngineDescriptor descriptor{"deck", "Deck", 1, deck_table::parameters, deck_presets::presets};
const EffectEngine deck_engine{&descriptor, &create_kernel, &tail_frames, &latency_frames, false};

} // namespace

const EffectEngine& engine() { return deck_engine; }

} // namespace oscillate::native_dsp::effects::deck
